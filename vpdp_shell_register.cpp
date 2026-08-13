#include "appconfig.h"
#include "dd11.h"
#include "host_lib/shell/shell_settings.h"
#include "kek_deuna.h"
#include "kl11.h"
#include "kw11.h"
#include "pdp_core.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void kek_tty_set_trace(uint32_t count);
extern "C" uint32_t kek_tty_trace_remaining();

static bool get_i_cfg(int32_t* out, int v) {
  if (!out) return false;
  *out = v;
  return true;
}

static bool get_pcping(int32_t* o) { return get_i_cfg(o, cfg.diag_pcping_sec); }
static bool set_pcping(int32_t v, char*, size_t) {
  cfg.diag_pcping_sec = (int)v;
  return true;
}
static bool get_serialdelay(int32_t* o) {
  return get_i_cfg(o, cfg.diag_serialdelay_ms);
}
static bool set_serialdelay(int32_t v, char*, size_t) {
  cfg.diag_serialdelay_ms = (int)v;
  kl11::serial_in_delay_ms = (uint32_t)v;
  return true;
}
static bool get_io_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)dd11::io_trace_remaining());
}
static bool set_io_trace(int32_t v, char*, size_t) {
  cfg.diag_io_trace = (int)v;
  dd11::set_io_trace((uint32_t)v);
  return true;
}
static bool get_clock_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)kw11::clock_trace_remaining());
}
static bool set_clock_trace(int32_t v, char*, size_t) {
  cfg.diag_clock_trace = (int)v;
  kw11::set_clock_trace((uint32_t)v);
  return true;
}
static bool get_console_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)kl11::console_trace_remaining());
}
static bool set_console_trace(int32_t v, char*, size_t) {
  cfg.diag_console_trace = (int)v;
  kl11::set_console_trace((uint32_t)v);
  kek_tty_set_trace((uint32_t)v);
  return true;
}
static bool get_kek_console_trace(uint32_t* o) {
  if (!o) return false;
  *o = kek_tty_trace_remaining();
  return true;
}
static bool get_dl_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)pdp_core::dl_trace_remaining());
}
static bool set_dl_trace(int32_t v, char*, size_t) {
  cfg.diag_dl_trace = (int)v;
  pdp_core::set_dl_trace((uint32_t)v);
  return true;
}
static bool get_du_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)pdp_core::du_trace_remaining());
}
static bool set_du_trace(int32_t v, char*, size_t) {
  cfg.diag_du_trace = (int)v;
  pdp_core::set_du_trace((uint32_t)v);
  return true;
}
static bool get_rp_trace(int32_t* o) {
  return get_i_cfg(o, (int32_t)pdp_core::rp_trace_remaining());
}
static bool set_rp_trace(int32_t v, char*, size_t) {
  cfg.diag_rp_trace = (int)v;
  pdp_core::set_rp_trace((uint32_t)v);
  return true;
}
static bool get_trace(bool* o) {
  if (!o) return false;
  *o = cfg.diag_trace;
  return true;
}
static bool set_trace(bool v, char*, size_t) {
  cfg.diag_trace = v;
  pdp_core::set_trace(v);
  return true;
}
static bool get_title(char* buf, size_t buflen) {
  if (!buf || !buflen) return false;
  strncpy(buf, cfg.title.c_str(), buflen - 1);
  buf[buflen - 1] = 0;
  return true;
}
static bool set_title(const char* v, char*, size_t) {
  cfg.title = v ? v : "";
  return true;
}
static bool get_eth_en(bool* o) {
  if (!o) return false;
  *o = cfg.eth_enabled;
  return true;
}
static bool set_eth_en(bool v, char*, size_t) {
  cfg.eth_enabled = v;
  kek_deuna::set_enabled(cfg.eth_enabled);
  return true;
}
static bool get_eth_mac(uint8_t mac[6]) {
  if (!mac) return false;
  memcpy(mac, cfg.eth_mac, 6);
  return true;
}
static bool set_eth_mac(const uint8_t mac[6], char*, size_t) {
  memcpy(cfg.eth_mac, mac, 6);
  kek_deuna::set_mac(cfg.eth_mac);
  return true;
}
static bool get_eth_ip(uint32_t* o) {
  if (!o) return false;
  *o = cfg.eth_guest_ip;
  return true;
}
static bool set_eth_ip(uint32_t v, char*, size_t) {
  cfg.eth_guest_ip = v;
  kek_deuna::set_network(cfg.eth_guest_ip, cfg.eth_guest_mask, cfg.eth_gateway_ip);
  return true;
}
static bool get_eth_mask(uint32_t* o) {
  if (!o) return false;
  *o = cfg.eth_guest_mask;
  return true;
}
static bool set_eth_mask(uint32_t v, char*, size_t) {
  cfg.eth_guest_mask = v;
  kek_deuna::set_network(cfg.eth_guest_ip, cfg.eth_guest_mask, cfg.eth_gateway_ip);
  return true;
}
static bool get_eth_gw(uint32_t* o) {
  if (!o) return false;
  *o = cfg.eth_gateway_ip;
  return true;
}
static bool set_eth_gw(uint32_t v, char*, size_t) {
  cfg.eth_gateway_ip = v;
  kek_deuna::set_network(cfg.eth_guest_ip, cfg.eth_guest_mask, cfg.eth_gateway_ip);
  return true;
}

static bool format_break(char* buf, size_t buflen) {
  if (cfg.diag_break_pc == 0) {
    snprintf(buf, buflen, "0");
    return true;
  }
  snprintf(buf, buflen, "%06o", (unsigned)cfg.diag_break_pc);
  return true;
}
static bool parse_break(const char* text, char* err, size_t errlen) {
  String v = text ? text : "";
  v.trim();
  if (!v.length() || v.equalsIgnoreCase("0") || v.equalsIgnoreCase("off") ||
      v.equalsIgnoreCase("clear") || v.equalsIgnoreCase("none") || v == "-") {
    cfg.diag_break_pc = 0;
    pdp_core::monitor_break_clear();
    return true;
  }
  char* end = nullptr;
  unsigned long pc = strtoul(v.c_str(), &end, 8);
  while (end && (*end == ' ' || *end == '\t')) end++;
  if (!end || *end || (pc & 1UL) || pc > 0177777UL) {
    snprintf(err, errlen, "break must be an even octal PC, or 0/clear");
    return false;
  }
  cfg.diag_break_pc = (uint16_t)pc;
  if (!pdp_core::monitor_break_set_pc(cfg.diag_break_pc)) {
    snprintf(err, errlen, "could not arm PC breakpoint");
    return false;
  }
  return true;
}

static bool format_boot_input(char* buf, size_t buflen) {
  snprintf(buf, buflen, "\"%s\"", config_format_boot_input(cfg).c_str());
  return true;
}
static bool parse_boot_input(const char* text, char*, size_t) {
  config_set_boot_input(cfg, String(text ? text : ""));
  return true;
}
static bool format_boot_script(char* buf, size_t buflen) {
  snprintf(buf, buflen, "\"%s\" (%u step%s)",
           config_format_boot_script(cfg).c_str(),
           (unsigned)cfg.boot_script_count,
           cfg.boot_script_count == 1 ? "" : "s");
  return true;
}
static bool parse_boot_script(const char* text, char*, size_t) {
  config_set_boot_script(cfg, String(text ? text : ""));
  return true;
}

static void add_int(const char* name, const char* help, int32_t min_v,
                    int32_t max_v, bool (*get)(int32_t*),
                    bool (*set)(int32_t, char*, size_t),
                    const char* const* aliases = nullptr) {
  ShellSettingDesc d;
  d.name = name;
  d.aliases = aliases;
  d.type = ShellValueType::Int;
  d.help = help;
  d.flags = ShellSetting_RuntimeOnly;
  d.min_i = min_v;
  d.max_i = max_v;
  d.get_i32 = get;
  d.set_i32 = set;
  shell_register_setting(d);
}

void vpdp_register_shell_settings() {
  add_int("pcping", "PC ping interval seconds", 0, 86400, get_pcping, set_pcping);
  add_int("serialdelay", "KL11 serial input delay ms", 0, 10000,
          get_serialdelay, set_serialdelay);
  add_int("io_trace", "Unibus I/O trace remaining", 0, 1000000,
          get_io_trace, set_io_trace);
  add_int("clock_trace", "KW11-L trace remaining", 0, 1000000,
          get_clock_trace, set_clock_trace);
  add_int("console_trace", "console trace remaining", 0, 1000000,
          get_console_trace, set_console_trace);
  {
    ShellSettingDesc d;
    d.name = "kek_console_trace";
    d.type = ShellValueType::UInt;
    d.help = "kek TTY trace remaining (set via console_trace)";
    d.flags = ShellSetting_RuntimeOnly;
    d.get_u32 = get_kek_console_trace;
    shell_register_setting(d);
  }
  add_int("dl_trace", "RL/DL trace remaining", 0, 1000000, get_dl_trace,
          set_dl_trace);
  add_int("du_trace", "MSCP/DU trace remaining", 0, 1000000, get_du_trace,
          set_du_trace);
  static const char* rp_aliases[] = { "dp_trace", nullptr };
  add_int("rp_trace", "RP/RH trace remaining", 0, 1000000, get_rp_trace,
          set_rp_trace, rp_aliases);

  {
    ShellSettingDesc d;
    d.name = "trace";
    d.type = ShellValueType::Bool;
    d.help = "instruction trace";
    d.flags = ShellSetting_RuntimeOnly;
    d.get_bool = get_trace;
    d.set_bool = set_trace;
    shell_register_setting(d);
  }
  {
    ShellSettingDesc d;
    d.name = "break";
    d.type = ShellValueType::Custom;
    d.help = "PC breakpoint (octal, or 0/clear)";
    d.flags = ShellSetting_RuntimeOnly;
    d.format = format_break;
    d.parse = parse_break;
    shell_register_setting(d);
  }
  {
    ShellSettingDesc d;
    d.name = "title";
    d.type = ShellValueType::String;
    d.help = "window/status title";
    d.flags = ShellSetting_RuntimeOnly;
    d.get_string = get_title;
    d.set_string = set_title;
    shell_register_setting(d);
  }
  {
    static const char* aliases[] = { "boot_text", nullptr };
    ShellSettingDesc d;
    d.name = "boot_input";
    d.aliases = aliases;
    d.type = ShellValueType::Custom;
    d.help = "KL11 typeahead after reboot";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.format = format_boot_input;
    d.parse = parse_boot_input;
    shell_register_setting(d);
  }
  {
    ShellSettingDesc d;
    d.name = "boot_script";
    d.type = ShellValueType::Custom;
    d.help = "prompt-driven boot script";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.format = format_boot_script;
    d.parse = parse_boot_script;
    shell_register_setting(d);
  }
  {
    static const char* aliases[] = { "ethernet_enabled", nullptr };
    ShellSettingDesc d;
    d.name = "ethernet";
    d.aliases = aliases;
    d.type = ShellValueType::Bool;
    d.help = "DEUNA attach";
    d.flags = ShellSetting_NextReboot;
    d.get_bool = get_eth_en;
    d.set_bool = set_eth_en;
    shell_register_setting(d);
  }
  {
    ShellSettingDesc d;
    d.name = "ethernet_mac";
    d.type = ShellValueType::MacAddr;
    d.help = "guest MAC";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.get_mac = get_eth_mac;
    d.set_mac = set_eth_mac;
    shell_register_setting(d);
  }
  {
    ShellSettingDesc d;
    d.name = "ethernet_guest_ip";
    d.type = ShellValueType::IPv4;
    d.help = "guest IP";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.get_ipv4 = get_eth_ip;
    d.set_ipv4 = set_eth_ip;
    shell_register_setting(d);
  }
  {
    static const char* aliases[] = { "ethernet_mask", nullptr };
    ShellSettingDesc d;
    d.name = "ethernet_guest_mask";
    d.aliases = aliases;
    d.type = ShellValueType::IPv4;
    d.help = "guest netmask";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.get_ipv4 = get_eth_mask;
    d.set_ipv4 = set_eth_mask;
    shell_register_setting(d);
  }
  {
    static const char* aliases[] = { "ethernet_gateway", nullptr };
    ShellSettingDesc d;
    d.name = "ethernet_gateway_ip";
    d.aliases = aliases;
    d.type = ShellValueType::IPv4;
    d.help = "guest gateway";
    d.flags = (uint16_t)(ShellSetting_RuntimeOnly | ShellSetting_NextReboot);
    d.get_ipv4 = get_eth_gw;
    d.set_ipv4 = set_eth_gw;
    shell_register_setting(d);
  }
}
