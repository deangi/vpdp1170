#include "telnet_shell.h"

#include "SD_FTP_Server/src/SD_FTP_Server.h"
#include "appconfig.h"
#include "config.h"
#include "disk.h"
#include "dd11.h"
#include "emu_control.h"
#include "fifo.h"
#include "host_lib/shell/shell_core.h"
#include "host_lib/shell/shell_media.h"
#include "host_lib/shell/shell_settings.h"
#include "kl11.h"
#include "kw11.h"
#include "pdp_core.h"
#include "platform.h"
#include "telnet.h"
#include "kek_deuna.h"

#include <Arduino.h>
#include "sd_fs.h"
#include "esp_attr.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void console_output_stats(uint32_t* pending, uint32_t* dropped);

extern "C" void kek_tty_set_trace(uint32_t count);
extern "C" uint32_t kek_tty_trace_remaining();
extern "C" void kek_tty_get_stats(uint32_t* tx_chars,
                                  uint32_t* tx_ready_events,
                                  uint32_t* tx_irq_queues,
                                  uint32_t* tx_irq_unqueues,
                                  uint32_t* rx_chars,
                                  uint32_t* rx_irq_queues,
                                  uint32_t* rx_irq_unqueues,
                                  uint8_t* last_tx,
                                  uint32_t* last_tx_ms,
                                  uint32_t* last_tx_ready_ms,
                                  uint32_t* trace_remaining,
                                  uint16_t* tks, uint16_t* tkb,
                                  uint16_t* tps, uint16_t* tpb,
                                  uint8_t* tx_busy);

static constexpr size_t SHELL_LINE_MAX = 255;
static constexpr size_t SHELL_QUEUE_DEPTH = 4;
static constexpr size_t SHELL_OUTPUT_BYTES = 8192;
static constexpr size_t SHELL_PATH_MAX = 128;

static volatile bool g_active = false;
static char g_input_line[SHELL_LINE_MAX + 1];
static size_t g_input_len = 0;
static char g_commands[SHELL_QUEUE_DEPTH][SHELL_LINE_MAX + 1];
static volatile uint8_t g_command_head = 0;
static volatile uint8_t g_command_tail = 0;
EXT_RAM_BSS_ATTR static uint8_t g_output_storage[SHELL_OUTPUT_BYTES];
EXT_RAM_BSS_ATTR static uint8_t g_file_buffer[4096];
static Fifo g_output;
static bool g_initialized = false;
static bool g_monitor_mode = false;
static char g_cwd[SHELL_PATH_MAX] = "/";

static void output_char(uint8_t value) {
  g_output.push(value);
  if (value == 255) g_output.push(value);
}

static bool output_char_wait(uint8_t value) {
  uint32_t started = millis();
  while (!g_output.push(value)) {
    if (!g_active || millis() - started >= 2000) return false;
    delay(1);
  }
  if (value == 255) {
    started = millis();
    while (!g_output.push(value)) {
      if (!g_active || millis() - started >= 2000) return false;
      delay(1);
    }
  }
  return true;
}

static void output_text(const char* text) {
  if (!text) return;
  while (*text) output_char((uint8_t)*text++);
}

static void output_printf(const char* format, ...) {
  char buffer[384];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  output_text(buffer);
}

static void prompt() {
  if (g_monitor_mode)
    output_text("monitor> ");
  else
    output_printf("vpdp:%s> ", g_cwd);
}

static bool queue_command(const char* command) {
  uint8_t next = (uint8_t)((g_command_head + 1) % SHELL_QUEUE_DEPTH);
  if (next == g_command_tail) return false;
  strncpy(g_commands[g_command_head], command, SHELL_LINE_MAX);
  g_commands[g_command_head][SHELL_LINE_MAX] = 0;
  g_command_head = next;
  return true;
}

static bool pop_command(char* command, size_t size) {
  if (g_command_head == g_command_tail) return false;
  strncpy(command, g_commands[g_command_tail], size - 1);
  command[size - 1] = 0;
  g_command_tail = (uint8_t)((g_command_tail + 1) % SHELL_QUEUE_DEPTH);
  return true;
}

static void register_vpdp_shell();

void telnet_shell_init() {
  if (g_initialized) return;
  g_output.init(g_output_storage, SHELL_OUTPUT_BYTES);
  shell_set_out(output_text);
  register_vpdp_shell();
  g_initialized = true;
}

void telnet_shell_enter() {
  telnet_shell_init();
  g_input_len = 0;
  g_input_line[0] = 0;
  g_command_tail = g_command_head;
  g_output.clear();
  strcpy(g_cwd, "/");
  g_monitor_mode = false;
  g_active = true;
  LOG("telnet shell: entered by %s", telnet_client_ip());
}

void telnet_shell_disconnect() {
  g_active = false;
  g_monitor_mode = false;
  g_input_len = 0;
  g_command_tail = g_command_head;
  if (g_initialized) g_output.clear();
}

bool telnet_shell_active() {
  return g_active;
}

bool telnet_shell_backspace() {
  if (!g_active || g_input_len == 0) return false;
  g_input_line[--g_input_len] = 0;
  return true;
}

bool telnet_shell_input(uint8_t c) {
  if (!g_active) return false;
  if (c == '\r' || c == '\n') {
    g_input_line[g_input_len] = 0;
    if (!queue_command(g_input_line))
      LOGE("telnet shell: command queue full");
    g_input_len = 0;
    g_input_line[0] = 0;
    return false;
  }
  if (c < 0x20 || c > 0x7e || g_input_len >= SHELL_LINE_MAX) return false;
  g_input_line[g_input_len++] = (char)c;
  g_input_line[g_input_len] = 0;
  return true;
}

size_t telnet_shell_output_peek(const uint8_t** data) {
  if (!g_initialized) {
    *data = nullptr;
    return 0;
  }
  return g_output.peek(data);
}

void telnet_shell_output_consume(size_t bytes) {
  g_output.consume(bytes);
}

static bool mounted_path(const char* path) {
  for (int slot = 0; slot < DRIVE_COUNT; slot++)
    if (disk_is_mounted(slot) && !strcasecmp(path, disk_path(slot))) return true;
  return false;
}

static bool normalize_path(const char* input, char* output, size_t size) {
  if (!input || !*input || !output || size < 2) return false;
  char combined[256];
  int written;
  if (input[0] == '/')
    written = snprintf(combined, sizeof(combined), "%s", input);
  else if (!strcmp(g_cwd, "/"))
    written = snprintf(combined, sizeof(combined), "/%s", input);
  else
    written = snprintf(combined, sizeof(combined), "%s/%s", g_cwd, input);
  if (written < 0 || (size_t)written >= sizeof(combined)) return false;

  char working[256];
  strncpy(working, combined, sizeof(working) - 1);
  working[sizeof(working) - 1] = 0;
  const char* parts[32];
  size_t count = 0;
  char* save = nullptr;
  for (char* part = strtok_r(working, "/", &save);
       part;
       part = strtok_r(nullptr, "/", &save)) {
    if (!strcmp(part, ".") || !*part) continue;
    if (!strcmp(part, "..")) {
      if (count) count--;
      continue;
    }
    if (strchr(part, '\\') || strchr(part, ':') || strchr(part, ';'))
      return false;
    if (count >= sizeof(parts) / sizeof(parts[0])) return false;
    parts[count++] = part;
  }

  size_t used = 0;
  output[used++] = '/';
  for (size_t i = 0; i < count; i++) {
    size_t length = strlen(parts[i]);
    if (used + length + (i + 1 < count ? 1 : 0) >= size) return false;
    memcpy(output + used, parts[i], length);
    used += length;
    if (i + 1 < count) output[used++] = '/';
  }
  output[used] = 0;
  return true;
}

static const char* basename_of(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int split_words(char* line, char* words[], int maximum) {
  int count = 0;
  char* cursor = line;
  while (*cursor && count < maximum) {
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (!*cursor) break;
    char quote = 0;
    if (*cursor == '"' || *cursor == '\'') quote = *cursor++;
    words[count++] = cursor;
    if (quote) {
      while (*cursor && *cursor != quote) cursor++;
    } else {
      while (*cursor && *cursor != ' ' && *cursor != '\t') cursor++;
    }
    if (*cursor) *cursor++ = 0;
  }
  return count;
}

static char* trim_in_place(char* text) {
  while (*text == ' ' || *text == '\t') text++;
  char* end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *end = 0;
  return text;
}

static bool parse_bool_value(const char* value, bool* result) {
  if (!value || !result) return false;
  if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") ||
      !strcasecmp(value, "on") || !strcmp(value, "1")) {
    *result = true;
    return true;
  }
  if (!strcasecmp(value, "false") || !strcasecmp(value, "no") ||
      !strcasecmp(value, "off") || !strcmp(value, "0")) {
    *result = false;
    return true;
  }
  return false;
}

static bool parse_int_value(const char* value, int minimum, int maximum,
                            int* result) {
  if (!value || !*value || !result) return false;
  char* end = nullptr;
  long parsed = strtol(value, &end, 10);
  while (end && (*end == ' ' || *end == '\t')) end++;
  if (!end || *end || parsed < minimum || parsed > maximum) return false;
  *result = (int)parsed;
  return true;
}

static bool parse_octal_value(const char* text, uint32_t maximum,
                              uint32_t* result) {
  if (!text || !*text || !result) return false;
  char* end = nullptr;
  unsigned long value = strtoul(text, &end, 8);
  while (end && (*end == ' ' || *end == '\t')) end++;
  if (!end || *end || value > maximum) return false;
  *result = (uint32_t)value;
  return true;
}

static void command_help() {
  shell_print_help();
}

static void monitor_help() {
  output_text(
      "PDP-11 monitor commands (octal addresses and values):\r\n"
      "  P                  pause after the current instruction\r\n"
      "  S                  execute one instruction and remain paused\r\n"
      "  C                  continue execution\r\n"
      "  B                  show breakpoint\r\n"
      "  B012340            break when PC equals 012340\r\n"
      "  B clear            clear breakpoint\r\n"
      "  D00100             dump physical RAM (alias: MP)\r\n"
      "  D00100:00200       dump an inclusive physical range\r\n"
      "  MP00100            same as D (physical)\r\n"
      "  MI00100            dump MMU I-space (current run mode)\r\n"
      "  MI00100:00200      inclusive I-space range\r\n"
      "  MD00100            dump MMU D-space (current run mode)\r\n"
      "  MD00100:00200      inclusive D-space range (use for stacks)\r\n"
      "  U                  dump MMU registers and Unibus map\r\n"
      "  I                  dump RH70 registers (peek, no side effects)\r\n"
      "  H                  dump trace history to USB serial\r\n"
      "  T 1000             trace the next 1000 instructions to USB serial\r\n"
      "  W000100=012345     deposit one word in physical RAM\r\n"
      "  R0=012345          set R0-R5, SP, PC, PS, or FPSR\r\n"
      "  >                  return to the management shell\r\n"
      "  ?                  show this help\r\n");
}

static void dump_rp_registers() {
  static const uint16_t kBase = 0176700u;
  static const char* kNames[] = {
      "CS1", "WC", "BA", "DA", "CS2", "DS", "ER1", "AS", "LA", "DB", "MR",
      "DT", "SN", "OF", "DC", "CC", "ER2", "ER3", "EC1", "EC2", "BAE"};
  static constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);

  uint16_t values[kCount] = {};
  for (unsigned i = 0; i < kCount; i++) {
    if (!pdp_core::read_rp06_word((uint16_t)(kBase + i * 2u), &values[i])) {
      output_text("error: RP06/RH70 is unavailable\r\n");
      return;
    }
  }

  bool stopped = false;
  if (!pdp_core::get_rp06_operator_stop(&stopped))
    stopped = false;

  output_printf("RP0 %s RH70/RP06 registers (peek):\r\n",
                stopped ? "stopped/offline" : "started/online");
  for (unsigned i = 0; i < kCount; i++) {
    if ((i % 4) == 0) {
      if (i) output_text("\r\n");
      output_printf("  %06o:", (unsigned)(kBase + i * 2u));
    }
    output_printf(" %s=%06o", kNames[i], (unsigned)values[i]);
  }
  output_text("\r\n");

  bool deferred = false;
  int delay = 0;
  int cs1_polls = 0;
  int wc_polls = 0;
  if (pdp_core::get_rp06_deferred(&deferred, &delay, &cs1_polls, &wc_polls)) {
    output_printf("  deferred=%s delay=%d cs1_polls=%d wc_polls=%d\r\n",
                  deferred ? "yes" : "no", delay, cs1_polls, wc_polls);
  }

  uint16_t psw = 0;
  bool any_pending = false;
  uint8_t counts[8] = {};
  uint16_t vectors[8] = {};
  if (pdp_core::get_interrupt_summary(&psw, &any_pending, counts, vectors)) {
    output_printf("  CPU PC=%06o PS=%06o SPL=%u any_irq=%u queued:",
                  (unsigned)pdp_core::pc(), (unsigned)psw,
                  (unsigned)((psw >> 5) & 7), any_pending ? 1 : 0);
    bool printed = false;
    for (int level = 0; level < 8; level++) {
      if (!counts[level]) continue;
      output_printf(" BR%d=%06o", level, (unsigned)vectors[level]);
      if (counts[level] > 1)
        output_printf("(+%u)", (unsigned)(counts[level] - 1));
      printed = true;
    }
    if (!printed) output_text(" none");
    output_text("\r\n");
  }
}

static void command_rp(const char* action) {
  if (!action || !*action ||
      (!strcasecmp(action, "status") || !strcasecmp(action, "stat"))) {
    uint16_t ds = 0;
    uint16_t as = 0;
    bool stopped = false;
    if (!pdp_core::read_rp06_word(0176712, &ds) ||
        !pdp_core::read_rp06_word(0176716, &as) ||
        !pdp_core::get_rp06_operator_stop(&stopped)) {
      output_text("error: RP06/RH70 is unavailable\r\n");
      return;
    }
    output_printf("RP0 %s DS=%06o AS=%06o\r\n",
                  stopped ? "stopped/offline" : "started/online",
                  (unsigned)ds, (unsigned)as);
    return;
  }

  if (!strcasecmp(action, "regs") || !strcasecmp(action, "registers")) {
    dump_rp_registers();
    return;
  }

  if (!strcasecmp(action, "stop") || !strcasecmp(action, "offline")) {
    if (!pdp_core::set_rp06_operator_stop(true)) {
      output_text("error: RP06/RH70 is unavailable\r\n");
      return;
    }
    output_text("RP0 STOP asserted: MOL/DRY low\r\n");
    return;
  }

  if (!strcasecmp(action, "start") || !strcasecmp(action, "online")) {
    if (!pdp_core::set_rp06_operator_stop(false)) {
      output_text("error: RP06/RH70 is unavailable\r\n");
      return;
    }
    output_text("RP0 START asserted: MOL/DRY high\r\n");
    return;
  }

  output_text("usage: rp <stop|start|status|regs>\r\n");
}

static const char* rl_command_name(int command) {
  static const char* kNames[] = {
      "NOP", "WCHK", "GETSTAT", "SEEK", "RDHDR", "WRITE", "READ", "READNH"};
  if (command < 0 || command > 7) return "?";
  return kNames[command];
}

static void dump_rl_registers() {
  static const uint16_t kBase = 0174400u;
  // Unibus RL11: CSR/BAR/DAR/MPR only. BAE (0174410) is RLV12/Qbus;
  // kek leaves that address unimplemented so RSTS INIT does not treat
  // the controller as RLV12.
  static constexpr unsigned kCount = 4;

  uint16_t values[kCount] = {};
  for (unsigned i = 0; i < kCount; i++) {
    if (!pdp_core::read_rl02_word((uint16_t)(kBase + i * 2u), &values[i])) {
      output_text("error: RL11/RL02 is unavailable "
                  "(no DL0-DL3 media mounted)\r\n");
      return;
    }
  }

  output_text("RL11/RL02 registers (peek):\r\n");
  output_printf("  %06o: CSR=%06o BAR=%06o DAR=%06o MPR=%06o\r\n",
                (unsigned)kBase, (unsigned)values[0], (unsigned)values[1],
                (unsigned)values[2], (unsigned)values[3]);
  uint16_t bae = 0;
  if (pdp_core::read_rl02_word((uint16_t)(kBase + 8u), &bae))
    output_printf("  %06o: BAE=%06o\r\n", (unsigned)(kBase + 8u),
                  (unsigned)bae);
  else
    output_text("  BAE not present (Unibus RL11)\r\n");
  const uint16_t csr = values[0];
  output_printf("  unit=%u cmd=%u(%s) IE=%u CRDY=%u DRDY=%u ERR=%u\r\n",
                (unsigned)((csr >> 8) & 3), (unsigned)((csr >> 1) & 7),
                rl_command_name((csr >> 1) & 7), (csr & 0100) ? 1 : 0,
                (csr & 0200) ? 1 : 0, (csr & 0001) ? 1 : 0,
                (csr & 0100000) ? 1 : 0);

  int16_t track = 0;
  uint8_t head = 0;
  uint8_t sector = 0;
  if (pdp_core::get_rl02_position(&track, &head, &sector)) {
    output_printf("  position: track=%d head=%u sector=%u\r\n", (int)track,
                  (unsigned)head, (unsigned)sector);
  }

  bool deferred = false;
  int delay = 0;
  int polls = 0;
  int unit = 0;
  int command = 0;
  int irq_ticks = 0;
  if (pdp_core::get_rl02_deferred(&deferred, &delay, &polls, &unit, &command,
                                  &irq_ticks)) {
    output_printf(
        "  deferred=%s delay=%d polls=%d unit=%d cmd=%u(%s) irq_ticks=%d\r\n",
        deferred ? "yes" : "no", delay, polls, unit, (unsigned)command,
        rl_command_name(command), irq_ticks);
  }

  uint16_t psw = 0;
  bool any_pending = false;
  uint8_t counts[8] = {};
  uint16_t vectors[8] = {};
  if (pdp_core::get_interrupt_summary(&psw, &any_pending, counts, vectors)) {
    output_printf("  CPU PC=%06o PS=%06o SPL=%u any_irq=%u queued:",
                  (unsigned)pdp_core::pc(), (unsigned)psw,
                  (unsigned)((psw >> 5) & 7), any_pending ? 1 : 0);
    bool printed = false;
    for (int level = 0; level < 8; level++) {
      if (!counts[level]) continue;
      output_printf(" BR%d=%06o", level, (unsigned)vectors[level]);
      if (counts[level] > 1)
        output_printf("(+%u)", (unsigned)(counts[level] - 1));
      printed = true;
    }
    if (!printed) output_text(" none");
    if (counts[5] && vectors[5] == 0160)
      output_text(" (RL)");
    output_text("\r\n");
  }
}

static void command_rl(const char* action) {
  if (!action || !*action || !strcasecmp(action, "status") ||
      !strcasecmp(action, "stat") || !strcasecmp(action, "regs") ||
      !strcasecmp(action, "registers")) {
    dump_rl_registers();
    return;
  }
  output_text("usage: rl [status|regs]\r\n");
}

static void command_clock() {
  uint16_t csr = 0;
  uint32_t since = 0;
  bool irq_queued = false;
  if (!pdp_core::get_kw11l_summary(&csr, &since, &irq_queued)) {
    output_text("error: KW11-L is unavailable\r\n");
    return;
  }
  output_printf("KW11-L LKS@177546=%06o  IE=%u DONE=%u  instr_since_tick=%u  irq100=%u\r\n",
                (unsigned)csr, (csr & 0100) ? 1 : 0, (csr & 0200) ? 1 : 0,
                (unsigned)since, irq_queued ? 1 : 0);

  uint16_t psw = 0;
  bool any_pending = false;
  uint8_t counts[8] = {};
  uint16_t vectors[8] = {};
  if (pdp_core::get_interrupt_summary(&psw, &any_pending, counts, vectors)) {
    output_printf("  CPU PC=%06o PS=%06o SPL=%u any_irq=%u",
                  (unsigned)pdp_core::pc(), (unsigned)psw,
                  (unsigned)((psw >> 5) & 7), any_pending ? 1 : 0);
    if (counts[6] && vectors[6] == 0100)
      output_text(" BR6=000100 (KW11-L)");
    output_text("\r\n");
  }
}

static void monitor_state() {
  uint16_t next_address = pdp_core::reg16(7);
  uint16_t next_opcode = 0;
  char disassembly[128];
  bool have_next = pdp_core::next_instruction(&next_address, &next_opcode);
  bool have_disassembly = pdp_core::disassemble_next(disassembly,
                                                     sizeof(disassembly));
  output_printf(
      "state: PC=%06o R0=%06o R1=%06o R2=%06o R3=%06o "
      "R4=%06o R5=%06o SP=%06o PS=%06o FPSR=%06o",
      (unsigned)pdp_core::reg16(7),
      (unsigned)pdp_core::reg16(0), (unsigned)pdp_core::reg16(1),
      (unsigned)pdp_core::reg16(2), (unsigned)pdp_core::reg16(3),
      (unsigned)pdp_core::reg16(4), (unsigned)pdp_core::reg16(5),
      (unsigned)pdp_core::reg16(6), (unsigned)pdp_core::psw(),
      (unsigned)pdp_core::fpsr());
  if (have_next)
    output_printf(" NEXT=%06o:%06o  %s\r\n",
                  (unsigned)next_address, (unsigned)next_opcode,
                  have_disassembly ? disassembly : "???");
  else
    output_text(" NEXT=unavailable\r\n");
}

static bool parse_monitor_octal(const char* text, uint32_t maximum,
                                uint32_t* result) {
  return parse_octal_value(text, maximum, result);
}

enum class MonitorDumpSpace { Physical, ISpace, DSpace };

static const char* monitor_dump_space_name(MonitorDumpSpace space) {
  switch (space) {
    case MonitorDumpSpace::Physical: return "physical RAM";
    case MonitorDumpSpace::ISpace: return "MMU I-space";
    case MonitorDumpSpace::DSpace: return "MMU D-space";
  }
  return "memory";
}

static void monitor_dump(const char* argument, MonitorDumpSpace space) {
  // 22-bit physical (up to 2044KW); old 0757776 cap hid stacks above 256KW.
  static constexpr uint32_t LAST_RAM_WORD = 017777776u;
  static constexpr uint32_t LAST_LOGICAL_WORD = 0177776u;
  static constexpr uint32_t MAX_DUMP_WORDS = 512;
  const bool physical = space == MonitorDumpSpace::Physical;
  const uint32_t max_address = physical ? LAST_RAM_WORD : LAST_LOGICAL_WORD;

  char range[64];
  strncpy(range, argument ? argument : "", sizeof(range) - 1);
  range[sizeof(range) - 1] = 0;
  char* separator = strchr(range, ':');
  if (separator) *separator++ = 0;

  uint32_t first = 0;
  uint32_t last = 0;
  if (!parse_monitor_octal(trim_in_place(range), max_address, &first) ||
      (first & 1)) {
    output_printf("error: invalid even %s address\r\n",
                  monitor_dump_space_name(space));
    return;
  }
  if (separator) {
    if (!parse_monitor_octal(trim_in_place(separator), max_address, &last) ||
        (last & 1) || last < first) {
      output_text("error: invalid dump range\r\n");
      return;
    }
  } else {
    last = first + 30;
    if (last > max_address) last = max_address;
  }

  uint32_t words = ((last - first) / 2) + 1;
  if (words > MAX_DUMP_WORDS) {
    output_printf("error: dump is limited to %u words per command\r\n",
                  (unsigned)MAX_DUMP_WORDS);
    return;
  }

  for (uint32_t address = first; address <= last;) {
    uint16_t values[8] = {};
    unsigned count = 0;
    uint32_t line_address = address;
    while (count < 8 && address <= last) {
      bool ok = false;
      if (physical) {
        ok = pdp_core::read_physical_word(address, &values[count]);
      } else {
        ok = pdp_core::read_mmu_word((uint16_t)address, &values[count],
                                    space == MonitorDumpSpace::DSpace);
      }
      if (!ok) {
        if (physical)
          output_printf("error: %s examine failed at %08o\r\n",
                        monitor_dump_space_name(space), (unsigned)address);
        else
          output_printf("error: %s examine failed at %06o\r\n",
                        monitor_dump_space_name(space), (unsigned)address);
        return;
      }
      count++;
      address += 2;
    }

    if (physical)
      output_printf("%08o:", (unsigned)line_address);
    else
      output_printf("%06o:", (unsigned)line_address);
    for (unsigned i = 0; i < 8; i++) {
      if (i < count)
        output_printf(" %06o", (unsigned)values[i]);
      else
        output_text("       ");
    }
    output_text("  ");
    for (unsigned i = 0; i < count; i++) {
      uint8_t low = (uint8_t)(values[i] & 0xff);
      uint8_t high = (uint8_t)(values[i] >> 8);
      output_char(low >= 0x20 && low <= 0x7e ? low : ' ');
      output_char(high >= 0x20 && high <= 0x7e ? high : ' ');
    }
    for (unsigned i = count; i < 8; i++) output_text("  ");
    output_text("\r\n");
  }
}

static const char* monitor_run_mode_name(int run_mode) {
  switch (run_mode) {
    case 0: return "Kernel";
    case 1: return "Supervisor";
    case 3: return "User";
    default: return "Unknown";
  }
}

static void monitor_dump_mmu_unibus() {
  uint16_t mmr0 = 0;
  uint16_t mmr1 = 0;
  uint16_t mmr2 = 0;
  uint16_t mmr3 = 0;
  uint16_t cpuerr = 0;
  uint16_t pir = 0;
  uint32_t io_base = 0;

  if (!pdp_core::get_mmu_summary(&mmr0, &mmr1, &mmr2, &mmr3,
                                 &cpuerr, &pir, &io_base)) {
    output_text("error: MMU state unavailable for this CPU core\r\n");
    return;
  }

  output_printf("MMU: MMR0=%06o MMR1=%06o MMR2=%06o MMR3=%06o\r\n",
                (unsigned)mmr0, (unsigned)mmr1, (unsigned)mmr2,
                (unsigned)mmr3);
  output_printf("     CPUERR=%06o PIR=%06o IOBASE=%08lo\r\n",
                (unsigned)cpuerr, (unsigned)pir, (unsigned long)io_base);

  static const int kRunModes[] = { 0, 1, 3 };
  for (unsigned i = 0; i < sizeof(kRunModes) / sizeof(kRunModes[0]); i++) {
    int mode = kRunModes[i];
    output_printf("%s PAR/PDR:\r\n", monitor_run_mode_name(mode));
    output_text("  pg  I-PAR  I-PDR  I-phys    D-PAR  D-PDR  D-phys\r\n");
    for (int page = 0; page < 8; page++) {
      uint16_t i_par = 0;
      uint16_t i_pdr = 0;
      uint16_t d_par = 0;
      uint16_t d_pdr = 0;
      uint32_t i_phys = 0;
      uint32_t d_phys = 0;
      bool i_ok = pdp_core::get_mmu_page(mode, false, page, &i_par, &i_pdr,
                                         &i_phys);
      bool d_ok = pdp_core::get_mmu_page(mode, true, page, &d_par, &d_pdr,
                                         &d_phys);
      if (!i_ok || !d_ok) {
        output_printf("  %d  <unavailable>\r\n", page);
        continue;
      }
      output_printf("  %d  %06o %06o %08lo  %06o %06o %08lo\r\n",
                    page, (unsigned)i_par, (unsigned)i_pdr,
                    (unsigned long)i_phys, (unsigned)d_par,
                    (unsigned)d_pdr, (unsigned long)d_phys);
    }
  }

  output_text("Unibus map physical bases:\r\n");
  for (int entry = 0; entry < 32; entry++) {
    uint32_t base = 0;
    if (!pdp_core::get_unibus_map_entry(entry, &base)) {
      output_printf("  %02o:<unavailable>\r\n", entry);
      return;
    }
    output_printf("  %02o:%08lo", entry, (unsigned long)base);
    if ((entry & 3) == 3) output_text("\r\n");
  }
  output_text("Unibus map (NPR devices): phys=(map[pg]+(uba&017777))&017777777\r\n");
  output_text("RH70 DMA: phys=(BAE<<16)|BA  (no Unibus map)\r\n");
}

static void monitor_dump_rh70() {
  // RH70/RP06 CSR block at 0176700..0176750 (Unibus addresses).
  static const uint16_t kBase = 0176700u;
  static const uint16_t kEnd = 0176752u;  // exclusive
  static const char* kNames[] = {
      "CS1", "WC", "BA", "DA", "CS2", "DS", "ER1", "AS", "LA", "DB", "MR",
      "DT", "SN", "OF", "DC", "CC", "ER2", "ER3", "EC1", "EC2", "BAE"};
  static constexpr unsigned kCount =
      sizeof(kNames) / sizeof(kNames[0]);

  uint16_t values[kCount];
  for (unsigned i = 0; i < kCount; i++) {
    uint16_t addr = (uint16_t)(kBase + i * 2u);
    if (addr >= kEnd || !pdp_core::read_rp06_word(addr, &values[i])) {
      output_text("error: RH70 registers unavailable\r\n");
      return;
    }
  }

  output_text("RH70 (peek):\r\n");
  for (unsigned i = 0; i < kCount; i++) {
    if ((i % 4) == 0) {
      if (i) output_text("\r\n");
      output_printf("  %06o:", (unsigned)(kBase + i * 2u));
    }
    output_printf(" %s=%06o", kNames[i], (unsigned)values[i]);
  }
  output_text("\r\n");

  bool deferred = false;
  int delay = 0;
  int cs1_polls = 0;
  int wc_polls = 0;
  if (pdp_core::get_rp06_deferred(&deferred, &delay, &cs1_polls, &wc_polls)) {
    output_printf("  deferred=%s delay=%d cs1_polls=%d wc_polls=%d\r\n",
                  deferred ? "yes" : "no", delay, cs1_polls, wc_polls);
  }
}

static void monitor_write(const char* argument) {
  char assignment[64];
  strncpy(assignment, argument ? argument : "", sizeof(assignment) - 1);
  assignment[sizeof(assignment) - 1] = 0;
  char* equals = strchr(assignment, '=');
  if (!equals) {
    output_text("usage: W<address>=<word>\r\n");
    return;
  }
  *equals++ = 0;

  uint32_t address = 0;
  uint32_t value = 0;
  if (!parse_monitor_octal(trim_in_place(assignment), 017777776u, &address) ||
      (address & 1) ||
      !parse_monitor_octal(trim_in_place(equals), 0177777u, &value)) {
    output_text("error: invalid octal address or word\r\n");
    return;
  }
  if (!pdp_core::write_physical_word(address, (uint16_t)value)) {
    output_text("error: memory deposit failed\r\n");
    return;
  }
  output_printf("%08o=%06o\r\n", (unsigned)address, (unsigned)value);
}

static void monitor_write_register(const char* command) {
  char assignment[64];
  strncpy(assignment, command ? command : "", sizeof(assignment) - 1);
  assignment[sizeof(assignment) - 1] = 0;
  char* equals = strchr(assignment, '=');
  if (!equals) {
    output_text("usage: R0=012345, SP=012345, PC=012345, PS=012345, or FPSR=012345\r\n");
    return;
  }
  *equals++ = 0;

  char* name = trim_in_place(assignment);
  char* value_text = trim_in_place(equals);
  uint32_t value = 0;
  if (!parse_monitor_octal(value_text, 0177777u, &value)) {
    output_text("error: invalid octal register value\r\n");
    return;
  }

  int reg = -1;
  if ((name[0] == 'R' || name[0] == 'r') && name[1] >= '0' &&
      name[1] <= '5' && name[2] == 0) {
    reg = name[1] - '0';
  } else if (!strcasecmp(name, "SP")) {
    reg = 6;
  } else if (!strcasecmp(name, "PC")) {
    reg = 7;
  } else if (!strcasecmp(name, "PS") || !strcasecmp(name, "PSW")) {
    if (!pdp_core::set_psw((uint16_t)value)) {
      output_text("error: PS write not supported by this CPU core\r\n");
      return;
    }
    output_printf("PS=%06o\r\n", (unsigned)value);
    return;
  } else if (!strcasecmp(name, "FPSR") || !strcasecmp(name, "FPS")) {
    if (!pdp_core::set_fpsr((uint16_t)value)) {
      output_text("error: FPSR write not supported by this CPU core\r\n");
      return;
    }
    output_printf("FPSR=%06o\r\n", (unsigned)value);
    return;
  } else {
    output_text("error: register must be R0-R5, SP, PC, PS, or FPSR\r\n");
    return;
  }

  if (!pdp_core::set_reg16(reg, (uint16_t)value)) {
    output_text("error: register write not supported by this CPU core\r\n");
    return;
  }

  static const char* kNames[] = { "R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC" };
  output_printf("%s=%06o\r\n", kNames[reg], (unsigned)value);
}

static void monitor_trace(const char* argument) {
  char text[32];
  strncpy(text, argument ? argument : "", sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  char* count_text = trim_in_place(text);
  char* end = nullptr;
  unsigned long count = strtoul(count_text, &end, 10);
  while (end && (*end == ' ' || *end == '\t')) end++;
  if (!count_text[0] || !end || *end || count > 1000000UL) {
    output_text("usage: T <decimal-instruction-count 0..1000000>\r\n");
    return;
  }
  pdp_core::monitor_trace_next((uint32_t)count);
  output_printf("instruction trace count set to %lu; output goes to USB serial\r\n",
                count);
}

static void execute_monitor_command(char* line) {
  char* command = trim_in_place(line);
  if (!*command) {
    prompt();
    return;
  }
  if (!strcmp(command, ">")) {
    g_monitor_mode = false;
    output_text("Returned to management shell.\r\n");
    prompt();
    return;
  }
  if (!strcasecmp(command, "?") || !strcasecmp(command, "help")) {
    monitor_help();
  } else if (!strcasecmp(command, "P")) {
    pdp_core::monitor_pause();
    monitor_state();
  } else if (!strcasecmp(command, "S")) {
    if (pdp_core::monitor_step() == 0)
      output_text("error: CPU did not execute an instruction\r\n");
    monitor_state();
  } else if (!strcasecmp(command, "C")) {
    pdp_core::monitor_continue();
    output_text("CPU running\r\n");
  } else if (command[0] == 'B' || command[0] == 'b') {
    char* arg = trim_in_place(command + 1);
    if (!arg || !*arg) {
      if (!pdp_core::monitor_break_active()) {
        output_text("no breakpoint set (B<octal-pc>; B clear)\r\n");
      } else {
        output_printf("armed: PC=%06o\r\n",
                      (unsigned)pdp_core::monitor_break_pc());
      }
    } else if (!strcasecmp(arg, "clear") || !strcasecmp(arg, "-") ||
               !strcasecmp(arg, "off")) {
      pdp_core::monitor_break_clear();
      output_text("breakpoint cleared\r\n");
    } else {
      char* end = nullptr;
      unsigned long pc = strtoul(arg, &end, 8);
      while (end && (*end == ' ' || *end == '\t')) end++;
      if (!end || *end || (pc & 1UL) || pc > 0177777UL) {
        output_text("usage: B<octal-pc> | B clear\r\n");
      } else if (pdp_core::monitor_break_set_pc((uint16_t)pc)) {
        output_printf("breakpoint: PC=%06lo\r\n", pc);
      } else {
        output_text("error: could not arm PC breakpoint\r\n");
      }
    }
  } else if (!strcasecmp(command, "H")) {
    pdp_core::monitor_dump_history();
    output_text("trace history requested; output goes to USB serial\r\n");
  } else if (command[0] == 'D' || command[0] == 'd') {
    monitor_dump(command + 1, MonitorDumpSpace::Physical);
  } else if ((command[0] == 'M' || command[0] == 'm') &&
             command[1] != 0) {
    const char kind = command[1];
    if (kind == 'P' || kind == 'p') {
      monitor_dump(command + 2, MonitorDumpSpace::Physical);
    } else if (kind == 'I' || kind == 'i') {
      monitor_dump(command + 2, MonitorDumpSpace::ISpace);
    } else if (kind == 'D' || kind == 'd') {
      monitor_dump(command + 2, MonitorDumpSpace::DSpace);
    } else {
      output_text("usage: MI/MD/MP<addr>[:<addr>]  (or D for physical)\r\n");
    }
  } else if (command[0] == 'M' || command[0] == 'm') {
    output_text("usage: MI/MD/MP<addr>[:<addr>]  (or D for physical)\r\n");
  } else if (!strcasecmp(command, "U")) {
    monitor_dump_mmu_unibus();
  } else if (!strcasecmp(command, "I")) {
    monitor_dump_rh70();
  } else if (command[0] == 'T' || command[0] == 't') {
    monitor_trace(command + 1);
  } else if (command[0] == 'W' || command[0] == 'w') {
    monitor_write(command + 1);
  } else if ((command[0] == 'R' || command[0] == 'r') ||
             !strncasecmp(command, "SP", 2) ||
             !strncasecmp(command, "PC", 2) ||
             !strncasecmp(command, "PS", 2) ||
             !strncasecmp(command, "FPSR", 4) ||
             !strncasecmp(command, "FPS", 3)) {
    monitor_write_register(command);
  } else {
    output_text("unknown monitor command (type ?)\r\n");
  }
  prompt();
}

static void command_ls(const char* argument) {
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument && *argument ? argument : g_cwd,
                      path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File entry = SD_FS.open(path, "r");
  if (!entry) {
    output_printf("error: cannot open %s\r\n", path);
    return;
  }
  if (!entry.isDirectory()) {
    output_printf("%10lu  %s\r\n", (unsigned long)entry.size(),
                  basename_of(path));
    entry.close();
    return;
  }
  File child;
  while ((child = entry.openNextFile())) {
    const char* name = basename_of(child.name());
    if (child.isDirectory())
      output_printf("     <DIR>  %s/\r\n", name);
    else
      output_printf("%10lu  %s\r\n", (unsigned long)child.size(), name);
    child.close();
  }
  entry.close();
}

static void command_cat(const char* argument) {
  if (!argument) {
    output_text("usage: cat <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File file = SD_FS.open(path, "r");
  if (!file || file.isDirectory()) {
    output_printf("error: cannot read file: %s\r\n", path);
    if (file) file.close();
    return;
  }

  unsigned scan_lines = 0;
  bool scan_previous_cr = false;
  bool binary = false;
  while (file.available() && scan_lines < 100) {
    int value = file.read();
    if (value < 0) break;
    uint8_t ch = (uint8_t)value;
    if (ch == '\r') {
      scan_lines++;
      scan_previous_cr = true;
    } else if (ch == '\n') {
      if (!scan_previous_cr) scan_lines++;
      scan_previous_cr = false;
    } else {
      scan_previous_cr = false;
      if (ch != '\t' && (ch < 0x20 || ch > 0x7e)) {
        binary = true;
        break;
      }
    }
  }
  if (binary) {
    file.close();
    output_text("error: file is binary\r\n");
    return;
  }
  if (!file.seek(0)) {
    file.close();
    output_printf("error: cannot rewind file: %s\r\n", path);
    return;
  }

  unsigned lines = 0;
  bool previous_cr = false;
  bool output_ok = true;
  while (file.available() && lines < 100 && output_ok) {
    int value = file.read();
    if (value < 0) break;
    uint8_t ch = (uint8_t)value;
    if (ch == '\r') {
      output_ok = output_char_wait('\r') && output_char_wait('\n');
      lines++;
      previous_cr = true;
    } else if (ch == '\n') {
      if (!previous_cr) {
        output_ok = output_char_wait('\r') && output_char_wait('\n');
        lines++;
      }
      previous_cr = false;
    } else {
      previous_cr = false;
      output_ok = output_char_wait(ch);
    }
  }
  file.close();
  if (output_ok && lines >= 100)
    output_text("[output limited to 100 lines]\r\n");
}

static void command_cd(const char* argument) {
  if (!argument) {
    output_text("usage: cd <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File directory = SD_FS.open(path, "r");
  if (!directory || !directory.isDirectory()) {
    output_printf("error: not a directory: %s\r\n", path);
    if (directory) directory.close();
    return;
  }
  directory.close();
  strcpy(g_cwd, path);
}

static void command_rm(const char* argument) {
  if (!argument) {
    output_text("usage: rm <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  if (mounted_path(path)) {
    output_text("error: file is mounted by the emulator\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File file = SD_FS.open(path, "r");
  if (!file) {
    output_printf("error: file not found: %s\r\n", path);
    return;
  }
  bool directory = file.isDirectory();
  file.close();
  if (directory) {
    output_text("error: rm removes files only\r\n");
    return;
  }
  output_printf(SD_FS.remove(path) ? "removed %s\r\n"
                                    : "error: remove failed: %s\r\n", path);
}

static void command_mv(const char* source_arg, const char* destination_arg) {
  char source[SHELL_PATH_MAX], destination[SHELL_PATH_MAX];
  if (!source_arg || !destination_arg ||
      !normalize_path(source_arg, source, sizeof(source)) ||
      !normalize_path(destination_arg, destination, sizeof(destination))) {
    output_text("usage: mv <source> <destination>\r\n");
    return;
  }
  if (mounted_path(source) || mounted_path(destination)) {
    output_text("error: source or destination is mounted\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  if (!SD_FS.exists(source)) {
    output_printf("error: file not found: %s\r\n", source);
    return;
  }
  if (SD_FS.exists(destination)) {
    output_printf("error: destination exists: %s\r\n", destination);
    return;
  }
  output_printf(SD_FS.rename(source, destination) ? "moved %s -> %s\r\n"
                                                   : "error: move failed\r\n",
                source, destination);
}

static void command_cp(const char* source_arg, const char* destination_arg) {
  char source[SHELL_PATH_MAX], destination[SHELL_PATH_MAX];
  if (!source_arg || !destination_arg ||
      !normalize_path(source_arg, source, sizeof(source)) ||
      !normalize_path(destination_arg, destination, sizeof(destination))) {
    output_text("usage: cp <source> <destination>\r\n");
    return;
  }
  if (mounted_path(source) || mounted_path(destination)) {
    output_text("error: source or destination is mounted\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  if (SD_FS.exists(destination)) {
    output_printf("error: destination exists: %s\r\n", destination);
    return;
  }
  File source_file = SD_FS.open(source, "r");
  if (!source_file || source_file.isDirectory()) {
    output_printf("error: cannot read file: %s\r\n", source);
    if (source_file) source_file.close();
    return;
  }
  File destination_file = SD_FS.open(destination, "w");
  if (!destination_file) {
    source_file.close();
    output_printf("error: cannot create: %s\r\n", destination);
    return;
  }
  bool ok = true;
  while (source_file.available()) {
    size_t count = source_file.read(g_file_buffer, sizeof(g_file_buffer));
    if (!count) break;
    if (destination_file.write(g_file_buffer, count) != count) {
      ok = false;
      break;
    }
  }
  destination_file.flush();
  destination_file.close();
  source_file.close();
  if (!ok) {
    SD_FS.remove(destination);
    output_text("error: copy failed; partial destination removed\r\n");
  } else {
    output_printf("copied %s -> %s\r\n", source, destination);
  }
}

static const char* slot_label(int slot) {
  if (slot == DRIVE_RK0) return "RK0";
  if (slot == DRIVE_RP0) return "RP0";
  if (slot == DRIVE_DU0) return "DU0";
  if (slot == DRIVE_A) return "RL0";
  if (slot == DRIVE_B) return "RL1";
  if (slot == DRIVE_C) return "RL2";
  return "RL3";
}

static bool vpdp_media_list(int index, MediaUnitInfo* out) {
  if (!out || index < 0 || index >= DRIVE_COUNT) return false;
  memset(out, 0, sizeof(*out));
  strncpy(out->name, slot_label(index), sizeof(out->name) - 1);
  if (!disk_is_mounted(index)) {
    out->mounted = false;
    return true;
  }
  out->mounted = true;
  strncpy(out->path, disk_path(index), sizeof(out->path) - 1);
  out->size_bytes = disk_size_bytes(index);
  out->readonly = disk_is_readonly(index);
  if (index >= DRIVE_A && index <= DRIVE_D)
    strncpy(out->extra, disk_rl_mounted_media_type(index),
            sizeof(out->extra) - 1);
  return true;
}

static void command_switches(const char* argument) {
  uint16_t value = 0;
  if (!pdp_core::get_console_switches(&value)) {
    output_text("error: console switch register is unavailable\r\n");
    return;
  }

  if (!argument || !*argument) {
    output_printf("switches=%06o\r\n", (unsigned)value);
    return;
  }

  char buffer[32];
  strncpy(buffer, argument, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = 0;
  char* equals = strchr(buffer, '=');
  if (equals) {
    *equals++ = 0;
    char* bit_text = trim_in_place(buffer);
    char* state_text = trim_in_place(equals);
    int bit = -1;
    bool state = false;
    if (!parse_int_value(bit_text, 0, 15, &bit) ||
        !parse_bool_value(state_text, &state)) {
      output_text("usage: switches [octal|bit=0|1]\r\n");
      return;
    }
    if (!pdp_core::set_console_switch(bit, state)) {
      output_text("error: cannot set console switch\r\n");
      return;
    }
  } else {
    uint32_t new_value = 0;
    if (!parse_octal_value(trim_in_place(buffer), 0177777u, &new_value)) {
      output_text("usage: switches [octal|bit=0|1]\r\n");
      return;
    }
    if (!pdp_core::set_console_switches((uint16_t)new_value)) {
      output_text("error: cannot set console switches\r\n");
      return;
    }
  }

  if (!pdp_core::get_console_switches(&value)) {
    output_text("error: console switch register is unavailable\r\n");
    return;
  }
  output_printf("switches=%06o\r\n", (unsigned)value);
}

static void command_lights() {
  uint16_t address = 0;
  uint32_t physical_address = 0;
  uint16_t data = 0;
  uint16_t leds = 0;
  bool data_valid = false;
  if (!pdp_core::get_console_lights(&address, &physical_address, &data,
                                    &data_valid, &leds)) {
    output_text("error: console lights are unavailable\r\n");
    return;
  }
  output_printf("address=%06o physical=%08o data=",
                (unsigned)address, (unsigned)physical_address);
  if (data_valid)
    output_printf("%06o", (unsigned)data);
  else
    output_text("------");
  output_printf(" leds=%06o\r\n", (unsigned)leds);
}

static void command_tty_stats() {
  uint32_t tx_chars = 0, tx_ready = 0, tx_irq_q = 0, tx_irq_uq = 0;
  uint32_t rx_chars = 0, rx_irq_q = 0, rx_irq_uq = 0;
  uint8_t last_tx = 0, tx_busy = 0;
  uint32_t last_tx_ms = 0, last_tx_ready_ms = 0, trace_rem = 0;
  uint16_t tks = 0, tkb = 0, tps = 0, tpb = 0;
  kek_tty_get_stats(&tx_chars, &tx_ready, &tx_irq_q, &tx_irq_uq,
                    &rx_chars, &rx_irq_q, &rx_irq_uq,
                    &last_tx, &last_tx_ms, &last_tx_ready_ms, &trace_rem,
                    &tks, &tkb, &tps, &tpb, &tx_busy);

  uint32_t tft_pend = 0, tft_drop = 0;
  uint32_t tel_pend = 0, tel_drop = 0;
  uint32_t ser_pend = 0, ser_drop = 0;
  console_output_stats(&tft_pend, &tft_drop);
  telnet_output_stats(&tel_pend, &tel_drop);
  kl11::serial_output_stats(&ser_pend, &ser_drop);

  output_printf(
      "tty tx=%lu txready=%lu irq64 q/u=%lu/%lu rx=%lu irq60 q/u=%lu/%lu\r\n",
      (unsigned long)tx_chars, (unsigned long)tx_ready,
      (unsigned long)tx_irq_q, (unsigned long)tx_irq_uq,
      (unsigned long)rx_chars, (unsigned long)rx_irq_q,
      (unsigned long)rx_irq_uq);
  output_printf(
      "  TKS=%06o TKB=%06o TPS=%06o TPB=%06o busy=%u last_tx=%03o\r\n",
      (unsigned)tks, (unsigned)tkb, (unsigned)tps, (unsigned)tpb,
      (unsigned)tx_busy, (unsigned)last_tx);
  output_printf(
      "  last_tx_ms=%lu last_ready_ms=%lu trace=%lu\r\n",
      (unsigned long)last_tx_ms, (unsigned long)last_tx_ready_ms,
      (unsigned long)trace_rem);
  output_printf(
      "  FIFOs tft pend/drop=%lu/%lu telnet=%lu/%lu serial=%lu/%lu\r\n",
      (unsigned long)tft_pend, (unsigned long)tft_drop,
      (unsigned long)tel_pend, (unsigned long)tel_drop,
      (unsigned long)ser_pend, (unsigned long)ser_drop);
}

static bool unit_is_rl(const char* unit) {
  return unit && (!strncasecmp(unit, "RL", 2) || !strncasecmp(unit, "DL", 2));
}

static int unit_slot(const char* unit) {
  if (!unit) return -1;
  if (!strcasecmp(unit, "RP0")) return DRIVE_RP0;
  if (!strcasecmp(unit, "RK0")) return DRIVE_RK0;
  if (!strcasecmp(unit, "DU0")) return DRIVE_DU0;
  if (!strcasecmp(unit, "RL0") || !strcasecmp(unit, "DL0"))
    return DRIVE_A;
  if (!strcasecmp(unit, "RL1") || !strcasecmp(unit, "DL1")) return DRIVE_B;
  if (!strcasecmp(unit, "RL2") || !strcasecmp(unit, "DL2")) return DRIVE_C;
  if (!strcasecmp(unit, "RL3") || !strcasecmp(unit, "DL3")) return DRIVE_D;
  return -1;
}

static void notify_media(int /*slot*/, bool /*mounted*/) {
  // Kek backends read live disk_* mount state; no scaffold notify needed.
}

static const char* vpdp_mount_usage() {
  return "usage: mount <RL0-RL3|RK0|RP0|DU0> <path> [ro]\r\n";
}
static const char* vpdp_create_usage() {
  return "usage: create <rk|rl01|rl02|rp04|rp05|rp06> <path>\r\n";
}

static bool vpdp_media_mount(const char* unit, const char* path_arg,
                             bool readonly, char* err, size_t errlen) {
  int slot = unit_slot(unit);
  if (slot < 0 || !path_arg) {
    snprintf(err, errlen, "%s", vpdp_mount_usage());
    return false;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(path_arg, path, sizeof(path))) {
    snprintf(err, errlen, "invalid path");
    return false;
  }
  if (disk_is_mounted(slot)) {
    snprintf(err, errlen, "%s is mounted; dismount it first", slot_label(slot));
    return false;
  }
  if (!disk_mount_mode(slot, path, readonly)) {
    snprintf(err, errlen, "mount failed: %s: %s", path,
             disk_last_error()[0] ? disk_last_error() : "unknown error");
    return false;
  }
  if (unit_is_rl(unit) && !disk_validate_rl_mounted(slot)) {
    uint32_t bytes = disk_size_bytes(slot);
    disk_dismount(slot);
    notify_media(slot, false);
    snprintf(err, errlen,
             "invalid RL image size: %lu bytes; expected RL01=%lu or RL02=%lu",
             (unsigned long)bytes, (unsigned long)DISK_RL01_IMAGE_BYTES,
             (unsigned long)DISK_RL02_IMAGE_BYTES);
    return false;
  }
  notify_media(slot, true);
  snprintf(err, errlen, "mounted %s on %s (%s)\r\n", path, slot_label(slot),
           disk_is_readonly(slot) ? "read-only" : "read-write");
  return true;
}

static bool vpdp_media_dismount(const char* unit, char* err, size_t errlen) {
  int slot = unit_slot(unit);
  if (slot < 0) {
    snprintf(err, errlen, "%s", "usage: dismount <RL0-RL3|RK0|RP0|DU0>\r\n");
    return false;
  }
  if (!disk_is_mounted(slot)) {
    snprintf(err, errlen, "%s is already empty\r\n", slot_label(slot));
    return true;
  }
  disk_dismount(slot);
  notify_media(slot, false);
  snprintf(err, errlen, "dismounted %s\r\n", slot_label(slot));
  return true;
}

static bool vpdp_media_create(const char* type, const char* path_arg,
                              char* err, size_t errlen) {
  uint32_t bytes = 0;
  if (type && !strcasecmp(type, "rk")) bytes = 2494464u;
  else if (type && !strcasecmp(type, "rl01")) bytes = 5242880u;
  else if (type && !strcasecmp(type, "rl02")) bytes = 10485760u;
  else if (type && !strcasecmp(type, "rp04"))
    bytes = uint32_t(RP04_CYL) * RP_HEADS * RP_SECTORS * RP_BYTES_PER_SEC;
  else if (type && !strcasecmp(type, "rp05"))
    bytes = uint32_t(RP05_CYL) * RP_HEADS * RP_SECTORS * RP_BYTES_PER_SEC;
  else if (type && !strcasecmp(type, "rp06"))
    bytes = uint32_t(RP06_CYL) * RP_HEADS * RP_SECTORS * RP_BYTES_PER_SEC;
  if (!bytes || !path_arg) {
    snprintf(err, errlen, "%s", vpdp_create_usage());
    return false;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(path_arg, path, sizeof(path))) {
    snprintf(err, errlen, "invalid path");
    return false;
  }
  if (mounted_path(path)) {
    snprintf(err, errlen, "path is mounted");
    return false;
  }
  shell_out_printf("creating %s (%lu bytes)...\r\n", path, (unsigned long)bytes);
  SD_FTP_StorageGuard guard;
  if (SD_FS.exists(path)) {
    snprintf(err, errlen, "file already exists: %s", path);
    return false;
  }
  File file = SD_FS.open(path, "w");
  if (!file) {
    snprintf(err, errlen, "cannot create: %s", path);
    return false;
  }
  memset(g_file_buffer, 0, sizeof(g_file_buffer));
  uint32_t remaining = bytes;
  bool ok = true;
  while (remaining) {
    size_t count = remaining > sizeof(g_file_buffer)
                     ? sizeof(g_file_buffer) : remaining;
    if (file.write(g_file_buffer, count) != count) {
      ok = false;
      break;
    }
    remaining -= count;
    if ((remaining & 0x3ffffu) == 0) delay(1);
  }
  file.flush();
  file.close();
  if (!ok) {
    SD_FS.remove(path);
    snprintf(err, errlen, "create failed; partial file removed");
    return false;
  }
  snprintf(err, errlen, "created %s\r\n", path);
  return true;
}

static bool vpdp_path_protected(const char* path) {
  return mounted_path(path);
}

static void execute_command(char* line) {
  if (g_monitor_mode) {
    execute_monitor_command(line);
    return;
  }
  char* command_start = trim_in_place(line);
  char* words[8];
  int count = split_words(command_start, words, 8);
  if (count == 0) {
    prompt();
    return;
  }
  if (!shell_dispatch(count, words))
    output_printf("unknown command: %s (type help)\r\n", words[0]);
  if (g_active) prompt();
}

extern void vpdp_register_shell_settings();

static void cmd_help(int, char**) { command_help(); }
static void cmd_pwd(int, char**) { output_printf("%s\r\n", g_cwd); }
static void cmd_cd(int argc, char** argv) {
  command_cd(argc > 1 ? argv[1] : nullptr);
}
static void cmd_ls(int argc, char** argv) {
  command_ls(argc > 1 ? argv[1] : nullptr);
}
static void cmd_cat(int argc, char** argv) {
  command_cat(argc > 1 ? argv[1] : nullptr);
}
static void cmd_rm(int argc, char** argv) {
  command_rm(argc > 1 ? argv[1] : nullptr);
}
static void cmd_mv(int argc, char** argv) {
  command_mv(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
}
static void cmd_cp(int argc, char** argv) {
  command_cp(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
}
static void cmd_rp(int argc, char** argv) {
  command_rp(argc > 1 ? argv[1] : nullptr);
}
static void cmd_rl(int argc, char** argv) {
  command_rl(argc > 1 ? argv[1] : nullptr);
}
static void cmd_clock(int, char**) { command_clock(); }
static void cmd_switches(int argc, char** argv) {
  command_switches(argc > 1 ? argv[1] : nullptr);
}
static void cmd_lights(int, char**) { command_lights(); }
static void cmd_tty(int, char**) { command_tty_stats(); }
static void cmd_monitor(int, char**) {
  g_monitor_mode = true;
  output_printf("PDP-11 monitor; CPU is currently %s.\r\n",
                pdp_core::monitor_paused() ? "paused" : "running");
  monitor_help();
}
static void cmd_exit(int, char**) {
  output_text("Returning Telnet to the PDP-11 console.\r\n");
  g_active = false;
  LOG("telnet shell: returned to PDP console");
}

static bool vpdp_restart(char* err, size_t errlen) {
  if (emu_control::submit("PDP;REBOOT")) return true;
  snprintf(err, errlen, "emulator command queue full");
  return false;
}
static const char* vpdp_restart_help() {
  return "reload config, remount, zero RAM, cold boot";
}

static void register_vpdp_shell() {
  static const char* help_aliases[] = { "?", nullptr };
  static const char* rl_aliases[] = { "dl", nullptr };
  static const char* clock_aliases[] = { "kw11l", "lks", nullptr };
  static const char* switch_aliases[] = { "switch", nullptr };

  shell_register("help", cmd_help,
                 "help                        show this list",
                 help_aliases, "File commands");
  shell_register("pwd", cmd_pwd,
                 "pwd                         show current SD directory",
                 nullptr, "File commands");
  shell_register("cd", cmd_cd,
                 "cd <path>                   change current directory",
                 nullptr, "File commands");
  shell_register("ls", cmd_ls,
                 "ls [path]                   list a file or directory",
                 nullptr, "File commands");
  shell_register("cat", cmd_cat,
                 "cat <path>                  display the first 100 lines",
                 nullptr, "File commands");
  shell_register("rm", cmd_rm,
                 "rm <path>                   remove a file",
                 nullptr, "File commands");
  shell_register("mv", cmd_mv,
                 "mv <source> <destination>   rename or move a file",
                 nullptr, "File commands");
  shell_register("cp", cmd_cp,
                 "cp <source> <destination>   copy a file",
                 nullptr, "File commands");

  static MediaOps media = {
      vpdp_media_list,
      vpdp_media_mount,
      vpdp_media_dismount,
      vpdp_media_create,
      vpdp_path_protected,
      vpdp_mount_usage,
      vpdp_create_usage,
  };
  shell_set_media_ops(&media);
  shell_register_media_commands();

  shell_register_set_command();
  vpdp_register_shell_settings();

  shell_register("rp", cmd_rp,
                 "rp <stop|start|status|regs> toggle RP0 STOP or dump RH70/RP06 state",
                 nullptr, "Emulator commands");
  shell_register("rl", cmd_rl,
                 "rl [status|regs]            dump RL11/RL02 CSR state (peek)",
                 rl_aliases, "Emulator commands");
  shell_register("clock", cmd_clock,
                 "clock                       dump KW11-L line-clock state (peek)",
                 clock_aliases, "Emulator commands");
  shell_register("lights", cmd_lights,
                 "lights                      show console address/data lights",
                 nullptr, "Emulator commands");
  shell_register("switches", cmd_switches,
                 "switches [octal|bit=0|1]    show or set console switch register",
                 switch_aliases, "Emulator commands");
  shell_register("tty", cmd_tty,
                 "tty                         show console TTY counters",
                 nullptr, "Emulator commands");
  shell_register("monitor", cmd_monitor,
                 "monitor                     enter PDP-11 front-panel monitor",
                 nullptr, "Emulator commands");

  static GuestControlOps guest = { vpdp_restart, vpdp_restart_help };
  shell_set_guest_control_ops(&guest);
  shell_register_guest_control_commands();

  shell_register("exit", cmd_exit,
                 "exit                        reconnect Telnet to the PDP console",
                 nullptr, "Emulator commands");
}

void telnet_shell_poll() {
  if (!g_initialized) return;
  char command[SHELL_LINE_MAX + 1];
  while (g_active && pop_command(command, sizeof(command)))
    execute_command(command);
}
