#include "shell_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static constexpr int kMaxCommands = 48;
static constexpr int kMaxAliases = 4;

struct ShellCommand {
  const char* name = nullptr;
  const char* aliases[kMaxAliases] = {};
  ShellHandler fn = nullptr;
  const char* help_line = nullptr;
  const char* pack = nullptr;
};

static ShellCommand g_cmds[kMaxCommands];
static int g_ncmds = 0;
static void (*g_out_text)(const char* s) = nullptr;

void shell_set_out(void (*text)(const char* s)) {
  g_out_text = text;
}

void shell_out_text(const char* s) {
  if (g_out_text && s) g_out_text(s);
}

void shell_out_printf(const char* fmt, ...) {
  char buf[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  shell_out_text(buf);
}

void shell_clear_commands() {
  g_ncmds = 0;
}

void shell_register(const char* name, ShellHandler fn,
                    const char* help_line,
                    const char* const* aliases,
                    const char* pack) {
  if (!name || !fn || g_ncmds >= kMaxCommands) return;
  ShellCommand& c = g_cmds[g_ncmds++];
  c.name = name;
  c.fn = fn;
  c.help_line = help_line;
  c.pack = pack;
  for (int i = 0; i < kMaxAliases; i++) c.aliases[i] = nullptr;
  if (!aliases) return;
  for (int i = 0; i < kMaxAliases && aliases[i]; i++)
    c.aliases[i] = aliases[i];
}

static bool name_matches(const ShellCommand& c, const char* word) {
  if (!word) return false;
  if (!strcasecmp(c.name, word)) return true;
  for (int i = 0; i < kMaxAliases && c.aliases[i]; i++) {
    if (!strcasecmp(c.aliases[i], word)) return true;
  }
  return false;
}

bool shell_dispatch(int argc, char** argv) {
  if (argc <= 0 || !argv || !argv[0]) return false;
  for (int i = 0; i < g_ncmds; i++) {
    if (!name_matches(g_cmds[i], argv[0])) continue;
    g_cmds[i].fn(argc, argv);
    return true;
  }
  return false;
}

void shell_print_help() {
  const char* last_pack = nullptr;
  for (int i = 0; i < g_ncmds; i++) {
    const ShellCommand& c = g_cmds[i];
    if (!c.help_line) continue;
    if (c.pack && (!last_pack || strcasecmp(last_pack, c.pack) != 0)) {
      shell_out_printf("%s:\r\n", c.pack);
      last_pack = c.pack;
    }
    shell_out_printf("  %s\r\n", c.help_line);
  }
}
