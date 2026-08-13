#include "shell_media.h"
#include "shell_core.h"

#include <string.h>

static const MediaOps* g_media = nullptr;

void shell_set_media_ops(const MediaOps* ops) { g_media = ops; }

static void cmd_drives(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!g_media || !g_media->list) {
    shell_out_text("error: media listing is not supported\r\n");
    return;
  }
  MediaUnitInfo info;
  for (int i = 0;; i++) {
    memset(&info, 0, sizeof(info));
    if (!g_media->list(i, &info)) break;
    if (!info.mounted) {
      shell_out_printf("%-3s  empty\r\n", info.name);
      continue;
    }
    if (info.extra[0]) {
      shell_out_printf("%-3s  %s  %lu bytes  %s  %s\r\n",
                       info.name, info.path,
                       (unsigned long)info.size_bytes, info.extra,
                       info.readonly ? "read-only" : "read-write");
    } else {
      shell_out_printf("%-3s  %s  %lu bytes  %s\r\n",
                       info.name, info.path,
                       (unsigned long)info.size_bytes,
                       info.readonly ? "read-only" : "read-write");
    }
  }
}

static void cmd_mount(int argc, char** argv) {
  if (!g_media || !g_media->mount) {
    shell_out_text("error: mount is not supported\r\n");
    return;
  }
  if (argc < 3) {
    const char* u = g_media->mount_usage ? g_media->mount_usage() : nullptr;
    shell_out_text(u && *u ? u : "usage: mount <unit> <path> [ro]\r\n");
    return;
  }
  bool ro = argc > 3 && (!strcasecmp(argv[3], "ro") ||
                         !strcasecmp(argv[3], "readonly"));
  char err[192] = {};
  if (!g_media->mount(argv[1], argv[2], ro, err, sizeof(err))) {
    if (!strncmp(err, "usage:", 6))
      shell_out_text(err);
    else
      shell_out_printf("error: %s\r\n", err[0] ? err : "mount failed");
    return;
  }
  if (err[0])
    shell_out_text(err);
  else
    shell_out_printf("mounted %s -> %s%s\r\n", argv[1], argv[2],
                     ro ? " (ro)" : "");
}

static void cmd_dismount(int argc, char** argv) {
  if (!g_media || !g_media->dismount) {
    shell_out_text("error: dismount is not supported\r\n");
    return;
  }
  if (argc < 2) {
    const char* u = g_media->mount_usage ? g_media->mount_usage() : nullptr;
    shell_out_text(u && *u ? u : "usage: dismount <unit>\r\n");
    return;
  }
  char err[192] = {};
  if (!g_media->dismount(argv[1], err, sizeof(err))) {
    shell_out_printf("error: %s\r\n", err[0] ? err : "dismount failed");
    return;
  }
  if (err[0])
    shell_out_text(err);
  else
    shell_out_printf("dismounted %s\r\n", argv[1]);
}

static void cmd_create(int argc, char** argv) {
  if (!g_media || !g_media->create_image) {
    shell_out_text("error: create is not supported\r\n");
    return;
  }
  if (argc < 3) {
    const char* u = g_media->create_usage ? g_media->create_usage() : nullptr;
    shell_out_text(u && *u ? u : "usage: create <type> <path>\r\n");
    return;
  }
  char err[192] = {};
  if (!g_media->create_image(argv[1], argv[2], err, sizeof(err))) {
    shell_out_printf("error: %s\r\n", err[0] ? err : "create failed");
    return;
  }
  if (err[0])
    shell_out_text(err);
  else
    shell_out_printf("created %s\r\n", argv[2]);
}

void shell_register_media_commands() {
  static const char* dismount_aliases[] = { "unmount", nullptr };
  shell_register("drives", cmd_drives,
                 "drives                      show mounted disk images",
                 nullptr, "Emulator commands");
  shell_register("mount", cmd_mount,
                 "mount <unit> <path> [ro]    mount an image on a unit",
                 nullptr, "Emulator commands");
  shell_register("dismount", cmd_dismount,
                 "dismount <unit>             dismount a drive",
                 dismount_aliases, "Emulator commands");
  shell_register("create", cmd_create,
                 "create <type> <path>        create an empty disk image",
                 nullptr, "Emulator commands");
}
