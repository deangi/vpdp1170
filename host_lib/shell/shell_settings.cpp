#include "shell_settings.h"
#include "shell_core.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr int kMaxSettings = 40;
static ShellSettingDesc g_set[kMaxSettings];
static int g_nset = 0;

void shell_clear_settings() { g_nset = 0; }

void shell_register_setting(const ShellSettingDesc& desc) {
  if (!desc.name || g_nset >= kMaxSettings) return;
  g_set[g_nset++] = desc;
}

static const ShellSettingDesc* find_setting(const char* name) {
  if (!name) return nullptr;
  for (int i = 0; i < g_nset; i++) {
    if (!strcasecmp(g_set[i].name, name)) return &g_set[i];
    const char* const* a = g_set[i].aliases;
    if (!a) continue;
    for (int j = 0; a[j]; j++) {
      if (!strcasecmp(a[j], name)) return &g_set[i];
    }
  }
  return nullptr;
}

static const char* apply_note(uint16_t flags) {
  if (flags & ShellSetting_NextReboot) {
    if (flags & ShellSetting_RuntimeOnly)
      return " (next guest reboot; runtime only)";
    return " (next guest reboot)";
  }
  if (flags & ShellSetting_RuntimeOnly) return " (runtime only)";
  return "";
}

static bool parse_bool(const char* value, bool* result) {
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

static bool parse_i32(const char* value, int32_t min_v, int32_t max_v,
                      int32_t* result) {
  if (!value || !result) return false;
  char* end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (!end || *end || parsed < min_v || parsed > max_v) return false;
  *result = (int32_t)parsed;
  return true;
}

static bool parse_u32(const char* value, uint32_t min_v, uint32_t max_v,
                      uint32_t* result) {
  if (!value || !result) return false;
  char* end = nullptr;
  unsigned long parsed = strtoul(value, &end, 10);
  if (!end || *end || parsed < min_v || parsed > max_v) return false;
  *result = (uint32_t)parsed;
  return true;
}

static bool parse_mac(const char* s, uint8_t mac[6]) {
  if (!s || !mac) return false;
  unsigned b[6];
  char sep1 = 0, sep2 = 0, sep3 = 0, sep4 = 0, sep5 = 0;
  if (sscanf(s, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x",
             &b[0], &sep1, &b[1], &sep2, &b[2], &sep3,
             &b[3], &sep4, &b[4], &sep5, &b[5]) != 11)
    return false;
  if (!(sep1 == sep2 && sep2 == sep3 && sep3 == sep4 && sep4 == sep5))
    return false;
  if (sep1 != '-' && sep1 != ':' && sep1 != '.') return false;
  for (int i = 0; i < 6; i++) {
    if (b[i] > 0xff) return false;
    mac[i] = (uint8_t)b[i];
  }
  return true;
}

static bool parse_ipv4(const char* s, uint32_t* out) {
  if (!s || !out) return false;
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
         ((uint32_t)c << 8) | (uint32_t)d;
  return true;
}

static void format_mac(const uint8_t mac[6], char* buf, size_t buflen) {
  snprintf(buf, buflen, "%02X-%02X-%02X-%02X-%02X-%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void format_ipv4(uint32_t ip, char* buf, size_t buflen) {
  snprintf(buf, buflen, "%u.%u.%u.%u",
           (unsigned)((ip >> 24) & 0xff), (unsigned)((ip >> 16) & 0xff),
           (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
}

static String unquote(const char* value) {
  String result = value ? value : "";
  if (result.length() >= 2) {
    char quote = result[0];
    if ((quote == '"' || quote == '\'') &&
        result[result.length() - 1] == quote)
      result = result.substring(1, result.length() - 1);
  }
  return result;
}

void shell_settings_dump() {
  char buf[192];
  for (int i = 0; i < g_nset; i++) {
    const ShellSettingDesc& d = g_set[i];
    if (d.flags & ShellSetting_Hidden) continue;
    switch (d.type) {
      case ShellValueType::Bool: {
        bool v = false;
        if (d.get_bool && d.get_bool(&v))
          shell_out_printf("%s=%s\r\n", d.name, v ? "true" : "false");
        break;
      }
      case ShellValueType::Int: {
        int32_t v = 0;
        if (d.get_i32 && d.get_i32(&v))
          shell_out_printf("%s=%ld\r\n", d.name, (long)v);
        break;
      }
      case ShellValueType::UInt: {
        uint32_t v = 0;
        if (d.get_u32 && d.get_u32(&v))
          shell_out_printf("%s=%lu\r\n", d.name, (unsigned long)v);
        break;
      }
      case ShellValueType::Octal: {
        uint32_t v = 0;
        if (d.get_u32 && d.get_u32(&v)) {
          if (v == 0) shell_out_printf("%s=0\r\n", d.name);
          else shell_out_printf("%s=%06lo\r\n", d.name, (unsigned long)v);
        }
        break;
      }
      case ShellValueType::String: {
        if (d.get_string && d.get_string(buf, sizeof(buf)))
          shell_out_printf("%s=\"%s\"\r\n", d.name, buf);
        break;
      }
      case ShellValueType::MacAddr: {
        uint8_t mac[6] = {};
        if (d.get_mac && d.get_mac(mac)) {
          format_mac(mac, buf, sizeof(buf));
          shell_out_printf("%s=%s\r\n", d.name, buf);
        }
        break;
      }
      case ShellValueType::IPv4: {
        uint32_t ip = 0;
        if (d.get_ipv4 && d.get_ipv4(&ip)) {
          format_ipv4(ip, buf, sizeof(buf));
          shell_out_printf("%s=%s\r\n", d.name, buf);
        }
        break;
      }
      case ShellValueType::Custom: {
        if (d.format && d.format(buf, sizeof(buf)))
          shell_out_printf("%s=%s\r\n", d.name, buf);
        break;
      }
    }
  }
}

bool shell_settings_apply(const char* assignment) {
  if (!assignment || !*assignment) {
    shell_settings_dump();
    return true;
  }
  char work[384];
  strncpy(work, assignment, sizeof(work) - 1);
  work[sizeof(work) - 1] = 0;
  char* equals = strchr(work, '=');
  if (!equals) {
    shell_out_text("usage: set name=value\r\n");
    return false;
  }
  *equals = 0;
  char* name = work;
  while (*name == ' ' || *name == '\t') name++;
  char* nend = name + strlen(name);
  while (nend > name && (nend[-1] == ' ' || nend[-1] == '\t')) *--nend = 0;
  char* value = equals + 1;
  while (*value == ' ' || *value == '\t') value++;
  char* vend = value + strlen(value);
  while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = 0;

  const ShellSettingDesc* d = find_setting(name);
  if (!d) {
    shell_out_printf("error: setting is not runtime-changeable: %s\r\n", name);
    return false;
  }

  char err[128] = {};
  const char* note = apply_note(d->flags);

  switch (d->type) {
    case ShellValueType::Bool: {
      bool v = false;
      if (!parse_bool(value, &v)) {
        shell_out_printf("error: %s must be true or false\r\n", d->name);
        return false;
      }
      if (d->set_bool && !d->set_bool(v, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      shell_out_printf("%s=%s%s\r\n", d->name, v ? "true" : "false", note);
      return true;
    }
    case ShellValueType::Int: {
      int32_t v = 0;
      if (!parse_i32(value, d->min_i, d->max_i, &v)) {
        shell_out_printf("error: %s must be %ld..%ld\r\n", d->name,
                         (long)d->min_i, (long)d->max_i);
        return false;
      }
      if (d->set_i32 && !d->set_i32(v, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      shell_out_printf("%s=%ld%s\r\n", d->name, (long)v, note);
      return true;
    }
    case ShellValueType::UInt: {
      if (!d->set_u32) {
        shell_out_printf("error: setting is not runtime-changeable: %s\r\n",
                         d->name);
        return false;
      }
      uint32_t v = 0;
      if (!parse_u32(value, d->min_u, d->max_u, &v)) {
        shell_out_printf("error: %s must be %lu..%lu\r\n", d->name,
                         (unsigned long)d->min_u, (unsigned long)d->max_u);
        return false;
      }
      if (!d->set_u32(v, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      shell_out_printf("%s=%lu%s\r\n", d->name, (unsigned long)v, note);
      return true;
    }
    case ShellValueType::Octal: {
      char* end = nullptr;
      unsigned long v = strtoul(value, &end, 8);
      while (end && (*end == ' ' || *end == '\t')) end++;
      if (!end || *end || v < d->min_u || v > d->max_u) {
        shell_out_printf("error: %s must be octal %lo..%lo\r\n", d->name,
                         (unsigned long)d->min_u, (unsigned long)d->max_u);
        return false;
      }
      if (d->set_u32 && !d->set_u32((uint32_t)v, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      if (v == 0) shell_out_printf("%s=0%s\r\n", d->name, note);
      else shell_out_printf("%s=%06lo%s\r\n", d->name, v, note);
      return true;
    }
    case ShellValueType::String: {
      String u = unquote(value);
      if (d->max_len && u.length() > d->max_len) {
        shell_out_printf("error: %s is too long (max %u)\r\n", d->name,
                         (unsigned)d->max_len);
        return false;
      }
      if (d->set_string && !d->set_string(u.c_str(), err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      shell_out_printf("%s=\"%s\"%s\r\n", d->name, u.c_str(), note);
      return true;
    }
    case ShellValueType::MacAddr: {
      uint8_t mac[6];
      String u = unquote(value);
      if (!parse_mac(u.c_str(), mac)) {
        shell_out_printf("error: bad %s \"%s\" (want aa-bb-cc-dd-ee-ff)\r\n",
                         d->name, u.c_str());
        return false;
      }
      if (d->set_mac && !d->set_mac(mac, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      char macbuf[24];
      format_mac(mac, macbuf, sizeof(macbuf));
      shell_out_printf("%s=%s%s\r\n", d->name, macbuf, note);
      return true;
    }
    case ShellValueType::IPv4: {
      uint32_t ip = 0;
      String u = unquote(value);
      if (!parse_ipv4(u.c_str(), &ip)) {
        shell_out_printf("error: bad %s \"%s\"\r\n", d->name, u.c_str());
        return false;
      }
      if (d->set_ipv4 && !d->set_ipv4(ip, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      char ipbuf[16];
      format_ipv4(ip, ipbuf, sizeof(ipbuf));
      shell_out_printf("%s=%s%s\r\n", d->name, ipbuf, note);
      return true;
    }
    case ShellValueType::Custom: {
      if (!d->parse) {
        shell_out_printf("error: %s has no parser\r\n", d->name);
        return false;
      }
      if (!d->parse(value, err, sizeof(err))) {
        shell_out_printf("error: %s\r\n", err[0] ? err : "set failed");
        return false;
      }
      char buf[192];
      if (d->format && d->format(buf, sizeof(buf)))
        shell_out_printf("%s=%s%s\r\n", d->name, buf, note);
      else
        shell_out_printf("%s updated%s\r\n", d->name, note);
      return true;
    }
  }
  return false;
}

static void cmd_set(int argc, char** argv) {
  if (argc <= 1) {
    shell_settings_dump();
    return;
  }
  char buf[384];
  buf[0] = 0;
  for (int i = 1; i < argc; i++) {
    if (i > 1) strlcat(buf, " ", sizeof(buf));
    strlcat(buf, argv[i], sizeof(buf));
  }
  shell_settings_apply(buf);
}

void shell_register_set_command() {
  static const char* aliases[] = { nullptr };
  shell_register("set", cmd_set,
                 "set [name=value]            show/change runtime settings",
                 nullptr, "Emulator commands");
  (void)aliases;
}
