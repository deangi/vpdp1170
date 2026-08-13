#include "boot_input.h"
#include "platform.h"

#include <Arduino.h>
#include <string.h>

static HostBootInputSegment g_segments[HOST_BOOT_INPUT_MAX_SEGMENTS];
static uint8_t g_count = 0;
static uint8_t g_index = 0;
static bool g_armed = false;
static bool g_waiting = false;
static uint32_t g_due_ms = 0;
static HostKeyInjectFn g_inject = nullptr;

void host_boot_input_set_inject(HostKeyInjectFn fn) {
  g_inject = fn;
}

static void schedule_current() {
  if (!g_armed || g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }
  g_waiting = true;
  g_due_ms = millis() + g_segments[g_index].delay_ms;
}

static void fire_current() {
  if (!g_armed || g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }

  const HostBootInputSegment& seg = g_segments[g_index];
  if (g_inject) {
    for (uint8_t i = 0; i < seg.data_len; i++)
      g_inject(seg.data[i]);
  }

  g_index++;
  if (g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }
  schedule_current();
}

void host_boot_input_disarm() {
  g_armed = false;
  g_count = 0;
  g_index = 0;
  g_waiting = false;
  g_due_ms = 0;
}

void host_boot_input_arm(const HostBootInputSegment* segs, uint8_t count) {
  host_boot_input_disarm();
  if (!segs || count == 0) return;

  g_count = count;
  if (g_count > HOST_BOOT_INPUT_MAX_SEGMENTS)
    g_count = HOST_BOOT_INPUT_MAX_SEGMENTS;
  memcpy(g_segments, segs, sizeof(g_segments[0]) * g_count);
  g_armed = true;
  g_index = 0;
  schedule_current();

  uint32_t total_delay = 0;
  size_t total_bytes = 0;
  for (uint8_t i = 0; i < g_count; i++) {
    total_delay += g_segments[i].delay_ms;
    total_bytes += g_segments[i].data_len;
  }
  LOG("console: armed boot_input (%u segment%s, %u bytes, %lu ms delays)",
      (unsigned)g_count,
      g_count == 1 ? "" : "s",
      (unsigned)total_bytes,
      (unsigned long)total_delay);
}

bool host_boot_input_active() {
  return g_armed || g_waiting;
}

void host_boot_input_poll() {
  if (!g_armed || !g_waiting) return;
  if ((int32_t)(millis() - g_due_ms) < 0) return;
  fire_current();
}
