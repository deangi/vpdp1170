#include "emu_control.h"

#include "appconfig.h"
#include "disk.h"
#include "dl11_file.h"
#include "fifo.h"
#include "kl11.h"
#include "platform.h"
#include "rk11.h"
#include "rl11.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace emu_control {

static constexpr size_t COMMAND_MAX = 256;
static constexpr size_t QUEUE_DEPTH = 4;

static char commands[QUEUE_DEPTH][COMMAND_MAX + 1];
static volatile uint8_t queue_head = 0;
static volatile uint8_t queue_tail = 0;
static bool reboot_requested = false;

static bool queue_empty() {
  return queue_head == queue_tail;
}

static bool wants_reply(char* tokens[], int count) {
  for (int i = 0; i < count; i++)
    if (!strcasecmp(tokens[i], "REPLY")) return true;
  return false;
}

static void reply(bool requested, const char* text) {
  if (requested) {
    if (!kl11::queue_control_reply(text))
      LOGE("EMU reply queue full");
  } else if (!strncmp(text, "ERROR;", 6)) {
    LOGE("EMU %s", text);
  }
}

static bool queue_line(const char* text) {
  if (!text) return false;
  size_t length = strlen(text);
  if (length > 300) return false;
  uint8_t line[301];
  memcpy(line, text, length);
  line[length] = '\r';
  if (!kl11::queue_input_bytes(line, length + 1)) {
    LOGE("EMU input reply queue full");
    return false;
  }
  return true;
}

static int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static void handle_out_ascii(const char* arguments) {
  bool requested = false;
  const char* data = arguments;
  if (!strncasecmp(data, "REPLY;", 6)) {
    requested = true;
    data += 6;
  }
  if (!dl11_file::output_connected()) {
    reply(requested, "ERROR;TTY;OUT;NOT_CONNECTED");
    return;
  }
  size_t bytes = strlen(data);
  const char* path = dl11_file::output_path();
  if (!dl11_file::write_stream((const uint8_t*)data, bytes)) {
    LOGE("VPDP file WRITE failed path=%s format=ASCII bytes=%u",
         path, (unsigned)bytes);
    reply(requested, "ERROR;OUTASCII;WRITE_FAILED");
    return;
  }
  LOG("VPDP file WRITE path=%s format=ASCII bytes=%u",
      path, (unsigned)bytes);
  reply(requested, "OK;OUTASCII");
}

static void handle_out_hex(const char* arguments) {
  bool requested = false;
  const char* text = arguments;
  if (!strncasecmp(text, "REPLY;", 6)) {
    requested = true;
    text += 6;
  }
  if (!dl11_file::output_connected()) {
    reply(requested, "ERROR;TTY;OUT;NOT_CONNECTED");
    return;
  }

  uint8_t bytes[128];
  size_t count = 0;
  int high_nibble = -1;
  for (; *text; text++) {
    if (*text == ' ' || *text == '\t') continue;
    int value = hex_value(*text);
    if (value < 0 || count >= sizeof(bytes)) {
      reply(requested, "ERROR;OUTHEX;INVALID_DATA");
      return;
    }
    if (high_nibble < 0) {
      high_nibble = value;
    } else {
      bytes[count++] = (uint8_t)((high_nibble << 4) | value);
      high_nibble = -1;
    }
  }
  if (high_nibble >= 0) {
    reply(requested, "ERROR;OUTHEX;ODD_DIGIT_COUNT");
    return;
  }
  const char* path = dl11_file::output_path();
  if (!dl11_file::write_stream(bytes, count)) {
    LOGE("VPDP file WRITE failed path=%s format=HEX bytes=%u",
         path, (unsigned)count);
    reply(requested, "ERROR;OUTHEX;WRITE_FAILED");
    return;
  }
  LOG("VPDP file WRITE path=%s format=HEX bytes=%u",
      path, (unsigned)count);
  reply(requested, "OK;OUTHEX");
}

static void handle_in_ascii() {
  char line[241];
  size_t count = 0;
  size_t bytes_read = 0;
  bool consumed = false;
  char path[96];
  strncpy(path, dl11_file::input_path(), sizeof(path) - 1);
  path[sizeof(path) - 1] = 0;
  while (count < sizeof(line) - 1) {
    uint8_t value = 0;
    if (dl11_file::read_stream(&value, 1) == 0) break;
    bytes_read++;
    consumed = true;
    if (value == '\n') break;
    if (value != '\r') line[count++] = (char)(value & 0x7f);
  }
  line[count] = 0;
  if (consumed) {
    LOG("VPDP file READ path=%s format=ASCII bytes=%u",
        path, (unsigned)bytes_read);
    queue_line(line);
  } else if (dl11_file::input_at_eof()) {
    LOG("VPDP file READ path=%s format=ASCII EOF", path);
    queue_line("*>EOF<*");
  } else {
    LOGE("VPDP file READ failed format=ASCII: input not connected");
    queue_line("ERROR;TTY;IN;NOT_CONNECTED");
  }
}

static void handle_in_hex(const char* count_text) {
  char* end = nullptr;
  long requested = strtol(count_text, &end, 10);
  if (!count_text[0] || !end || *end || requested < 1 || requested > 128) {
    queue_line("ERROR;INHEX;COUNT_MUST_BE_1_TO_128");
    return;
  }

  uint8_t bytes[128];
  char path[96];
  strncpy(path, dl11_file::input_path(), sizeof(path) - 1);
  path[sizeof(path) - 1] = 0;
  size_t count = dl11_file::read_stream(bytes, (size_t)requested);
  if (count == 0) {
    if (dl11_file::input_at_eof()) {
      LOG("VPDP file READ path=%s format=HEX requested=%ld EOF",
          path, requested);
      queue_line("*>EOF<*");
    } else {
      LOGE("VPDP file READ failed format=HEX requested=%ld: input not connected",
           requested);
      queue_line("ERROR;TTY;IN;NOT_CONNECTED");
    }
    return;
  }
  LOG("VPDP file READ path=%s format=HEX requested=%ld bytes=%u",
      path, requested, (unsigned)count);

  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  char line[257];
  for (size_t i = 0; i < count; i++) {
    line[i * 2] = HEX_DIGITS[bytes[i] >> 4];
    line[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0f];
  }
  line[count * 2] = 0;
  queue_line(line);
}

static bool valid_path(const char* path) {
  if (!path || path[0] != '/' || strstr(path, "..")) return false;
  for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
    if (*p < 0x20 || *p == 0x7f || *p == ';') return false;
  return strlen(path) < 96;
}

static void remove_path_controls(char* path) {
  if (!path) return;
  char* destination = path;
  for (const char* source = path; *source; source++) {
    if (*source == '\r' || *source == '\n' ||
        *source == '\a' || *source == '\t')
      continue;
    *destination++ = *source;
  }
  *destination = 0;
}

static bool path_is_mounted_disk(const char* path) {
  for (int slot = 0; slot < DRIVE_COUNT; slot++)
    if (disk_is_mounted(slot) && !strcmp(path, disk_path(slot))) return true;
  return false;
}

static int parse_eof(char* tokens[], int count) {
  for (int i = 0; i < count; i++) {
    if (strncasecmp(tokens[i], "EOF=", 4)) continue;
    const char* value = tokens[i] + 4;
    if (!strcasecmp(value, "NONE")) return -1;
    char* end = nullptr;
    long parsed = strtol(value, &end, 0);
    if (end && *end == 0 && parsed >= 0 && parsed <= 255) return (int)parsed;
    return -2;
  }
  return -1;
}

static bool has_token(char* tokens[], int count, const char* wanted) {
  for (int i = 0; i < count; i++)
    if (!strcasecmp(tokens[i], wanted)) return true;
  return false;
}

static int disk_slot(const char* unit) {
  if (!strcasecmp(unit, "RL0") || !strcasecmp(unit, "DL0")) return DRIVE_A;
  if (!strcasecmp(unit, "RL1") || !strcasecmp(unit, "DL1")) return DRIVE_B;
  if (!strcasecmp(unit, "RL2") || !strcasecmp(unit, "DL2")) return DRIVE_C;
  if (!strcasecmp(unit, "RL3") || !strcasecmp(unit, "DL3")) return DRIVE_D;
  if (!strcasecmp(unit, "RK0")) return DRIVE_A;
  return -1;
}

static bool controller_active(const char* unit) {
  bool rk = !strncasecmp(unit, "RK", 2);
  return rk ? cfg.boot_kind == AppConfig::BK_RK
            : cfg.boot_kind == AppConfig::BK_RL;
}

static bool unit_is_rl(const char* unit) {
  return unit && (!strncasecmp(unit, "RL", 2) || !strncasecmp(unit, "DL", 2));
}

static void media_changed(const char* unit, int slot, bool mounted) {
  if (!strncasecmp(unit, "RK", 2))
    rk11::media_changed(slot, mounted);
  else
    rl11::media_changed(slot, mounted);
}

static void handle_tty(char* tokens[], int count, bool requested) {
  int base = !strcasecmp(tokens[0], "TTY") ? 1 : 0;
  if (base == 1 && count > 1 && !strcasecmp(tokens[1], "STATUS")) {
    char result[320];
    snprintf(result, sizeof(result),
             "STATUS;TTY;IN=%s,%s;OUT=%s,%s,%s",
             dl11_file::input_connected() ? "CONNECTED"
               : dl11_file::input_at_eof() ? "EOF" : "DISCONNECTED",
             dl11_file::input_path(),
             dl11_file::output_connected() ? "CONNECTED" : "DISCONNECTED",
             dl11_file::output_path(),
             dl11_file::output_append_mode() ? "APPEND" : "TRUNCATE");
    reply(requested, result);
    return;
  }
  if (count <= base + 1) {
    reply(requested, "ERROR;BAD_COMMAND");
    return;
  }
  const char* direction = tokens[base];
  const char* action = tokens[base + 1];

  if (!strcasecmp(direction, "IN")) {
    if (!strcasecmp(action, "OPEN") && count > base + 2) {
      char* path = tokens[base + 2];
      remove_path_controls(path);
      int eof = parse_eof(tokens, count);
      bool notify = has_token(tokens, count, "NOTIFY");
      if (!valid_path(path)) reply(requested, "ERROR;INVALID_PATH");
      else if (path_is_mounted_disk(path))
        reply(requested, "ERROR;PATH_IS_MOUNTED_DISK");
      else if (dl11_file::output_connected() &&
               !strcmp(path, dl11_file::output_path()))
        reply(requested, "ERROR;INPUT_OUTPUT_PATH_CONFLICT");
      else if (eof == -2) reply(requested, "ERROR;INVALID_EOF");
      else if (dl11_file::open_input(path, eof, notify)) {
        char result[160];
        snprintf(result, sizeof(result), "OK;TTY;IN;OPEN;%s", path);
        reply(requested, result);
      } else {
        reply(requested, "ERROR;FILE_OPEN_FAILED");
      }
      return;
    }
    if (!strcasecmp(action, "CLOSE")) {
      dl11_file::close_input();
      reply(requested, "OK;TTY;IN;CLOSE");
      return;
    }
    if (!strcasecmp(action, "STATUS")) {
      char result[192];
      snprintf(result, sizeof(result), "STATUS;TTY;IN;%s;%s",
               dl11_file::input_connected() ? "CONNECTED"
                 : dl11_file::input_at_eof() ? "EOF" : "DISCONNECTED",
               dl11_file::input_path());
      reply(requested, result);
      return;
    }
  }

  if (!strcasecmp(direction, "OUT")) {
    if (!strcasecmp(action, "OPEN") && count > base + 2) {
      char* path = tokens[base + 2];
      remove_path_controls(path);
      bool append = !has_token(tokens, count, "TRUNCATE");
      if (!valid_path(path)) reply(requested, "ERROR;INVALID_PATH");
      else if (path_is_mounted_disk(path))
        reply(requested, "ERROR;PATH_IS_MOUNTED_DISK");
      else if (dl11_file::input_connected() &&
               !strcmp(path, dl11_file::input_path()))
        reply(requested, "ERROR;INPUT_OUTPUT_PATH_CONFLICT");
      else if (dl11_file::open_output(path, append)) {
        char result[160];
        snprintf(result, sizeof(result), "OK;TTY;OUT;OPEN;%s;%s",
                 path, append ? "APPEND" : "TRUNCATE");
        reply(requested, result);
      } else {
        reply(requested, "ERROR;FILE_OPEN_FAILED");
      }
      return;
    }
    if (!strcasecmp(action, "CLOSE")) {
      dl11_file::close_output();
      reply(requested, "OK;TTY;OUT;CLOSE");
      return;
    }
    if (!strcasecmp(action, "STATUS")) {
      char result[192];
      snprintf(result, sizeof(result), "STATUS;TTY;OUT;%s;%s;%s",
               dl11_file::output_connected() ? "CONNECTED" : "DISCONNECTED",
               dl11_file::output_path(),
               dl11_file::output_append_mode() ? "APPEND" : "TRUNCATE");
      reply(requested, result);
      return;
    }
  }

  reply(requested, "ERROR;BAD_TTY_COMMAND");
}

static void handle_disk(char* tokens[], int count, bool requested) {
  if (count < 3) {
    reply(requested, "ERROR;BAD_DISK_COMMAND");
    return;
  }
  const char* action = tokens[1];
  const char* unit = tokens[2];
  if (!strcasecmp(action, "STATUS") && !strcasecmp(unit, "ALL")) {
    char result[640];
    size_t used = snprintf(result, sizeof(result), "STATUS;DISK");
    for (int slot = DRIVE_A; slot <= DRIVE_D && used < sizeof(result); slot++) {
      const char* name = cfg.boot_kind == AppConfig::BK_RK && slot == DRIVE_A
                           ? "RK0" : (slot == 0 ? "RL0" :
                                      slot == 1 ? "RL1" :
                                      slot == 2 ? "RL2" : "RL3");
      used += snprintf(result + used, sizeof(result) - used, ";%s=%s,%s,%s",
                       name, disk_is_mounted(slot) ? "MOUNTED" : "EMPTY",
                       disk_path(slot), disk_is_readonly(slot) ? "RO" : "RW");
    }
    reply(requested, result);
    return;
  }
  int slot = disk_slot(unit);
  if (slot < 0) {
    reply(requested, "ERROR;INVALID_UNIT");
    return;
  }
  if (!controller_active(unit)) {
    reply(requested, "ERROR;CONTROLLER_INACTIVE");
    return;
  }

  if (!strcasecmp(action, "MOUNT") && count > 3) {
    char* path = tokens[3];
    remove_path_controls(path);
    bool readonly = has_token(tokens, count, "READONLY");
    if (!valid_path(path)) {
      reply(requested, "ERROR;INVALID_PATH");
      return;
    }
    if ((dl11_file::input_connected() && !strcmp(path, dl11_file::input_path())) ||
        (dl11_file::output_connected() && !strcmp(path, dl11_file::output_path()))) {
      reply(requested, "ERROR;PATH_IS_TTY_FILE");
      return;
    }
    if (!disk_mount_mode(slot, path, readonly)) {
      reply(requested, "ERROR;DISK_MOUNT_FAILED");
      return;
    }
    if (unit_is_rl(unit) && !rl11::validate_mounted_media(slot)) {
      uint32_t bytes = disk_size_bytes(slot);
      disk_dismount(slot);
      rl11::media_changed(slot, false);
      LOGE("EMU DISK MOUNT rejected %s path=%s size=%lu expected RL01=%lu RL02=%lu",
           unit, path, (unsigned long)bytes,
           (unsigned long)rl11::RL01_IMAGE_BYTES,
           (unsigned long)rl11::RL02_IMAGE_BYTES);
      reply(requested, "ERROR;DISK;INVALID_RL_SIZE");
      return;
    }
    media_changed(unit, slot, true);
    char result[192];
    snprintf(result, sizeof(result), "OK;DISK;MOUNT;%s;%s;%s",
             unit, path, disk_is_readonly(slot) ? "RO" : "RW");
    reply(requested, result);
    return;
  }

  if (!strcasecmp(action, "DISMOUNT")) {
    disk_dismount(slot);
    media_changed(unit, slot, false);
    char result[96];
    snprintf(result, sizeof(result), "OK;DISK;DISMOUNT;%s", unit);
    reply(requested, result);
    return;
  }

  if (!strcasecmp(action, "STATUS")) {
    char result[224];
    snprintf(result, sizeof(result), "STATUS;DISK;%s;%s;%s;%lu;%s",
             unit, disk_is_mounted(slot) ? "MOUNTED" : "EMPTY",
             disk_path(slot), (unsigned long)disk_size_bytes(slot),
             disk_is_readonly(slot) ? "RO" : "RW");
    reply(requested, result);
    return;
  }

  reply(requested, "ERROR;BAD_DISK_COMMAND");
}

static void execute(char* command) {
  if (!strncasecmp(command, "OUTASCII;", 9)) {
    handle_out_ascii(command + 9);
    return;
  }
  if (!strncasecmp(command, "OUTHEX;", 7)) {
    handle_out_hex(command + 7);
    return;
  }
  if (!strcasecmp(command, "INASCII")) {
    handle_in_ascii();
    return;
  }
  if (!strncasecmp(command, "INHEX:", 6) ||
      !strncasecmp(command, "INHEX;", 6)) {
    handle_in_hex(command + 6);
    return;
  }

  char* tokens[20];
  int count = 0;
  char* save = nullptr;
  for (char* token = strtok_r(command, ";", &save);
       token && count < (int)(sizeof(tokens) / sizeof(tokens[0]));
       token = strtok_r(nullptr, ";", &save)) {
    while (*token == ' ') token++;
    char* end = token + strlen(token);
    while (end > token && end[-1] == ' ') *--end = 0;
    tokens[count++] = token;
  }
  if (count == 0) return;
  bool requested = wants_reply(tokens, count);

  if (!strcasecmp(tokens[0], "IN") || !strcasecmp(tokens[0], "OUT") ||
      !strcasecmp(tokens[0], "TTY")) {
    handle_tty(tokens, count, requested);
  } else if (!strcasecmp(tokens[0], "CLOSE") && count > 1 &&
             !strcasecmp(tokens[1], "ALL")) {
    char input_path[96];
    char output_path[96];
    strncpy(input_path, dl11_file::input_path(), sizeof(input_path) - 1);
    input_path[sizeof(input_path) - 1] = 0;
    strncpy(output_path, dl11_file::output_path(), sizeof(output_path) - 1);
    output_path[sizeof(output_path) - 1] = 0;
    dl11_file::disconnect_all();
    LOG("VPDP file CLOSE ALL input=%s output=%s", input_path, output_path);
    reply(requested, "OK;CLOSE;ALL");
  } else if (!strcasecmp(tokens[0], "DISK")) {
    handle_disk(tokens, count, requested);
  } else if (!strcasecmp(tokens[0], "PDP") && count > 1 &&
             !strcasecmp(tokens[1], "REBOOT")) {
    reply(requested, "OK;PDP;REBOOT;SCHEDULED");
    reboot_requested = true;
  } else if (!strcasecmp(tokens[0], "STATUS")) {
    char result[224];
    snprintf(result, sizeof(result),
             "STATUS;TTY1=%s;IN=%s;OUT=%s;BOOT=%s",
             dl11_file::enabled() ? "ENABLED" : "DISABLED",
             dl11_file::input_connected() ? dl11_file::input_path() : "",
             dl11_file::output_connected() ? dl11_file::output_path() : "",
             cfg.boot_unit_label());
    reply(requested, result);
  } else {
    reply(requested, "ERROR;UNKNOWN_COMMAND");
  }
}

void init() {
  queue_head = queue_tail = 0;
  reboot_requested = false;
}

bool submit(const char* command) {
  uint8_t next = (uint8_t)((queue_head + 1) % QUEUE_DEPTH);
  if (next == queue_tail) return false;
  strncpy(commands[queue_head], command, COMMAND_MAX);
  commands[queue_head][COMMAND_MAX] = 0;
  queue_head = next;
  return true;
}

void poll() {
  dl11_file::host_poll();
  if (queue_empty()) return;
  char command[COMMAND_MAX + 1];
  strncpy(command, commands[queue_tail], sizeof(command));
  command[COMMAND_MAX] = 0;
  queue_tail = (uint8_t)((queue_tail + 1) % QUEUE_DEPTH);
  LOG("EMU command received");
  execute(command);
}

bool consume_reboot_request() {
  bool requested = reboot_requested;
  reboot_requested = false;
  return requested;
}

}  // namespace emu_control
