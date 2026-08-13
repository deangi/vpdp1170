#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ShellValueType : uint8_t {
  Bool,
  Int,
  UInt,
  Octal,
  String,
  MacAddr,
  IPv4,
  Custom
};

enum ShellSettingFlags : uint16_t {
  ShellSetting_None        = 0,
  ShellSetting_RuntimeOnly = 1 << 0,
  ShellSetting_NextReboot  = 1 << 1,
  ShellSetting_Hidden      = 1 << 2,
};

struct ShellSettingDesc {
  const char* name = nullptr;
  const char* const* aliases = nullptr;
  ShellValueType type = ShellValueType::Int;
  const char* help = nullptr;
  uint16_t flags = ShellSetting_RuntimeOnly;

  int32_t  default_i = 0;
  int32_t  min_i = 0;
  int32_t  max_i = 0;
  uint32_t default_u = 0;
  uint32_t min_u = 0;
  uint32_t max_u = 0;
  bool     default_bool = false;
  size_t   max_len = 0;

  bool (*get_bool)(bool* out) = nullptr;
  bool (*set_bool)(bool v, char* err, size_t errlen) = nullptr;
  bool (*get_i32)(int32_t* out) = nullptr;
  bool (*set_i32)(int32_t v, char* err, size_t errlen) = nullptr;
  bool (*get_u32)(uint32_t* out) = nullptr;
  bool (*set_u32)(uint32_t v, char* err, size_t errlen) = nullptr;
  bool (*get_string)(char* buf, size_t buflen) = nullptr;
  bool (*set_string)(const char* v, char* err, size_t errlen) = nullptr;
  bool (*get_mac)(uint8_t mac[6]) = nullptr;
  bool (*set_mac)(const uint8_t mac[6], char* err, size_t errlen) = nullptr;
  bool (*get_ipv4)(uint32_t* out) = nullptr;
  bool (*set_ipv4)(uint32_t v, char* err, size_t errlen) = nullptr;
  bool (*format)(char* buf, size_t buflen) = nullptr;
  bool (*parse)(const char* text, char* err, size_t errlen) = nullptr;
};

void shell_clear_settings();
void shell_register_setting(const ShellSettingDesc& desc);

// Dump all non-hidden keys (`set` with no args).
void shell_settings_dump();

// assignment is "name=value" (value may contain spaces). Returns false on error
// after printing via shell_out_*.
bool shell_settings_apply(const char* assignment);

void shell_register_set_command();
