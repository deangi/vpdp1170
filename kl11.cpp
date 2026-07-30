// Host console bridge for vpdp1170 (kek path).
//
// Guest KL11/TTY CSRs live in kek_src_tty.cpp. This file owns the host-side
// FIFOs, serialdelay gate, VPDP escape-channel parser, and USB-Serial drain.
#include "kl11.h"

#include "console.h"
#include "emu_control.h"
#include "fifo.h"
#include "platform.h"
#include "telnet.h"

#include <Arduino.h>
#include <atomic>
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>


namespace kl11 {

static uint32_t console_trace_count = 0;

void set_console_trace(uint32_t count) {
  console_trace_count = count;
}

uint32_t console_trace_remaining() {
  return console_trace_count;
}

void charge_console_trace(const char* direction, uint8_t value) {
  if (console_trace_count == 0) return;
  console_trace_count--;

  char display[8];
  if (value >= 0x20 && value <= 0x7e) {
    display[0] = '\'';
    display[1] = (char)value;
    display[2] = '\'';
    display[3] = '\0';
  } else {
    snprintf(display, sizeof(display), "^%c",
             value < 0x20 ? (char)(value + '@') : '?');
  }

  LOG("CONSOLE %s char=%03o %s remaining=%u",
      direction, (unsigned)value, display, (unsigned)console_trace_count);
}

#define VPDP_KL11_FIFO_BYTES 131072
static uint8_t serial_out_fallback[8192];
static uint8_t* serial_out_storage = nullptr;
static size_t serial_out_capacity = 0;
static Fifo g_serial_out;
static bool g_serial_out_inited = false;
static TaskHandle_t g_serial_output_task = nullptr;
static std::atomic<uint32_t> g_serial_dropped { 0 };

#define VPDP_CONTROL_REPLY_BYTES 1024
static uint8_t control_reply_storage[VPDP_CONTROL_REPLY_BYTES];
static Fifo g_control_reply;
static bool g_control_reply_inited = false;

static const uint8_t CONTROL_PREFIX[] = { 033, ']', 'V', 'P', 'D', 'P', ';' };
static constexpr size_t CONTROL_PREFIX_LEN = sizeof(CONTROL_PREFIX);
static constexpr uint8_t CONTROL_PREFIX_BASIC = '$';
static constexpr size_t CONTROL_COMMAND_MAX = 256;
enum ControlParseState {
  CONTROL_IDLE,
  CONTROL_PREFIX_MATCH,
  CONTROL_TEXT
};
static ControlParseState control_state = CONTROL_IDLE;
static uint8_t control_prefix_pos = 0;
static uint8_t control_prefix_first = 0;
static char control_command[CONTROL_COMMAND_MAX + 1];
static size_t control_command_len = 0;

uint32_t serial_in_delay_ms = 0;

static void ensure_serial_out() {
  if (g_serial_out_inited) return;
  serial_out_storage = static_cast<uint8_t*>(
      heap_caps_malloc(VPDP_KL11_FIFO_BYTES,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (serial_out_storage) {
    serial_out_capacity = VPDP_KL11_FIFO_BYTES;
    LOG("console USB FIFO: %u KB PSRAM",
        (unsigned)(serial_out_capacity / 1024));
  } else {
    serial_out_storage = serial_out_fallback;
    serial_out_capacity = sizeof(serial_out_fallback);
    LOGE("console USB FIFO: PSRAM allocation failed; using %u KB DRAM",
         (unsigned)(serial_out_capacity / 1024));
  }
  g_serial_out.init(serial_out_storage, serial_out_capacity);
  g_serial_out_inited = true;
}

static void ensure_control_reply() {
  if (!g_control_reply_inited) {
    g_control_reply.init(control_reply_storage, VPDP_CONTROL_REPLY_BYTES);
    g_control_reply_inited = true;
  }
}

void reset() {
  ensure_serial_out();
  ensure_control_reply();
  control_state = CONTROL_IDLE;
  control_prefix_pos = 0;
  control_prefix_first = 0;
  control_command_len = 0;
  g_serial_dropped.store(0, std::memory_order_relaxed);
}

static size_t drain_serial_out(size_t limit) {
  if (g_serial_silenced) {
    g_serial_out.clear();
    return 0;
  }
  size_t total = 0;
  const uint8_t* p;
  size_t n;
  while (total < limit && (n = g_serial_out.peek(&p)) > 0) {
    size_t room = limit - total;
    if (n > room) n = room;
    size_t w = Serial.write(p, n);
    if (w == 0) break;
    g_serial_out.consume(w);
    total += w;
    if (w < n) break;
  }
  return total;
}

static void serial_output_task(void*) {
  for (;;) {
    size_t drained = drain_serial_out(4096);
    if (drained == 0)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    else
      taskYIELD();
  }
}

bool start_serial_output_task() {
  ensure_serial_out();
  if (g_serial_output_task) return true;
  return xTaskCreatePinnedToCore(serial_output_task, "usbout", 4096, nullptr, 1,
                                 &g_serial_output_task, 0) == pdPASS;
}

void queue_serial_out(uint8_t out) {
  if (g_serial_silenced) return;
  ensure_serial_out();
  bool was_empty = false;
  if (!g_serial_out.push(out, &was_empty)) {
    g_serial_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (was_empty && g_serial_output_task)
    xTaskNotifyGive(g_serial_output_task);
}

void serial_output_stats(uint32_t* pending, uint32_t* dropped) {
  ensure_serial_out();
  if (pending) *pending = (uint32_t)g_serial_out.count();
  if (dropped)
    *dropped = g_serial_dropped.load(std::memory_order_relaxed);
}

bool queue_control_reply(const char* payload) {
  if (!payload) return false;
  ensure_control_reply();
  size_t payload_len = strlen(payload);
  size_t required = CONTROL_PREFIX_LEN + payload_len + 1;
  if (required > g_control_reply.capacity() - g_control_reply.count())
    return false;
  for (size_t i = 0; i < CONTROL_PREFIX_LEN; i++)
    g_control_reply.push(CONTROL_PREFIX[i]);
  for (size_t i = 0; i < payload_len; i++)
    g_control_reply.push((uint8_t)payload[i]);
  g_control_reply.push(003);
  return true;
}

bool queue_input_bytes(const uint8_t* data, size_t bytes) {
  if (!data || bytes == 0) return true;
  ensure_control_reply();
  if (bytes > g_control_reply.capacity() - g_control_reply.count())
    return false;
  for (size_t i = 0; i < bytes; i++)
    g_control_reply.push(data[i] & 0x7f);
  return true;
}

bool pop_priority_input(uint8_t* out) {
  if (!out) return false;
  ensure_control_reply();
  return g_control_reply.pop(out);
}

static void emit_console_byte(uint8_t out) {
  console_feed(out);
  telnet_write(out);
  queue_serial_out(out);
}

static void reset_control_parser() {
  control_state = CONTROL_IDLE;
  control_prefix_pos = 0;
  control_prefix_first = 0;
  control_command_len = 0;
}

void handle_guest_output(uint8_t out) {
  if (control_state == CONTROL_IDLE) {
    if (out == CONTROL_PREFIX[0] || out == CONTROL_PREFIX_BASIC) {
      control_state = CONTROL_PREFIX_MATCH;
      control_prefix_pos = 1;
      control_prefix_first = out;
    } else {
      emit_console_byte(out);
    }
    return;
  }

  if (control_state == CONTROL_PREFIX_MATCH) {
    if (out == CONTROL_PREFIX[control_prefix_pos]) {
      if (++control_prefix_pos == CONTROL_PREFIX_LEN) {
        control_state = CONTROL_TEXT;
        control_command_len = 0;
        if (control_prefix_first == CONTROL_PREFIX_BASIC)
          LOG("EMU control: accepted BASIC-PLUS $]VPDP compatibility prefix");
      }
      return;
    }

    emit_console_byte(control_prefix_first);
    for (uint8_t i = 1; i < control_prefix_pos; i++)
      emit_console_byte(CONTROL_PREFIX[i]);
    emit_console_byte(out);
    reset_control_parser();
    return;
  }

  if (out == 003 || out == 004) {
    control_command[control_command_len] = 0;
    if (!emu_control::submit(control_command))
      LOGE("EMU command queue full; command discarded");
    reset_control_parser();
    return;
  }

  bool allowed_control =
      out == '\r' || out == '\n' || out == '\a' || out == '\t';
  if (out < 0x20 && !allowed_control) {
    reset_control_parser();
    emit_console_byte(out);
    return;
  }

  if (control_command_len < CONTROL_COMMAND_MAX)
    control_command[control_command_len++] = (char)out;
}

// Legacy device entry points kept as no-ops so any residual call sites link.
void poll() {}
uint16_t read16(uint32_t) { return 0; }
void write16(uint32_t, uint16_t) {}

}  // namespace kl11
