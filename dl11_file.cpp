#include "dl11_file.h"

#include "fifo.h"
#include "kd11.h"
#include "kl11.h"
#include "pdp1140.h"
#include "platform.h"
#include "sam11.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include "esp_attr.h"
#include <string.h>

namespace dl11_file {

static constexpr uint16_t RX_VECTOR = 0300;
static constexpr uint16_t TX_VECTOR = 0304;
static constexpr uint8_t IRQ_LEVEL = 4;
static constexpr size_t FIFO_SIZE = 4096;

static bool g_enabled = false;
static uint16_t rcsr = 0;
static uint16_t rbuf = 0;
static uint16_t xcsr = 0200;
static uint16_t xbuf = 0;
static uint8_t tx_delay = 0;
static uint8_t rx_poll_div = 0;

EXT_RAM_BSS_ATTR static uint8_t input_storage[FIFO_SIZE];
EXT_RAM_BSS_ATTR static uint8_t output_storage[FIFO_SIZE];
static Fifo input_fifo;
static Fifo output_fifo;
static bool fifos_initialized = false;

static File input_file;
static File output_file;
static char input_name[96] = {0};
static char output_name[96] = {0};
static bool input_eof = false;
static int input_eof_byte = -1;
static bool input_notify = false;
static bool output_append = true;

static bool interrupt_pending(uint16_t vector) {
  for (uint8_t i = 0; i < ITABN; i++) {
    if (itab[i].vec == 0) break;
    if (itab[i].vec == vector) return true;
  }
  return false;
}

static void init_fifos() {
  if (fifos_initialized) return;
  input_fifo.init(input_storage, FIFO_SIZE);
  output_fifo.init(output_storage, FIFO_SIZE);
  fifos_initialized = true;
}

static void copy_path(char* destination, size_t size, const char* source) {
  if (!destination || size == 0) return;
  strncpy(destination, source ? source : "", size - 1);
  destination[size - 1] = 0;
}

static void drain_output_locked() {
  if (!output_file) return;
  const uint8_t* data = nullptr;
  size_t count = 0;
  size_t total_written = 0;
  while ((count = output_fifo.peek(&data)) > 0) {
    size_t written = output_file.write(data, count);
    if (written == 0) break;
    output_fifo.consume(written);
    total_written += written;
    if (written < count) break;
  }
  if (total_written > 0) {
    LOG("VPDP file WRITE path=%s source=TT1 bytes=%u",
        output_name, (unsigned)total_written);
  }
}

static bool finish_input_locked(bool enqueue_eof_byte) {
  char completed_path[sizeof(input_name)];
  copy_path(completed_path, sizeof(completed_path), input_name);

  if (enqueue_eof_byte && input_eof_byte >= 0 && !input_eof) {
    if (!input_fifo.push((uint8_t)input_eof_byte)) return false;
  }
  input_eof = true;
  if (input_file) {
    input_file.close();
    LOG("VPDP file CLOSE direction=IN path=%s reason=EOF", completed_path);
  }
  input_name[0] = 0;
  if (input_notify) {
    char reply[160];
    snprintf(reply, sizeof(reply), "EVENT;TTY;IN;EOF;%s", completed_path);
    if (!kl11::queue_control_reply(reply))
      LOGE("TT1 EOF notification queue full");
  }
  input_notify = false;
  return true;
}

void set_enabled(bool enabled_value) {
  g_enabled = enabled_value;
  if (!g_enabled) {
    disconnect_all();
    kd11::cancelinterrupt(RX_VECTOR);
    kd11::cancelinterrupt(TX_VECTOR);
  }
}

bool enabled() {
  return g_enabled;
}

void reset() {
  init_fifos();
  rcsr = 0;
  rbuf = 0;
  xcsr = 0200;
  xbuf = 0;
  tx_delay = 0;
  rx_poll_div = 0;
  kd11::cancelinterrupt(RX_VECTOR);
  kd11::cancelinterrupt(TX_VECTOR);
}

void poll() {
  if (!g_enabled) return;

  if (++rx_poll_div >= 100) {
    rx_poll_div = 0;
    if (!(rcsr & 0200) && !interrupt_pending(RX_VECTOR)) {
      uint8_t value = 0;
      if (input_fifo.pop(&value)) {
        rbuf = value & 0177;
        rcsr |= 0200;
        if (rcsr & 0100) kd11::interrupt(RX_VECTOR, IRQ_LEVEL);
      }
    }
  }

  if (!(xcsr & 0200) && ++tx_delay > 32) {
    if (output_file && !output_fifo.push((uint8_t)(xbuf & 0177))) {
      LOGE("TT1 output FIFO full; byte dropped");
    }
    xcsr |= 0200;
    if (xcsr & 0100) kd11::interrupt(TX_VECTOR, IRQ_LEVEL);
  }
}

void host_poll() {
  if (!g_enabled) return;
  init_fifos();

  SD_FTP_StorageGuard guard;
  drain_output_locked();

  if (!input_file || input_fifo.count() >= input_fifo.capacity()) return;

  size_t room = input_fifo.capacity() - input_fifo.count();
  size_t limit = room < 128 ? room : 128;
  size_t total_read = 0;
  while (limit-- > 0 && input_file.available()) {
    int value = input_file.read();
    if (value < 0 || !input_fifo.push((uint8_t)value)) break;
    total_read++;
  }
  if (total_read > 0) {
    LOG("VPDP file READ path=%s destination=TT1 bytes=%u",
        input_name, (unsigned)total_read);
  }

  if (!input_file.available()) {
    if (!finish_input_locked(true)) return;
  }
}

uint16_t read16(uint32_t address) {
  switch (address) {
    case DEV_DL_1_TTY_IN_STATUS:
      return rcsr;
    case DEV_DL_1_TTY_IN_DATA:
      if (rcsr & 0200) {
        rcsr &= ~0200;
        return rbuf;
      }
      return 0;
    case DEV_DL_1_TTY_OUT_STATUS:
      return xcsr;
    case DEV_DL_1_TTY_OUT_DATA:
      return 0;
    default:
      return 0;
  }
}

void write16(uint32_t address, uint16_t value) {
  switch (address) {
    case DEV_DL_1_TTY_IN_STATUS: {
      bool was_disabled = !(rcsr & 0100);
      rcsr = (rcsr & 0200) | (value & 0100);
      if (was_disabled && (rcsr & 0300) == 0300)
        kd11::interrupt(RX_VECTOR, IRQ_LEVEL);
      break;
    }
    case DEV_DL_1_TTY_OUT_STATUS: {
      bool was_disabled = !(xcsr & 0100);
      xcsr = (xcsr & 0200) | (value & 0100);
      if (was_disabled && (xcsr & 0300) == 0300)
        kd11::interrupt(TX_VECTOR, IRQ_LEVEL);
      break;
    }
    case DEV_DL_1_TTY_OUT_DATA:
      xbuf = value & 0377;
      xcsr &= ~0200;
      tx_delay = 0;
      break;
    case DEV_DL_1_TTY_IN_DATA:
    default:
      break;
  }
}

bool open_input(const char* path, int eof_byte, bool notify) {
  init_fifos();
  SD_FTP_StorageGuard guard;
  LOG("VPDP file OPEN attempt path=%s direction=IN", path ? path : "");
  File replacement = SD_MMC.open(path, "r");
  if (!replacement) {
    LOGE("VPDP file OPEN failed path=%s direction=IN", path ? path : "");
    return false;
  }

  if (input_file) {
    LOG("VPDP file CLOSE direction=IN path=%s reason=replace", input_name);
    input_file.close();
  }
  input_file = replacement;
  input_fifo.clear();
  rcsr &= ~0200;
  rbuf = 0;
  kd11::cancelinterrupt(RX_VECTOR);
  copy_path(input_name, sizeof(input_name), path);
  input_eof = false;
  input_eof_byte = eof_byte;
  input_notify = notify;
  LOG("VPDP file OPEN path=%s direction=IN size=%u eof=%d notify=%s",
      input_name, (unsigned)input_file.size(), input_eof_byte,
      input_notify ? "yes" : "no");
  return true;
}

void close_input() {
  SD_FTP_StorageGuard guard;
  if (input_file) {
    LOG("VPDP file CLOSE direction=IN path=%s reason=command", input_name);
    input_file.close();
  }
  input_fifo.clear();
  rcsr &= ~0200;
  rbuf = 0;
  kd11::cancelinterrupt(RX_VECTOR);
  input_name[0] = 0;
  input_eof = false;
  input_eof_byte = -1;
  input_notify = false;
}

bool input_connected() {
  return (bool)input_file;
}

bool input_at_eof() {
  return input_eof;
}

const char* input_path() {
  return input_name;
}

bool open_output(const char* path, bool append) {
  init_fifos();
  SD_FTP_StorageGuard guard;
  LOG("VPDP file OPEN attempt path=%s direction=OUT mode=%s",
      path ? path : "", append ? "APPEND" : "TRUNCATE");
  if (output_file && !strcmp(path, output_name)) {
    drain_output_locked();
    output_file.flush();
    LOG("VPDP file CLOSE direction=OUT path=%s reason=reopen", output_name);
    output_file.close();
    output_name[0] = 0;
  }
  File replacement = SD_MMC.open(path, append ? "a" : "w");
  if (!replacement) {
    LOGE("VPDP file OPEN failed path=%s direction=OUT mode=%s",
         path ? path : "", append ? "APPEND" : "TRUNCATE");
    return false;
  }

  drain_output_locked();
  if (output_file) {
    output_file.flush();
    LOG("VPDP file CLOSE direction=OUT path=%s reason=replace", output_name);
    output_file.close();
  }
  output_file = replacement;
  copy_path(output_name, sizeof(output_name), path);
  output_append = append;
  LOG("VPDP file OPEN path=%s direction=OUT mode=%s",
      output_name, output_append ? "APPEND" : "TRUNCATE");
  return true;
}

void close_output() {
  SD_FTP_StorageGuard guard;
  drain_output_locked();
  if (output_file) {
    output_file.flush();
    LOG("VPDP file CLOSE direction=OUT path=%s reason=command", output_name);
    output_file.close();
  }
  output_fifo.clear();
  output_name[0] = 0;
}

bool output_connected() {
  return (bool)output_file;
}

const char* output_path() {
  return output_name;
}

bool output_append_mode() {
  return output_append;
}

size_t read_stream(uint8_t* data, size_t max_bytes) {
  if (!data || max_bytes == 0) return 0;
  init_fifos();

  size_t count = 0;
  if ((rcsr & 0200) && count < max_bytes) {
    data[count++] = (uint8_t)(rbuf & 0377);
    rcsr &= ~0200;
    rbuf = 0;
    kd11::cancelinterrupt(RX_VECTOR);
  }
  while (count < max_bytes && input_fifo.pop(&data[count])) count++;
  if (count == max_bytes) return count;

  SD_FTP_StorageGuard guard;
  while (count < max_bytes && input_file && input_file.available()) {
    int value = input_file.read();
    if (value < 0) break;
    data[count++] = (uint8_t)value;
  }

  if (input_file && !input_file.available()) {
    finish_input_locked(false);
  }
  return count;
}

bool write_stream(const uint8_t* data, size_t bytes) {
  if (!data || !output_file) return false;
  SD_FTP_StorageGuard guard;
  drain_output_locked();
  size_t written = output_file.write(data, bytes);
  output_file.flush();
  return written == bytes;
}

void disconnect_all() {
  close_input();
  close_output();
}

}  // namespace dl11_file
