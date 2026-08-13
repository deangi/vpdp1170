#include "shell_media.h"
#include "shell_core.h"

#include <string.h>

static const GuestControlOps* g_guest = nullptr;

void shell_set_guest_control_ops(const GuestControlOps* ops) {
  g_guest = ops;
}

static void cmd_restart(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!g_guest || !g_guest->restart) {
    shell_out_text("error: restart is not supported\r\n");
    return;
  }
  char err[128] = {};
  if (!g_guest->restart(err, sizeof(err))) {
    shell_out_printf("error: %s\r\n", err[0] ? err : "restart failed");
    return;
  }
  const char* help = g_guest->restart_help ? g_guest->restart_help() : nullptr;
  if (help && *help)
    shell_out_printf("emulator reset scheduled (%s)\r\n", help);
  else
    shell_out_text("emulator reset scheduled\r\n");
}

void shell_register_guest_control_commands() {
  static const char* aliases[] = { "restart", nullptr };
  shell_register(
      "reset", cmd_restart,
      "reset                       restart emulator (reload/remount/cold boot)",
      aliases, "Emulator commands");
  shell_register(
      "reboot", cmd_restart,
      "reboot                      alias for reset",
      nullptr, "Emulator commands");
}
