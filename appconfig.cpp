#include "appconfig.h"
#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

#include "sd_fs.h"
#include <stdlib.h>    // strtod for boot_input <<seconds>> markers
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

static int clamp_mem_size_kw(long value) {
  if (value < AppConfig::MEM_SIZE_KW_MIN) return AppConfig::MEM_SIZE_KW_MIN;
  if (value > AppConfig::MEM_SIZE_KW_MAX) return AppConfig::MEM_SIZE_KW_MAX;
  return (int)value;
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

// Decode escaped console text into raw bytes (shared by boot_input / boot_script).
static size_t decode_escaped_bytes(const String& s, uint8_t* out, size_t out_max) {
  size_t n = 0;
  for (int i = 0; i < (int)s.length() && n < out_max; i++) {
    uint8_t b = (uint8_t)s[i];

    if (s[i] == '^' && i + 1 < (int)s.length()) {
      char c = s[++i];
      if (c == '?') b = 0x7f;
      else          b = ((uint8_t)c) & 0x1f;
    } else if (s[i] == '\\' && i + 1 < (int)s.length()) {
      char c = s[++i];
      switch (c) {
        case 'r': b = '\r'; break;
        case 'n': b = '\n'; break;
        case 't': b = '\t'; break;
        case 'b': b = '\b'; break;
        case 'f': b = '\f'; break;
        case 'e': b = 0x1b; break;
        case 's': b = ' ';  break;
        case '\\': b = '\\'; break;
        case '"': b = '"'; break;
        case '\'': b = '\''; break;
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
          b = (uint8_t)v;
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
            b = (uint8_t)v;
          } else {
            b = (uint8_t)c;
          }
          break;
      }
    }

    out[n++] = b;
  }
  return n;
}

bool config_parse_ipv4(const char* s, uint32_t* out_host_order) {
  if (!s || !out_host_order) return false;
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  *out_host_order = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)c << 8) | (uint32_t)d;
  return true;
}

void config_format_ipv4(uint32_t host_order, char* buf, size_t buflen) {
  if (!buf || buflen == 0) return;
  snprintf(buf, buflen, "%u.%u.%u.%u",
           (unsigned)((host_order >> 24) & 0xff),
           (unsigned)((host_order >> 16) & 0xff),
           (unsigned)((host_order >> 8) & 0xff),
           (unsigned)(host_order & 0xff));
}

bool config_parse_mac(const char* s, uint8_t mac[6]) {
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

void config_format_mac(const uint8_t mac[6], char* buf, size_t buflen) {
  if (!buf || buflen == 0) return;
  if (!mac) {
    buf[0] = 0;
    return;
  }
  snprintf(buf, buflen, "%02X-%02X-%02X-%02X-%02X-%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void config_apply_ethernet_defaults(AppConfig& cfg) {
  cfg.eth_enabled = false;
  const uint8_t def_mac[6] = { 0x08, 0x00, 0x2B, 0x11, 0x70, 0x01 };
  memcpy(cfg.eth_mac, def_mac, 6);
  cfg.eth_guest_ip   = 0x0A0B0002u;  // 10.11.0.2
  cfg.eth_guest_mask = 0xFFFFFF00u;  // 255.255.255.0
  cfg.eth_gateway_ip = 0x0A0B0001u;  // 10.11.0.1
}

void config_set_boot_input(AppConfig& cfg, const String& encoded) {
  cfg.boot_input_len = 0;
  cfg.boot_input_segment_count = 0;
  memset(cfg.boot_input, 0, sizeof(cfg.boot_input));
  memset(cfg.boot_input_segments, 0, sizeof(cfg.boot_input_segments));

  String s = unquote_config_value(encoded);
  if (s.length() == 0) return;

  static constexpr uint32_t kDelayMinMs = 100;     // 0.1 s
  static constexpr uint32_t kDelayMaxMs = 120000;  // 120 s

  uint32_t pending_delay_ms = 0;
  String text_chunk;

  auto flush_text = [&]() {
    if (text_chunk.length() == 0 && pending_delay_ms == 0) return;

    // Delay-only marker (no following text yet): keep accumulating into
    // pending_delay_ms until text arrives, unless we need a delay-only
    // segment at end — handled after the loop.
    if (text_chunk.length() == 0) return;

    if (cfg.boot_input_segment_count >= AppConfig::BOOT_INPUT_MAX_SEGMENTS) {
      LOGE("boot_input: truncated to %u segments",
           (unsigned)AppConfig::BOOT_INPUT_MAX_SEGMENTS);
      text_chunk = "";
      return;
    }

    AppConfig::BootInputSegment& seg =
        cfg.boot_input_segments[cfg.boot_input_segment_count];
    seg.delay_ms = pending_delay_ms;
    pending_delay_ms = 0;
    seg.data_len = (uint8_t)decode_escaped_bytes(
        text_chunk, seg.data, AppConfig::BootInputSegment::DATA_MAX);
    text_chunk = "";

    // Also append to flat boot_input[] for byte-count logging.
    for (uint8_t i = 0; i < seg.data_len &&
         cfg.boot_input_len < AppConfig::BOOT_INPUT_MAX; i++) {
      cfg.boot_input[cfg.boot_input_len++] = seg.data[i];
    }

    if (seg.data_len > 0 || seg.delay_ms > 0)
      cfg.boot_input_segment_count++;
  };

  auto parse_delay_marker = [&](int open_at) -> int {
    // s[open_at] == '<' and s[open_at+1] == '<'; find closing '>>'.
    int close = -1;
    for (int j = open_at + 2; j + 1 < (int)s.length(); j++) {
      if (s[j] == '>' && s[j + 1] == '>') {
        close = j;
        break;
      }
    }
    if (close < 0) return -1;  // not a closed marker

    String num = s.substring(open_at + 2, close);
    num.trim();
    char* endp = nullptr;
    const char* cstr = num.c_str();
    double sec = strtod(cstr, &endp);
    if (endp == cstr || (endp && *endp != '\0')) {
      LOGE("boot_input: ignoring invalid delay marker <<%s>>", num.c_str());
      return close + 2;  // skip marker, inject nothing
    }

    double ms_d = sec * 1000.0;
    if (ms_d < (double)kDelayMinMs) {
      LOGE("boot_input: delay %.3fs clamped to 0.1s", sec);
      ms_d = (double)kDelayMinMs;
    } else if (ms_d > (double)kDelayMaxMs) {
      LOGE("boot_input: delay %.3fs clamped to 120s", sec);
      ms_d = (double)kDelayMaxMs;
    }
    uint32_t ms = (uint32_t)(ms_d + 0.5);

    // Flush any text before this marker, then accumulate delay.
    flush_text();
    // If pending delay already set (stacked markers), add sequentially by
    // emitting a delay-only segment first when text_chunk is empty.
    if (pending_delay_ms > 0 && text_chunk.length() == 0) {
      if (cfg.boot_input_segment_count < AppConfig::BOOT_INPUT_MAX_SEGMENTS) {
        AppConfig::BootInputSegment& seg =
            cfg.boot_input_segments[cfg.boot_input_segment_count];
        seg.delay_ms = pending_delay_ms;
        seg.data_len = 0;
        cfg.boot_input_segment_count++;
      }
      pending_delay_ms = 0;
    }
    pending_delay_ms += ms;
    return close + 2;
  };

  int i = 0;
  while (i < (int)s.length()) {
    if (i + 1 < (int)s.length() && s[i] == '<' && s[i + 1] == '<') {
      int next = parse_delay_marker(i);
      if (next > i) {
        i = next;
        continue;
      }
    }
    text_chunk += s[i];
    i++;
  }

  flush_text();

  // Trailing delay with no following text: still schedule a wait (no keys).
  if (pending_delay_ms > 0 &&
      cfg.boot_input_segment_count < AppConfig::BOOT_INPUT_MAX_SEGMENTS) {
    AppConfig::BootInputSegment& seg =
        cfg.boot_input_segments[cfg.boot_input_segment_count++];
    seg.delay_ms = pending_delay_ms;
    seg.data_len = 0;
  } else if (pending_delay_ms > 0) {
    LOGE("boot_input: truncated to %u segments",
         (unsigned)AppConfig::BOOT_INPUT_MAX_SEGMENTS);
  }

  // No markers and plain text: one segment with delay_ms=0 (immediate).
  // flush_text already created it when text_chunk was non-empty.
}

String config_format_boot_input(const AppConfig& cfg) {
  String out;
  for (uint8_t i = 0; i < cfg.boot_input_segment_count; i++) {
    const AppConfig::BootInputSegment& seg = cfg.boot_input_segments[i];
    if (seg.delay_ms > 0) {
      char marker[32];
      // Prefer one decimal place when not an integer number of seconds.
      if (seg.delay_ms % 1000 == 0)
        snprintf(marker, sizeof(marker), "<<%u>>",
                 (unsigned)(seg.delay_ms / 1000u));
      else
        snprintf(marker, sizeof(marker), "<<%.1f>>",
                 (double)seg.delay_ms / 1000.0);
      out += marker;
    }
    if (seg.data_len > 0)
      out += config_escape_bytes(seg.data, seg.data_len);
  }
  return out;
}

// Trim only space/tab around boot_script separators. Do not use isspace():
// CR/LF must survive so expect clauses can include \r \n (literal or escaped).
static String trim_boot_script_ws(const String& s) {
  int a = 0, b = (int)s.length();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return s.substring(a, b);
}

void config_set_boot_script(AppConfig& cfg, const String& encoded) {
  cfg.boot_script_count = 0;
  String s = unquote_config_value(encoded);
  s = trim_boot_script_ws(s);
  if (s.length() == 0) return;

  int start = 0;
  while (start <= (int)s.length() &&
         cfg.boot_script_count < AppConfig::BOOT_SCRIPT_MAX_STEPS) {
    int sep = -1;
    for (int i = start; i + 1 < (int)s.length(); i++) {
      if (s[i] == '|' && s[i + 1] == '|') {
        sep = i;
        break;
      }
    }
    String step = (sep < 0) ? s.substring(start) : s.substring(start, sep);
    step = trim_boot_script_ws(step);
    if (step.length() > 0) {
      int arrow = step.indexOf("=>");
      if (arrow < 0) {
        LOGE("boot_script: step %u missing '=>' separator: %s",
             (unsigned)cfg.boot_script_count, step.c_str());
      } else {
        // Expect/reply keep \r \n and \040/\s; only pad spaces around =>.
        String expect_s = trim_boot_script_ws(step.substring(0, arrow));
        String reply_s  = trim_boot_script_ws(step.substring(arrow + 2));
        AppConfig::BootScriptStep& out =
            cfg.boot_script[cfg.boot_script_count];
        out.expect_len = (uint8_t)decode_escaped_bytes(
            expect_s, out.expect, AppConfig::BootScriptStep::EXPECT_MAX);
        out.reply_len = (uint8_t)decode_escaped_bytes(
            reply_s, out.reply, AppConfig::BootScriptStep::REPLY_MAX);
        if (out.expect_len == 0 && out.reply_len == 0) {
          LOGE("boot_script: ignoring empty step %u",
               (unsigned)cfg.boot_script_count);
        } else {
          cfg.boot_script_count++;
        }
      }
    }
    if (sep < 0) break;
    start = sep + 2;
  }

  // Warn if more steps were provided than we can store.
  if (start < (int)s.length()) {
    for (int i = start; i + 1 < (int)s.length(); i++) {
      if (s[i] == '|' && s[i + 1] == '|') {
        LOGE("boot_script: truncated to %u steps (max %u)",
             (unsigned)AppConfig::BOOT_SCRIPT_MAX_STEPS,
             (unsigned)AppConfig::BOOT_SCRIPT_MAX_STEPS);
        break;
      }
    }
  }
}

String config_format_boot_script(const AppConfig& cfg) {
  String out;
  for (uint8_t i = 0; i < cfg.boot_script_count; i++) {
    if (i) out += " || ";
    out += config_escape_bytes(cfg.boot_script[i].expect,
                               cfg.boot_script[i].expect_len);
    out += " => ";
    out += config_escape_bytes(cfg.boot_script[i].reply,
                               cfg.boot_script[i].reply_len);
  }
  return out;
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
      case ' ':
        // Leading/trailing spaces would be eaten by boot_script separator
        // trim; always emit octal so expect clauses round-trip (\040).
        if (i == 0 || i + 1 == len)
          out += "\\040";
        else
          out += ' ';
        break;
      default:
        if (c >= 32 && c < 127) out += (char)c;
        else {
          snprintf(tmp, sizeof(tmp), "\\%03o", (unsigned)c);
          out += tmp;
        }
        break;
    }
  }
  return out;
}

// -------- SD --------

bool sd_mount() {
#if VPDP_SD_BACKEND == VPDP_SD_SPI_IDF
  // CrowPanel: ESP-IDF SDSPI, CS hard-tied → GPIO_NUM_NC. DIP S1=1 S0=1.
  if (!crow_sd_mount()) return false;
  uint8_t type = SD_FS.cardType();
  if (type == CARD_NONE) {
    LOGE("No SD card detected");
    return false;
  }
  const char* tname = (type == CARD_MMC)  ? "MMC"
                    : (type == CARD_SD)   ? "SDSC"
                    : (type == CARD_SDHC) ? "SDHC"
                                          : "?";
  uint64_t mb = SD_FS.cardSize() / (1024ULL * 1024ULL);
  LOG("SD mounted: type=%s size=%llu MB", tname, (unsigned long long)mb);
  return true;
#else
  SD_FS.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  if (!SD_FS.begin("/sdcard", false /*1bit*/, false /*format*/,
                    20000 /*freq*/, SD_MAX_OPEN_FILES)) {
    LOGE("SD_FS.begin() failed");
    return false;
  }
  uint8_t type = SD_FS.cardType();
  if (type == CARD_NONE) {
    LOGE("No SD card detected");
    return false;
  }
  const char* tname = (type == CARD_MMC)  ? "MMC"
                    : (type == CARD_SD)   ? "SDSC"
                    : (type == CARD_SDHC) ? "SDHC"
                                          : "?";
  uint64_t mb = SD_FS.cardSize() / (1024ULL * 1024ULL);
  LOG("SD mounted: type=%s size=%llu MB", tname, (unsigned long long)mb);
  return true;
#endif
}

// -------- defaults --------

void config_apply_compiled_defaults(AppConfig& cfg) {
  cfg.title         = APP_TITLE;
  cfg.version       = APP_VERSION;
  cfg.build         = APP_BUILD_DATE;
  cfg.mem_size_kw   = AppConfig::MEM_SIZE_KW_MAX;

  cfg.wifi_ssid     = WIFI_SSID;
  cfg.wifi_password = WIFI_PASS;
  cfg.wifi_hostname = WIFI_HOSTNAME;

  cfg.telnet_enabled = true;
  cfg.telnet_port    = TELNET_PORT;

  cfg.boot_input_len = 0;
  cfg.boot_input_segment_count = 0;
  cfg.boot_script_count = 0;
  cfg.serial1_enabled = false;
  config_apply_ethernet_defaults(cfg);

  cfg.ftp_enabled    = true;
  cfg.ftp_port       = FTP_PORT;
  cfg.ftp_user       = FTP_DEFAULT_USER;
  cfg.ftp_password   = FTP_DEFAULT_PASS;

  cfg.diag_pcping_sec = 5;
  cfg.diag_serialdelay_ms = 20;
  cfg.diag_io_trace   = 0;
  cfg.diag_clock_trace = 0;
  cfg.diag_console_trace = 0;
  cfg.diag_dl_trace = 0;
  cfg.diag_rp_trace = 0;
  cfg.diag_du_trace = 0;
  cfg.diag_trace      = false;
  cfg.diag_break_pc   = 0;
  cfg.kwp_enabled     = false;

  cfg.disk_a        = DEFAULT_DL0_IMG;
  cfg.disk_b        = DEFAULT_DL1_IMG;
  cfg.disk_c        = DEFAULT_DL2_IMG;
  cfg.disk_d        = DEFAULT_DL3_IMG;
  cfg.disk_rk0      = "";
  cfg.disk_rp0      = "";
  cfg.disk_du0      = "";
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
    else if (key == "mem_size_kw") cfg.mem_size_kw = clamp_mem_size_kw(val.toInt());
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
    else if (key == "boot_script")
      config_set_boot_script(cfg, val);
  } else if (section == "serial1") {
    if      (key == "enabled") cfg.serial1_enabled = truthy(val);
  } else if (section == "ethernet") {
    if (key == "enabled") {
      cfg.eth_enabled = truthy(val);
    } else if (key == "mac") {
      uint8_t mac[6];
      if (config_parse_mac(val.c_str(), mac))
        memcpy(cfg.eth_mac, mac, 6);
      else
        LOGE("ethernet: bad mac \"%s\"", val.c_str());
    } else if (key == "guest_ip") {
      uint32_t ip = 0;
      if (config_parse_ipv4(val.c_str(), &ip))
        cfg.eth_guest_ip = ip;
      else
        LOGE("ethernet: bad guest_ip \"%s\"", val.c_str());
    } else if (key == "guest_mask" || key == "mask" || key == "netmask") {
      uint32_t ip = 0;
      if (config_parse_ipv4(val.c_str(), &ip))
        cfg.eth_guest_mask = ip;
      else
        LOGE("ethernet: bad guest_mask \"%s\"", val.c_str());
    } else if (key == "gateway_ip" || key == "gateway") {
      uint32_t ip = 0;
      if (config_parse_ipv4(val.c_str(), &ip))
        cfg.eth_gateway_ip = ip;
      else
        LOGE("ethernet: bad gateway_ip \"%s\"", val.c_str());
    }
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
    else if (key == "dl_trace") {
      long count = val.toInt();
      cfg.diag_dl_trace = count < 0 ? 0
                        : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "rp_trace" || key == "dp_trace") {
      long count = val.toInt();
      cfg.diag_rp_trace = count < 0 ? 0
                        : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "du_trace") {
      long count = val.toInt();
      cfg.diag_du_trace = count < 0 ? 0
                        : count > 1000000 ? 1000000 : (int)count;
    }
    else if (key == "trace")      cfg.diag_trace = truthy(val);
    else if (key == "break") {
      // Octal virtual PC. 0 / off / clear / empty disables.
      String v = to_lower(val);
      if (v.length() == 0 || v == "0" || v == "off" || v == "clear" ||
          v == "none" || v == "-") {
        cfg.diag_break_pc = 0;
      } else {
        char* end = nullptr;
        unsigned long pc = strtoul(val.c_str(), &end, 8);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end && !*end && !(pc & 1UL) && pc <= 0177777UL)
          cfg.diag_break_pc = (uint16_t)pc;
      }
    }
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
    else if (key == "du0")      cfg.disk_du0 = val;
    else if (key == "rp0_type") cfg.disk_rp0_type = to_lower(val);
    else if (key == "boot") {
      String v = to_lower(val);
      cfg.boot_kind = AppConfig::BK_RL;
      if      (v == "dl0" || v == "rl0" || v == "0") cfg.boot_drive = 'a';
      else if (v == "dl1" || v == "rl1" || v == "1") cfg.boot_drive = 'b';
      else if (v == "dl2" || v == "rl2" || v == "2") cfg.boot_drive = 'c';
      else if (v == "dl3" || v == "rl3" || v == "3") cfg.boot_drive = 'd';
      // rk0 (DEC) / dk0 (Bell Labs Unix V6 device name) both mean the RK05.
      else if (v == "rk0" || v == "dk0") {
        cfg.boot_drive = 'a';
        cfg.boot_kind  = AppConfig::BK_RK;
      }
      // rp0 / hp0 / dp0: RH11/RP MASSBUS pack (DP is the older RP11 name).
      else if (v == "rp0" || v == "hp0" || v == "dp0") {
        cfg.boot_drive = 'a';
        cfg.boot_kind  = AppConfig::BK_RP;
      }
      else if (v == "du0") {
        cfg.boot_drive = 'a';
        cfg.boot_kind  = AppConfig::BK_DU;
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
  if (SD_FS.exists(path)) return;
  char backup[192];
  if (snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) return;
  if (SD_FS.exists(backup)) {
    if (SD_FS.rename(backup, path))
      LOG("Restored interrupted config update: %s", path);
    else
      LOGE("Could not restore config backup %s", backup);
  }
}

static bool parse_config_file(AppConfig& cfg, const char* path, ConfigDomain domain) {
  SD_FTP_StorageGuard guard;
  recover_config_backup(path);
  File f = SD_FS.open(path, FILE_READ);
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

static void reset_pdp_reload_state(AppConfig& cfg) {
  // An emulator-only reload must start from the same PDP-domain defaults as
  // a hard board reset. Preserve network service settings, which come from
  // /wificonfig.ini and remain live across a guest reboot.
  AppConfig defaults;
  config_apply_compiled_defaults(defaults);

  cfg.title = defaults.title;
  cfg.mem_size_kw = defaults.mem_size_kw;
  cfg.boot_input_len = 0;
  cfg.boot_input_segment_count = 0;
  cfg.boot_script_count = 0;
  cfg.serial1_enabled = defaults.serial1_enabled;
  cfg.eth_enabled = defaults.eth_enabled;
  memcpy(cfg.eth_mac, defaults.eth_mac, 6);
  cfg.eth_guest_ip = defaults.eth_guest_ip;
  cfg.eth_guest_mask = defaults.eth_guest_mask;
  cfg.eth_gateway_ip = defaults.eth_gateway_ip;

  cfg.diag_pcping_sec = defaults.diag_pcping_sec;
  cfg.diag_serialdelay_ms = defaults.diag_serialdelay_ms;
  cfg.diag_io_trace = defaults.diag_io_trace;
  cfg.diag_clock_trace = defaults.diag_clock_trace;
  cfg.diag_console_trace = defaults.diag_console_trace;
  cfg.diag_dl_trace = defaults.diag_dl_trace;
  cfg.diag_rp_trace = defaults.diag_rp_trace;
  cfg.diag_du_trace = defaults.diag_du_trace;
  cfg.diag_trace = defaults.diag_trace;
  cfg.diag_break_pc = defaults.diag_break_pc;
  cfg.kwp_enabled = defaults.kwp_enabled;

  // Missing or blank disk keys mean dismounted. DU0 must be cleared along
  // with the older controllers; retaining it caused media from a previous
  // profile to be reopened on every emulator reset.
  cfg.disk_a = "";
  cfg.disk_b = "";
  cfg.disk_c = "";
  cfg.disk_d = "";
  cfg.disk_rk0 = "";
  cfg.disk_rp0 = "";
  cfg.disk_du0 = "";
  cfg.disk_rp0_type = defaults.disk_rp0_type;
  cfg.boot_drive = defaults.boot_drive;
  cfg.boot_kind = defaults.boot_kind;
}

bool config_load_pdp(AppConfig& cfg) {
  reset_pdp_reload_state(cfg);

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
  File f = SD_FS.open(WIFI_CFG_PATH, FILE_WRITE);
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
  File f = SD_FS.open(PDP_CFG_PATH, FILE_WRITE);
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
  f.println("; mem_size_kw is PDP memory size in 1024-word units.");
  f.println("; Range: 32..2048 KW. 2048 KW is the full 4 MB PDP-11/70 address space.");
  f.printf("mem_size_kw = %d\r\n", cfg.mem_size_kw);
  f.println();
  f.println("[console]");
  f.println("; boot_input is injected into the KL11 input queue after each");
  f.println("; PDP-11 boot/reset. Escapes: \\r \\n \\t \\e \\xHH \\ooo ^C ^[ ^?.");
  f.println("; Optional <<seconds>> delays (0.1..120) between bursts, e.g.");
  f.println(";   boot_input = \"<<2.5>>START\\r<<2>>\\r\"");
  f.printf("boot_input = \"%s\"\r\n",
           config_format_boot_input(cfg).c_str());
  f.println("; boot_script waits for prompt text (case-insensitive), then");
  f.println("; injects the reply. Steps: expect => reply || expect => reply");
  f.printf("boot_script = \"%s\"\r\n",
           config_format_boot_script(cfg).c_str());
  f.println();
  f.println("[serial1]");
  f.println("; Optional second DL11-compatible TTY at 0176500. The TTY0 VPDP");
  f.println("; command channel and direct SD file commands are always available;");
  f.println("; this setting enables only TT1 background file streaming.");
  f.printf("enabled = %s\r\n", cfg.serial1_enabled ? "true" : "false");
  f.println();
  f.println("[ethernet]");
  f.println("; DEUNA Unibus Ethernet (L3 NAT onto WiFi STA). Default off.");
  f.println("; When enabled, presents DEUNA at 174510 (vec 120, BR5).");
  f.println("; Phase 2: CSR/port commands + TX sink. TCP/IP NAT comes later.");
  f.println("; Guest uses a private subnet; host Telnet/FTP keep the STA IP.");
  f.printf("enabled   = %s\r\n", cfg.eth_enabled ? "true" : "false");
  {
    char macbuf[24];
    char ipbuf[16];
    config_format_mac(cfg.eth_mac, macbuf, sizeof(macbuf));
    f.printf("mac       = %s\r\n", macbuf);
    config_format_ipv4(cfg.eth_guest_ip, ipbuf, sizeof(ipbuf));
    f.printf("guest_ip  = %s\r\n", ipbuf);
    config_format_ipv4(cfg.eth_guest_mask, ipbuf, sizeof(ipbuf));
    f.printf("guest_mask = %s\r\n", ipbuf);
    config_format_ipv4(cfg.eth_gateway_ip, ipbuf, sizeof(ipbuf));
    f.printf("gateway_ip = %s\r\n", ipbuf);
  }
  f.println();
  f.println("[diag]");
  f.println("; pcping      = seconds between host's periodic PC/register dump");
  f.println(";               to USB-Serial. 0 disables it (so do large values).");
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
  f.println("; dl_trace   = log the next N kek RL/DL controller and host");
  f.println(";              disk events, then stop. 0 disables.");
  f.println("; rp_trace   = log the next N kek RH/RP (DP) controller and host");
  f.println(";              disk events, then stop. 0 disables. Alias: dp_trace.");
  f.println("; du_trace   = log the next N UDA50/MSCP init, ring, command,");
  f.println(";              response, interrupt, and DMA events, then stop.");
  f.println("; break      = arm a PC breakpoint before guest boot (octal VA).");
  f.println(";              0 disables. Example: break = 04642 pauses on first");
  f.println(";              fetch of that PC so H can dump the lead-in history.");
  f.printf("pcping      = %d\r\n", cfg.diag_pcping_sec);
  f.printf("serialdelay = %d\r\n", cfg.diag_serialdelay_ms);
  f.printf("io_trace    = %d\r\n", cfg.diag_io_trace);
  f.printf("clock_trace = %d\r\n", cfg.diag_clock_trace);
  f.printf("console_trace = %d\r\n", cfg.diag_console_trace);
  f.printf("dl_trace    = %d\r\n", cfg.diag_dl_trace);
  f.printf("rp_trace    = %d\r\n", cfg.diag_rp_trace);
  f.printf("du_trace    = %d\r\n", cfg.diag_du_trace);
  f.printf("trace       = %s\r\n", cfg.diag_trace ? "true" : "false");
  f.printf("break       = %06o\r\n", (unsigned)cfg.diag_break_pc);
  f.printf("kwp_enabled = %s\r\n", cfg.kwp_enabled ? "true" : "false");
  f.println();
  f.println("[disks]");
  f.println("; dl0..dl3 = RL11 units (RL01/RL02 disk packs).");
  f.println("; rk0      = RK05 2.5 MB disk pack (e.g. RT-11).");
  f.println("; rp0      = RH11/RP disk pack (bootable with boot=rp0).");
  f.println("; rp0_type = rp04, rp05, rp06, or rp07. Default rp06.");
  f.println("; Leave a slot blank to dismount it at boot.");
  f.printf("dl0  = %s\r\n", cfg.disk_a.c_str());
  f.printf("dl1  = %s\r\n", cfg.disk_b.c_str());
  f.printf("dl2  = %s\r\n", cfg.disk_c.c_str());
  f.printf("dl3  = %s\r\n", cfg.disk_d.c_str());
  f.printf("rk0  = %s\r\n", cfg.disk_rk0.c_str());
  f.printf("rp0  = %s\r\n", cfg.disk_rp0.c_str());
  f.printf("du0  = %s\r\n", cfg.disk_du0.c_str());
  f.printf("rp0_type = %s\r\n", cfg.disk_rp0_type.c_str());
  // Friendly boot value: dl0/dl1/dl2/dl3/rk0/rp0/du0.
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else if (cfg.boot_kind == AppConfig::BK_RP) boot_name = "rp0";
  else if (cfg.boot_kind == AppConfig::BK_DU) boot_name = "du0";
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

  File s = SD_FS.open(src, FILE_READ);
  if (!s) { LOGE("config_copy_file: can't open %s for read", src); return false; }
  uint32_t srcSize = (uint32_t)s.size();

  if (SD_FS.exists(temp)) SD_FS.remove(temp);
  File d = SD_FS.open(temp, FILE_WRITE);
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

  File v = SD_FS.open(temp, FILE_READ);
  uint32_t verifySize = v ? (uint32_t)v.size() : 0;
  if (v) v.close();
  if (!copy_ok || total != srcSize || verifySize != srcSize) {
    LOGE("config_copy_file: temporary copy failed src=%u written=%u on-disk=%u",
         (unsigned)srcSize, (unsigned)total, (unsigned)verifySize);
    SD_FS.remove(temp);
    return false;
  }

  if (SD_FS.exists(backup)) SD_FS.remove(backup);
  bool had_dst = SD_FS.exists(dst);
  if (had_dst && !SD_FS.rename(dst, backup)) {
    LOGE("config_copy_file: can't preserve %s as %s", dst, backup);
    SD_FS.remove(temp);
    return false;
  }
  if (!SD_FS.rename(temp, dst)) {
    LOGE("config_copy_file: can't activate %s", dst);
    if (had_dst && !SD_FS.rename(backup, dst))
      LOGE("config_copy_file: FAILED to restore %s from %s", dst, backup);
    SD_FS.remove(temp);
    return false;
  }
  if (had_dst) SD_FS.remove(backup);
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

  fs::File root = SD_FS.open("/");
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
  LOG("[system]  title=\"%s\"  version=\"%s\"  build=\"%s\"  mem_size_kw=%d",
      cfg.title.c_str(), cfg.version.c_str(), cfg.build.c_str(),
      cfg.mem_size_kw);
  LOG("[wifi]    ssid=\"%s\"  hostname=\"%s\"  (password=%d chars)",
      cfg.wifi_ssid.c_str(), cfg.wifi_hostname.c_str(),
      (int)cfg.wifi_password.length());
  LOG("[telnet]  enabled=%s  port=%d",
      cfg.telnet_enabled ? "true" : "false", cfg.telnet_port);
  LOG("[console] boot_input=\"%s\" (%u bytes, %u segment%s)",
      config_format_boot_input(cfg).c_str(),
      (unsigned)cfg.boot_input_len,
      (unsigned)cfg.boot_input_segment_count,
      cfg.boot_input_segment_count == 1 ? "" : "s");
  LOG("[console] boot_script=\"%s\" (%u step%s)",
      config_format_boot_script(cfg).c_str(),
      (unsigned)cfg.boot_script_count,
      cfg.boot_script_count == 1 ? "" : "s");
  LOG("[serial1] enabled=%s  CSR=776500  RX-vector=300  TX-vector=304",
      cfg.serial1_enabled ? "true" : "false");
  {
    char macbuf[24];
    char gip[16], mask[16], gw[16];
    config_format_mac(cfg.eth_mac, macbuf, sizeof(macbuf));
    config_format_ipv4(cfg.eth_guest_ip, gip, sizeof(gip));
    config_format_ipv4(cfg.eth_guest_mask, mask, sizeof(mask));
    config_format_ipv4(cfg.eth_gateway_ip, gw, sizeof(gw));
    LOG("[ethernet] enabled=%s  mac=%s  guest=%s/%s  gateway=%s%s",
        cfg.eth_enabled ? "true" : "false", macbuf, gip, mask, gw,
        cfg.eth_enabled ? "  DEUNA@174510" : "");
  }
  LOG("[ftp]     enabled=%s  port=%d  user=\"%s\" (password=%d chars)",
      cfg.ftp_enabled ? "true" : "false", cfg.ftp_port,
      cfg.ftp_user.c_str(), (int)cfg.ftp_password.length());
  LOG("[diag]    pcping=%d sec%s  serialdelay=%d ms  io_trace=%d  clock_trace=%d  console_trace=%d  dl_trace=%d  rp_trace=%d  du_trace=%d  trace=%s  break=%06o%s  kwp_enabled=%s",
      cfg.diag_pcping_sec, cfg.diag_pcping_sec <= 0 ? " (disabled)" : "",
      cfg.diag_serialdelay_ms,
      cfg.diag_io_trace,
      cfg.diag_clock_trace,
      cfg.diag_console_trace,
      cfg.diag_dl_trace,
      cfg.diag_rp_trace,
      cfg.diag_du_trace,
      cfg.diag_trace ? "true" : "false",
      (unsigned)cfg.diag_break_pc,
      cfg.diag_break_pc == 0 ? " (disabled)" : "",
      cfg.kwp_enabled ? "true (V7 mode)" : "false (V4B-safe)");
  const char* boot_name;
  if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "rk0";
  else if (cfg.boot_kind == AppConfig::BK_RP) boot_name = "rp0";
  else if (cfg.boot_kind == AppConfig::BK_DU) boot_name = "du0";
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
  LOG("          du0=\"%s\"", cfg.disk_du0.c_str());
  LOG("--------------------------------------");
}
