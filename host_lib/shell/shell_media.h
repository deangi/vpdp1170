#pragma once

#include <stddef.h>
#include <stdint.h>

struct MediaUnitInfo {
  char name[16];
  char path[128];
  uint32_t size_bytes;
  bool mounted;
  bool readonly;
  char extra[24];  // e.g. "RL02", may be empty
};

struct MediaOps {
  bool (*list)(int index, MediaUnitInfo* out);
  bool (*mount)(const char* unit, const char* path, bool readonly,
                char* err, size_t errlen);
  bool (*dismount)(const char* unit, char* err, size_t errlen);
  bool (*create_image)(const char* type, const char* path,
                       char* err, size_t errlen);
  bool (*path_protected)(const char* path);
  const char* (*mount_usage)();
  const char* (*create_usage)();
};

void shell_set_media_ops(const MediaOps* ops);
void shell_register_media_commands();

struct GuestControlOps {
  bool (*restart)(char* err, size_t errlen);
  const char* (*restart_help)();
};

void shell_set_guest_control_ops(const GuestControlOps* ops);
void shell_register_guest_control_commands();
