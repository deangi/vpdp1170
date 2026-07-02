#include "appconfig.h"
#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

#include <SD_MMC.h>
#include <string.h>    // strrchr, strncmp, strcasecmp for variant discovery

// -------- helpers --------

static String trim(const String& s) {
  int a = 0, b = (int)s.length();
  while (a < b && isspace((uint8_t)s[a])) a++;
  while (b > a && isspace((uint8_t)s[b - 1])) b--;
  return s.substring(a, b);
}

static String to_lower(const String& s) {
  String t = s;
  t.toLowerCase();
  return t;
}

static bool truthy(const String& s) {
  return s.equalsIgnoreCase("true") ||
         s == "1" ||
         s.equalsIgnoreCase("yes") ||
         s.equalsIgnoreCase("on");
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String strip_inline_comment(const String& val) {
  bool in_quote = false;
  char quote = 0;
  bool escaped = false;
  for (int i = 0; i < (int)val.length(); i++) {
    char c = val[i];
    if (escaped) { escaped = false; continue; }
    if (c == '\\') { escaped = true; continue; }
    if (in_quote) {
      if (c == quote) in_quote = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_quote = true;
      quote = c;
      continue;
    }
    if (c == ';' || c == '#') return trim(val.substring(0, i));
  }
  return trim(val);
}

static String unquote_config_value(const String& val) {
  if (val.length() >= 2) {
    char q = val[0];
    if ((q == '"' || q == '\'') && val[val.length() - 1] == q)
      return val.substring(1, val.length() - 1);
  }
  return val;
}

void config_set_boot_input(AppConfig& cfg, const String& encoded) {
  cfg.boot_input_len = 0;
  String s = unquote_config_value(encoded);

  for (int i = 0; i < (int)s.length() &&
                  cfg.boot_input_len < AppConfig::BOOT_INPUT_MAX; i++) {
    uint8_t out = (uint8_t)s[i];

    if (s[i] == '^' && i + 1 < (int)s.length()) {
      char c = s[++i];
      if (c == '?') out = 0x7f;
      else          out = ((uint8_t)c) & 0x1f;
    } else if (s[i] == '\\' && i + 1 < (int)s.length()) {
      char c = s[++i];
      switch (c) {
        case 'r': out = '\r'; break;
        case 'n': out = '\n'; break;
        case 't': out = '\t'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'e': out = 0x1b; break;
        case 's': out = ' ';  break;
        case '\\': out = '\\'; break;
        case '"': out = '"'; break;
        case '\'': out = '\''; break;
        case 'x': {
          int v = 0;
          int digits = 0;
          while (i + 1 < (int)s.length() && digits < 2) {
            int h = hex_value(s[i + 1]);
            if (h < 0) break;
            v = (v << 4) | h;
            i++;
            digits++;
          }
          out = (uint8_t)v;
          break;
        }
        default:
          if (c >= '0' && c <= '7') {
            int v = c - '0';
            int digits = 1;
            while (i + 1 < (int)s.length() && digits < 3 &&
                   s[i + 1] >= '0' && s[i + 1] <= '7') {
              v = (v << 3) | (s[i + 1] - '0');
              i++;
              digits++;
            }
            out = (uint8_t)v;
          } else {
            out = (uint8_t)c;
          }
          break;
      }
    }

    cfg.boot_input[cfg.boot_input_len++] = out;
  }
}

String config_escape_bytes(const uint8_t* bytes, size_t len) {
  String out;
  char tmp[6];
  for (size_t i = 0; i < len; i++) {
    uint8_t c = bytes[i];
    switch (c) {
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case 0x1b: out += "\\e"; break;
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      default:
        if (c >= 32 && c < 127) out += (char)c;
        else {
          snprintf(tmp, sizeof(tmp), "\\x%02X", c);
          out += tmp;
        }
        break;
    }
  }
  return out;
}

// -------- SD --------

bool sd_mount() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  if (!SD_MMC.begin("/sdcard", false /*1bit*/, false /*format*/,
                    20000 /*freq*/, SD_MAX_OPEN_FILES)) {
    LOGE("SD_MMC.begin() failed");
    return false;
  }
  uint8_t type = SD_MMC.cardType();
  if (type == CARD_NONE) {
    LOGE("No SD card detected");
    return false;
  }
  const char* tname = (type == CARD_MMC)  ? "MMC"
                    : (type == CARD_SD)   ? "SDSC"
                    : (type == CARD_SDHC) ? "SDHC"
                                          : "?";
  uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  LOG("SD mounted: type=%s size=%llu MB", tname, (unsigned long long)mb);
  return true;
}

// -------- defaults --------

void config_apply_compiled_defaults(AppConfig& cfg) {
  cfg.title         = APP_TITLE;
  cfg.version       = APP_VERSION;
  cfg.build         = APP_BUILD_DATE;

  cfg.wifi_ssid     = WIFI_SSID;
  cfg.wifi_password = WIFI_PASS;
  cfg.wifi_hostname = WIFI_HOSTNAME;

  cfg.telnet_enabled = true;
  cfg.telnet_port    = TELNET_PORT;

  cfg.boot_input_len = 0;
  cfg.serial1_enabled = false;

  cfg.ftp_enabled    = true;
  cfg.ftp_port       = FTP_PORT;
  cfg.ftp_user       = FTP_DEFAULT_USER;
  cfg.ftp_password   = FTP_DEFAULT_PASS;

  cfg.diag_pcping_sec = 5;
  cfg.diag_serialdelay_ms = 20;
  cfg.diag_io_trace   = 0;
  cfg.diag_clock_trace = 0;
  cfg.diag_console_trace = 0;
  cfg.diag_trace      = false;
  cfg.v4b_quirks      = true;
  cfg.kwp_enabled     = false;

  cfg.disk_a        = DEFAULT_DL0_IMG;
  cfg.disk_b        = DEFAULT_DL1_IMG;
  cfg.disk_c        = DEFAULT_DL2_IMG;
  cfg.disk_d        = DEFAULT_DL3_IMG;
  cfg.disk_rk0      = "";
  cfg.disk_rp0      = "";
  cfg.disk_rp0_type = "rp06";
  cfg.boot_drive    = 'a';
  cfg.boot_kind     = AppConfig::BK_RL;
}

// -------- parser --------

enum ConfigDomain : uint8_t { CONFIG_NETWORK, CONFIG_EMULATOR };

static void parse_line(AppConfig& cfg, String& section, const String& raw,
                       ConfigDomain domain) {
  String t = trim(raw);
  if (t.length() == 0) return;
  if (t.startsWith(";") || t.startsWith("#")) return;
  if (t.startsWith("[") && t.endsWith("]")) {
    section = to_lower(t.substring(1, t.length() - 1));
    return;
  }
  int eq = t.indexOf('=');
  if (eq < 0) return;

  String key = to_lower(trim(t.substring(0, eq)));
  String val = strip_inline_comment(t.substring(eq + 1));

  bool network_section = section == "wifi" || section == "telnet" || section == "ftp";
  if ((domain == CONFIG_NETWORK) != network_section) return;

  if (section == "system") {
    if (key == "title") cfg.title = val;
  } else if (section == "wifi") {
    if      (key == "ssid")     cfg.wifi_ssid     = val;
    else if (key == "password") cfg.wifi_password = val;
    else if (key == "hostname") cfg.wifi_hostname = val;
  } else if (section == "telnet") {
    if      (key == "enabled")  cfg.telnet_enabled = truthy(val);
    else if (key == "port")     cfg.telnet_port = val.toInt();
  } else if (section == "console") {
    if      (key == "boot_input" || key == "typeahead" || key == "boot_keys")
      config_set_boot_input(cfg, val);
  } else if (section == "serial1") {
    if      (key == "enabled") cfg.serial1_enabled = truthy(val);
  } else if (section == "ftp") {
    if      (key == "enabled")  cfg.ftp_enabled  = truthy(val);
    else if (key == "port")     cfg.ftp_port     = val.toInt();
    else if (key == "user")     cfg.ftp_user     = val;
    else if (key == "password") cfg.ftp_password = val;
  } else if (section == "diag" || section == "emu") {
    // "emu" kept as an alias for back-compat with the first revision of
    // the parser; "diag" is the canonical section going forward.
    if      (key == "pcping")     cfg.diag_pcping_sec = val.toInt();
    else if (key == "serialdelay") cfg.diag_serialdelay_ms = val.toInt();
    else if (key == "io_trace") {
      long count = val.toInt();
      cfg.diag_io_trace = count < 0 ? 0
                        : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "clock_trace") {
      long count = val.toInt();
      cfg.diag_clock_trace = count < 0 ? 0
                           : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "console_trace") {
      long count = val.toInt();
      cfg.diag_console_trace = count < 0 ? 0
                             : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "trace")      cfg.diag_trace = truthy(val);
    else if (key == "v4b_quirks") cfg.v4b_quirks = (val.equalsIgnoreCase("true") ||
                                                   val == "1" ||
                                                   val.equalsIgnoreCase("yes") ||
                                                   val.equalsIgnoreCase("on"));
    else if (key == "kwp_enabled") cfg.kwp_enabled = (val.equalsIgnoreCase("true") ||
                                                     val == "1" ||
                                                     val.equalsIgnoreCase("yes") ||
                                                     val.equalsIgnoreCase("on"));
  } else if (section == "disks") {
    // DL0..DL3, RK0, and RP0 have separate host media slots. boot= chooses
    // which controller bootstrap is installed, but mounting one media type
    // no longer hides another.
    if      (key == "dl0")      cfg.disk_a = val;
    else if (key == "dl1")      cfg.disk_b = val;
    else if (key == "dl2")      cfg.disk_c = val;
    else if (key == "dl3")      cfg.disk_d = val;
    else if (key == "rk0")      cfg.disk_rk0 = val;
    else if (key == "rp0")      cfg.disk_rp0 = val;
    else if (key == "rp0_type") cfg.disk_rp0_type = to_lower(val);
    else if (key == "boot") {
      String v = to_lower(val);
      cfg.boot_kind = AppConfig::BK_RL;
      if      (v == "dl0" || v == "0")  cfg.boot_drive = 'a';
      else if (v == "dl1" || v == "1")  cfg.boot_drive = 'b';
      else if (v == "dl2" || v == "2") cfg.boot_drive = 'c';
      else if (v == "dl3" || v == "3") cfg.boot_drive = 'd';
      // rk0 (DEC) / dk0 (Bell Labs Unix V6 device name) both mean the RK05.
      else if (v == "rk0" || v == "dk0") {
        cfg.boot_drive = 'a';
        cfg.boot_kind  = AppConfig::BK_RK;
      }
      else if (v.length() == 1 && v[0] >= 'a' && v[0] <= 'd')
        cfg.boot_drive = v[0];           // legacy single-char form
      else {
        LOGE("pdpconfig.ini: unknown boot value \"%s\" - using dl0", val.c_str());
        cfg.boot_drive = 'a';
      }
    }
  }
}

// Internal: parse one config file at `path` into cfg through `parse_line`.
// Returns true if the file was opened and parsed; false if it didn't exist.
static void recover_config_backup(const char* path) {
  if (SD_MMC.exists(path)) return;
  char backup[192];
  if (snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) return;
  if (SD_MMC.exists(backup)) {
    if (SD_MMC.rename(backup, path))
      LOG("Restored interrupted config update: %s", path);
    else
      LOGE("Could not restore config backup %s", backup);
  }
}

static bool parse_config_file(AppConfig& cfg, const char* path, ConfigDomain domain) {
  SD_FTP_StorageGuard guard;
  recover_config_backup(path);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  String section;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    parse_line(cfg, section, line, domain);
  }
  f.close();
  return true;
}

bool config_load_wifi(AppConfig& cfg) {
  // Compiled defaults are already in cfg (caller did
  // config_apply_compiled_defaults). Clear wifi-only fields so a present
  // file overrides them, then fall back to secrets.h for any field left
  // blank.
  cfg.wifi_ssid     = "";
  cfg.wifi_password = "";
  cfg.wifi_hostname = "";

  bool existed = parse_config_file(cfg, WIFI_CFG_PATH, CONFIG_NETWORK);
  if (!existed) {
    LOG("%s not found, writing defaults", WIFI_CFG_PATH);
    // Restore compiled defaults so the writer emits a useful template.
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASS;
    cfg.wifi_hostname = WIFI_HOSTNAME;
    config_write_default_wifi(cfg);
    return false;
  }

  if (cfg.wifi_ssid.length() == 0)     cfg.wifi_ssid     = WIFI_SSID;
  if (cfg.wifi_password.length() == 0) cfg.wifi_password = WIFI_PASS;
  if (cfg.wifi_hostname.length() == 0) cfg.wifi_hostname = WIFI_HOSTNAME;
  if (cfg.ftp_user.length() == 0)      cfg.ftp_user      = FTP_DEFAULT_USER;
  if (cfg.ftp_password.length() == 0)  cfg.ftp_password  = FTP_DEFAULT_PASS;
  return true;
}

bool config_load_pdp(AppConfig& cfg) {
  // Disk paths: clear so a present file overrides; blank lines in the
  // file leave the field empty (= dismounted), which is intended.
  cfg.disk_a = "";
  cfg.disk_b = "";
  cfg.disk_c = "";
  cfg.disk_d = "";
  cfg.disk_rk0 = "";
  cfg.disk_rp0 = "";
  cfg.disk_rp0_type = "rp06";
  cfg.boot_input_len = 0;
  cfg.serial1_enabled = false;

  bool existed = parse_config_file(cfg, PDP_CFG_PATH, CONFIG_EMULATOR);
  if (!existed) {
    LOG("%s not found, writing defaults", PDP_CFG_PATH);
    cfg.disk_a = DEFAULT_DL0_IMG;
    cfg.disk_b = DEFAULT_DL1_IMG;
    cfg.disk_c = DEFAULT_DL2_IMG;
    cfg.disk_d = DEFAULT_DL3_IMG;
    config_write_default_pdp(cfg);
    return false;
  }
  return true;
}

bool config_write_default_wifi(const AppConfig& cfg) {
  SD_FTP_StorageGuard guard;
  // SD_MMC's FILE_WRITE truncates, which is what we want for a clean rewrite.
  File f = SD_MMC.open(WIFI_CFG_PATH, FILE_WRITE);
  if (!f) {
    LOGE("Could not open %s for write", WIFI_CFG_PATH);
    return false;
  }
  f.println("; vpdp1170 WiFi configuration");
  f.println("; Copy this to wificonfig-NAME.ini to create a named variant");
  f.println("; (then pick it from the Settings -> WiFi Config menu).");
  f.println();
  f.println("[wifi]");
  f.println("; Leave ssid/password blank to use the values compiled into secrets.h.");
  f.println("ssid     = ");
  f.println("password = ");
  f.printf("hostname = %s\r\n", cfg.wifi_hostname.c_str());
  f.println();
  f.println("[telnet]");
  f.printf("enabled = %s\r\n", cfg.telnet_enabled ? "true" : "false");
  f.printf("port    = %d\r\n", cfg.telnet_port);
  f.println();
  f.println("[ftp]");
  f.println("; FTP exposes the SD card root. Passive data uses port+1.");
  f.printf("enabled  = %s\r\n", cfg.ftp_enabled ? "true" : "false");
  f.printf("port     = %d\r\n", cfg.ftp_port);
  f.printf("user     = %s\r\n", cfg.ftp_user.c_str());
  f.printf("password = %s\r\n", cfg.ftp_password.c_str());
  f.close();
  LOG("Wrote default %s", WIFI_CFG_PATH);
  return true;
}

bool config_write_default_pdp(const AppConfig& cfg) {
  SD_FTP_StorageGuard guard;
  File f = SD_MMC.open(PDP_CFG_PATH, FILE_WRITE);
  if (!f) {
    LOGE("Could not open %s for write", PDP_CFG_PATH);
    return false;
  }
  f.println("; vpdp1170 PDP-11 configuration");
  f.println("; Copy this to pdpconfig-NAME.ini to create a named variant");
  f.println("; (then pick it from the Settings -> PDP Config menu).");
  f.println();
  f.println("[system]");
  f.printf("title   = %s\r\n", cfg.title.c_str());
  f.println();
  f.println("[console]");
  f.println("; boot_input is injected into the KL11 input queue after each");
  f.println("; PDP-11 boot/reset. Escapes: \\r \\n \\t \\e \\xHH \\ooo ^C ^[ ^?.");
  f.printf("boot_input = \"%s\"\r\n",
           config_escape_bytes(cfg.boot_input, cfg.boot_input_len).c_str());
  f.println();
  f.println("[serial1]");
  f.println("; Optional second DL11-compatible TTY at 0176500. The TTY0 VPDP");
  f.println("; command channel and direct SD file commands are always available;");
  f.println("; this setting enables only TT1 background file streaming.");
  f.printf("enabled = %s\r\n", cfg.serial1_enabled ? "true" : "false");
  f.println();
  f.println("[diag]");
  f.println("; pcping      = seconds between host's periodic PC/register dump");
  f.println(";               to USB-Serial. 0 disables it (so do large values).");
  f.println("; v4b_quirks  = absorb KE11-A (0o772100..0o772176) and TT1");
  f.println(";               (0o776500..0o776516) probes silently. Default");
  f.println(";               true makes RSTS/E V4B + RT-11 + V6 + XXDP boot;");
  f.println(";               set false to attempt RSTS/E V7 (V4B will not");
  f.println(";               boot in that mode).");
  f.println("; kwp_enabled = activate the KW11-P programmable real-time clock");
  f.println(";               at 0o772540 (vector 0104, BR6). Implements the");
  f.println(";               100 kHz, 10 kHz, line and external rates plus");
  f.println(";               up/down, one-shot/repeat, DONE and overrun.");
  f.println(";               Default false (stub mode) because");
  f.println(";               RSTS V4B sees a working KW11-P and programs it");
  f.println(";               for interrupts that break its terminal echo");
  f.println(";               (upper case shows as lower case). Set true only");
  f.println(";               for RSTS V7 hardware-test bring-up.");
  f.println("; serialdelay = minimum ms between successive characters loaded");
  f.println(";               into the KL11 TKB. Prevents back-to-back addchars");
  f.println(";               while the guest is still inside klrint on the");
  f.println(";               prior byte (which would re-enter klrint on sam11");
  f.println(";               and reverse the order). 0 disables; 10-50 ms");
  f.println(";               typical for V6 / RT-11 / RSTS under a line-");
  f.println(";               buffered host (Arduino IDE Serial Monitor).");
  f.println("; trace       = per-instruction panic trace ring. Expensive:");
  f.println(";               set true only when chasing a HALT/panic.");
  f.println("; io_trace    = log the next N I/O-page reads/writes, then stop.");
  f.println(";               0 disables I/O tracing.");
  f.println("; clock_trace = log the next N KW11-L/KW11-P register and");
  f.println(";               interrupt events, then stop. 0 disables.");
  f.println("; console_trace = log the next N characters read from or written");
  f.println(";                 to the KL11 console, then stop. 0 disables.");
  f.printf("pcping      = %d\r\n", cfg.diag_pcping_sec);
  f.printf("serialdelay = %d\r\n", cfg.diag_serialdelay_ms);
  f.printf("io_trace    = %d\r\n", cfg.diag_io_trace);
  f.printf("clock_trace = %d\r\n", cfg.diag_clock_trace);
  f.printf("console_trace = %d\r\n", cfg.diag_console_trace);
  f.printf("trace       = %s\r\n", cfg.diag_trace ? "true" : "false");
  f.printf("v4b_quirks  = %s\r\n", cfg.v4b_quirks ? "true" : "false");
  f.printf("kwp_enabled = %s\r\n", cfg.kwp_enabled ? "true" : "false");
  f.println();
  f.println("[disks]");
  f.println("; dl0..dl3 = RL11 units (RL01/RL02 disk packs).");
  f.println("; rk0      = RK05 2.5 MB disk pack (e.g. RT-11).");
  f.println("; rp0      = optional secondary RH11/RP disk pack.");
  f.println("; rp0_type = rp04, rp05, or rp06. Default rp06.");
  f.println("; Leave a slot blank to dismount it at boot.");
  f.printf("dl0  = %s\r\n", cfg.disk_a.c_str());
  f.printf("dl1  = %s\r\n", cfg.disk_b.c_str());
  f.printf("dl2  = %s\r\n", cfg.disk_c.c_str());
  f.printf("dl3  = %s\r\n", cfg.disk_d.c_str());
  f.printf("rk0  = %s\r\n", cfg.disk_rk0.c_str());
  f.printf("rp0  = %s\r\n", cfg.disk_rp0.c_str());
  f.printf("rp0_type = %s\r\n", cfg.disk_rp0_type.c_str());
  // Friendly boot value: dl0/dl1/dl2/dl3/rk0
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else boot_name = (cfg.boot_drive == 'a') ? "dl0"
                 : (cfg.boot_drive == 'b') ? "dl1"
                 : (cfg.boot_drive == 'c') ? "dl2"
                 : (cfg.boot_drive == 'd') ? "dl3" : "dl0";
  f.printf("boot = %s\r\n", boot_name);
  f.close();
  LOG("Wrote default %s", PDP_CFG_PATH);
  return true;
}

bool config_copy_file(const char* src, const char* dst) {
  SD_FTP_StorageGuard guard;
  char temp[192];
  char backup[192];
  if (snprintf(temp, sizeof(temp), "%s.tmp", dst) >= (int)sizeof(temp) ||
      snprintf(backup, sizeof(backup), "%s.bak", dst) >= (int)sizeof(backup)) {
    LOGE("config_copy_file: destination path too long: %s", dst);
    return false;
  }

  File s = SD_MMC.open(src, FILE_READ);
  if (!s) { LOGE("config_copy_file: can't open %s for read", src); return false; }
  uint32_t srcSize = (uint32_t)s.size();

  if (SD_MMC.exists(temp)) SD_MMC.remove(temp);
  File d = SD_MMC.open(temp, FILE_WRITE);
  if (!d) {
    LOGE("config_copy_file: can't open %s for write", temp);
    s.close();
    return false;
  }

  uint8_t buf[512];
  size_t total = 0;
  bool copy_ok = true;
  while (s.available()) {
    int n = s.read(buf, sizeof(buf));
    if (n <= 0) { copy_ok = false; break; }
    int w = d.write(buf, n);
    if (w != n) {
      LOGE("config_copy_file: short write (%d/%d) at %u into %s",
           w, n, (unsigned)total, temp);
      copy_ok = false;
      break;
    }
    total += n;
  }
  s.close();
  d.flush();
  d.close();

  File v = SD_MMC.open(temp, FILE_READ);
  uint32_t verifySize = v ? (uint32_t)v.size() : 0;
  if (v) v.close();
  if (!copy_ok || total != srcSize || verifySize != srcSize) {
    LOGE("config_copy_file: temporary copy failed src=%u written=%u on-disk=%u",
         (unsigned)srcSize, (unsigned)total, (unsigned)verifySize);
    SD_MMC.remove(temp);
    return false;
  }

  if (SD_MMC.exists(backup)) SD_MMC.remove(backup);
  bool had_dst = SD_MMC.exists(dst);
  if (had_dst && !SD_MMC.rename(dst, backup)) {
    LOGE("config_copy_file: can't preserve %s as %s", dst, backup);
    SD_MMC.remove(temp);
    return false;
  }
  if (!SD_MMC.rename(temp, dst)) {
    LOGE("config_copy_file: can't activate %s", dst);
    if (had_dst && !SD_MMC.rename(backup, dst))
      LOGE("config_copy_file: FAILED to restore %s from %s", dst, backup);
    SD_MMC.remove(temp);
    return false;
  }
  if (had_dst) SD_MMC.remove(backup);
  LOG("config_copy_file: atomically replaced %s from %s (%u bytes)",
      dst, src, (unsigned)srcSize);
  return true;
}

int config_list_variants(const char* prefix, char names[][44], int max) {
  SD_FTP_StorageGuard guard;
  // Scan SD root for files matching "<prefix>NAME.ini" (case-insensitive
  // on the ".ini" suffix; the prefix is matched as written). Stores the
  // middle NAME portion in names[i]. Skips a file that's exactly the
  // active filename (prefix-without-trailing-dash + ".ini") to keep
  // wificonfig.ini / pdpconfig.ini out of the picker.
  if (max <= 0) return 0;
  int count = 0;

  fs::File root = SD_MMC.open("/");
  if (!root) return 0;

  size_t plen = strlen(prefix);
  for (fs::File f = root.openNextFile(); f && count < max;
       f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char* fullname = f.name();
      const char* slash = strrchr(fullname, '/');
      const char* base  = slash ? slash + 1 : fullname;
      size_t blen = strlen(base);

      // prefix match
      if (strncmp(base, prefix, plen) == 0 &&
          blen > plen + 4 /* at least 1 char + ".ini" */ &&
          strcasecmp(base + blen - 4, ".ini") == 0) {
        size_t midlen = blen - plen - 4;
        if (midlen > 0 && midlen < 43) {
          memcpy(names[count], base + plen, midlen);
          names[count][midlen] = 0;
          count++;
        }
      }
    }
    f.close();
  }
  root.close();
  return count;
}

// -------- printer --------

void config_print(const AppConfig& cfg) {
  LOG("---- /wificonfig.ini + /pdpconfig.ini effective values ----");
  LOG("[system]  title=\"%s\"  version=\"%s\"  build=\"%s\"",
      cfg.title.c_str(), cfg.version.c_str(), cfg.build.c_str());
  LOG("[wifi]    ssid=\"%s\"  hostname=\"%s\"  (password=%d chars)",
      cfg.wifi_ssid.c_str(), cfg.wifi_hostname.c_str(),
      (int)cfg.wifi_password.length());
  LOG("[telnet]  enabled=%s  port=%d",
      cfg.telnet_enabled ? "true" : "false", cfg.telnet_port);
  LOG("[console] boot_input=\"%s\" (%u bytes)",
      config_escape_bytes(cfg.boot_input, cfg.boot_input_len).c_str(),
      (unsigned)cfg.boot_input_len);
  LOG("[serial1] enabled=%s  CSR=776500  RX-vector=300  TX-vector=304",
      cfg.serial1_enabled ? "true" : "false");
  LOG("[ftp]     enabled=%s  port=%d  user=\"%s\" (password=%d chars)",
      cfg.ftp_enabled ? "true" : "false", cfg.ftp_port,
      cfg.ftp_user.c_str(), (int)cfg.ftp_password.length());
  LOG("[diag]    pcping=%d sec%s  serialdelay=%d ms  io_trace=%d  clock_trace=%d  console_trace=%d  trace=%s  v4b_quirks=%s  kwp_enabled=%s",
      cfg.diag_pcping_sec, cfg.diag_pcping_sec <= 0 ? " (disabled)" : "",
      cfg.diag_serialdelay_ms,
      cfg.diag_io_trace,
      cfg.diag_clock_trace,
      cfg.diag_console_trace,
      cfg.diag_trace ? "true" : "false",
      cfg.v4b_quirks  ? "true" : "false",
      cfg.kwp_enabled ? "true (V7 mode)" : "false (V4B-safe)");
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else boot_name = (cfg.boot_drive == 'a') ? "dl0"
                 : (cfg.boot_drive == 'b') ? "dl1"
                 : (cfg.boot_drive == 'c') ? "dl2"
                 : (cfg.boot_drive == 'd') ? "dl3" : "?";
  LOG("[disks]   dl0=\"%s\"  dl1=\"%s\"",
      cfg.disk_a.c_str(), cfg.disk_b.c_str());
  LOG("          dl2=\"%s\"  dl3=\"%s\"",
      cfg.disk_c.c_str(), cfg.disk_d.c_str());
  LOG("          rk0=\"%s\"  rp0=\"%s\" (%s)  boot=%s",
      cfg.disk_rk0.c_str(), cfg.disk_rp0.c_str(),
      cfg.disk_rp0_type.c_str(), boot_name);
  LOG("--------------------------------------");
}
