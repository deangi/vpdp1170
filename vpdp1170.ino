//-------------------------------------------------------------------------------
// vpdp1170 - DEC PDP-11/70 emulator on ESP32-S3 display hosts
//
// Cloned from vpdp1140 on 2026-07-01 as the host scaffold for a PDP-11/70
// emulator. V1.0 boots Unix V6 through the kek PDP-11/70 CPU/MMU adapter;
// the inherited 11/40-derived core remains in the tree for reference and
// fallback while the kek device set is brought across in phases.
//
// Initial source candidate for the 11/70 engine is Folkert van Heusden's
// kek emulator (MIT), kept under _upstream_kek for reference/import work.
//
// The ESP32-S3 host scaffolding (TFT console, telnet, USB serial, SD images,
// touch settings menu, wificonfig.ini + pdpconfig.ini, dual-core split)
// carries over from v8088 unchanged; only the CPU core, I/O page dispatch,
// and disk/console wiring are PDP-11-specific.
//
// ---- Dual board (Arduino IDE) ----
// Select host in config.h: VPDP_BOARD_FREENOVE_28 or VPDP_BOARD_CROWPANEL_7
// (must be in config.h — an .ino-only #define does not reach .cpp files).
//
// Common Arduino Tools settings (both boards):
//   Tools → Board → ESP32S3 Dev Module (do not select an "...Octal" variant)
//   Tools → Flash Size → 16MB (128Mb)
//   Tools → Partition Scheme → Huge APP (3MB No OTA / 1MB SPIFFS)
//   Tools → PSRAM → OPI PSRAM (NOT QSPI PSRAM and NOT Disabled)
//
// Freenove ESP32-S3 2.8" (FNK0104B, COM18 typical):
//   TFT_eSPI with FNK0104B in User_Setup_Select.h
//   Tools → USB CDC On Boot → Enabled
//
// Elecrow CrowPanel Advance 7" (COM3 typical):
//   ESP32-S3-WROOM-1-N16R8: 16MB flash + 8MB OPI (octal) PSRAM.
//   Tools → Flash Size → 16MB (128Mb)  — required (module is N16R8).
//   Tools → PSRAM → OPI PSRAM (NOT QSPI PSRAM and NOT Disabled).
//   OPI PSRAM is mandatory for the 800x480 RGB framebuffer. If disabled or
//   misconfigured, free_psram=0 and drawing can crash with StoreProhibited.
//   LovyanGFX library; Tools → USB CDC On Boot → DISABLED
//   (CDC Enabled sends app Serial to native USB while the flash/monitor
//   COM is UART0 — ROM lines appear but [vpdp1170] LOGs look missing.)
//   DIP S1=1 S0=1 for TF card. STC8H backlight + GT911 power before gfx.init.
//   Touch: LGFX::getTouch() after gfx.init (do not reclaim Wire). Menu still
//   uses the Freenove 320x240 layout in the top-left of the 800x480 panel.
//
// V1.0 23-May-2026, Dean Gienger, Claude
// Set up to boot from a RL02 disk (10mb) - eventually support 2 RL02 disks (DL0 and DL1)
// and four RL11 units DL0..DL3.
//
// RL02K disks are single-platter cartridges with 512 tracks per side, 40 sectors per track, and 
// a sector size of 256 bytes, for a total capacity of 10Mb (10,485,760 bytes). They are used in 
// RL02 disk drives in conjunction with an RL11 Disk Controller.  These are front loading disk
// cartidges.  Round, about 15" diameter, about 3-4" thick.
//
// Sample disks: https://www.pcjs.org/software/dec/pdp11/disks/rl02k/xxdp/
// PDP 11/70 KB11-B/C, up to 4 Mb of memory, 
// KW11-L Line clock
// Optional KW11-P programmable clock
// Unibus connected RL and RK controller
// Masbus connected RP controller
// KL11 console
// Optional Serial1
// 
//------------------------------------------------------------------------------------------------
// Board select lives in config.h (VPDP_BOARD) — not here. An .ino-only #define
// does not reach console.cpp/ui.cpp and causes LGFX vs TFT_eSPI link errors.
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include "config.h"
#include "gfx.h"        // GfxDisplay = TFT_eSPI (Freenove) or LGFX (CrowPanel)
#include "sd_fs.h"      // SD_FS = SD_MMC (Freenove) or Crow SDSPI (Elecrow)
#if VPDP_HAS_WS2812
#include "Freenove_WS2812_Lib_for_ESP32.h"
#endif
#include "platform.h"
#include "secrets.h"
#include "appconfig.h"
#include "pdp_core.h"
#include "kl11.h"
#include "dd11.h"  // dd11::set_io_trace()
#include "kw11.h"
#include "kwp.h"   // kwp::enabled gate
#include "kek_deuna.h"
#include "eth_nat.h"
#include "disk.h"
#include "console.h"
#include "telnet.h"
#include "telnet_shell.h"
#include "ftp.h"
#include "touch.h"
#include "ui.h"
#include "dl11_file.h"
#include "emu_control.h"
#include "host_diag.h"
#include "boot_script.h"
#include "boot_input.h"

static GfxDisplay tft;
#if VPDP_HAS_WS2812
static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);
#endif
AppConfig cfg;             // non-static so ui.cpp (System Info screen,
                           // title display) can read it via the extern in
                           // appconfig.h. Only vpdp1170.ino writes it.

extern "C" void kek_tty_set_trace(uint32_t count);

static bool sd_ok = false;
static bool cpu_running = false;   // true once the PDP-11 is booting in loop()

// The PDP-11 runs on core 1 (loop); all TFT rendering runs on core 0
// (render_task). The settings menu is the only shared mutable UI state -
// this mutex guards it. The 80x25 console grid is snapshotted under a
// short spinlock in console_render so core-1 ANSI drains cannot tear a
// frame (cursor underline / scroll artifacts).
static SemaphoreHandle_t g_ui_mutex = nullptr;

enum BootState { BOOT_RUNNING, BOOT_OK, BOOT_FAIL };
static BootState boot_state = BOOT_RUNNING;

static void led(uint8_t r, uint8_t g, uint8_t b) {
#if VPDP_HAS_WS2812
  strip.setLedColorData(0, r, g, b);
  strip.show();
#else
  (void)r; (void)g; (void)b;
#endif
}

// Re-draw just the title row (top 22 px). Called once at boot before the
// config is loaded (shows APP_TITLE) and again after config_load_pdp so
// [system] title = ... from pdpconfig.ini takes effect on the boot screen.
static void tft_banner_title() {
  tft.fillRect(0, 0, TFT_W, 22, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  tft.printf("%s  v%s", title, APP_VERSION);
  gfx_writeback(tft, 0, 0, TFT_W, 22);
}

static void tft_banner() {
  tft.fillScreen(TFT_BLACK);
  tft_banner_title();
  tft.setCursor(4, 22);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("build %s", APP_BUILD_DATE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_writeback(tft);
}

static void tft_status(int row, const char* label, const char* value, uint16_t color) {
  int y = 50 + row * 18;
  tft.fillRect(0, y, TFT_W, 18, TFT_BLACK);
  tft.setCursor(4, y);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.print(label);
  tft.setTextColor(color, TFT_BLACK);
  tft.print(value);
  gfx_writeback(tft, 0, y, TFT_W, 18);
}

static void apply_runtime_pdp_config() {
  // Guest RAM size must track /pdpconfig.ini on every emulator reset, not
  // only during setup(). Without this, cold_boot() keeps the previous
  // profile's g_target_memory_kw while the reset banner still prints the
  // newly loaded cfg.mem_size_kw.
  pdp_core::set_target_memory_kw((uint32_t)cfg.mem_size_kw);

  dd11::set_io_trace((uint32_t)(cfg.diag_io_trace < 0
                                  ? 0 : cfg.diag_io_trace));
  kw11::set_clock_trace((uint32_t)(cfg.diag_clock_trace < 0
                                     ? 0 : cfg.diag_clock_trace));
  kl11::set_console_trace((uint32_t)(cfg.diag_console_trace < 0
                                      ? 0 : cfg.diag_console_trace));
  kek_tty_set_trace((uint32_t)(cfg.diag_console_trace < 0
                                ? 0 : cfg.diag_console_trace));
  pdp_core::set_dl_trace((uint32_t)(cfg.diag_dl_trace < 0
                                      ? 0 : cfg.diag_dl_trace));
  pdp_core::set_du_trace((uint32_t)(cfg.diag_du_trace < 0
                                      ? 0 : cfg.diag_du_trace));
  pdp_core::set_rp_trace((uint32_t)(cfg.diag_rp_trace < 0
                                      ? 0 : cfg.diag_rp_trace));
  kwp::enabled             = cfg.kwp_enabled;
  kek_deuna::set_enabled(cfg.eth_enabled);
  kek_deuna::set_mac(cfg.eth_mac);
  kek_deuna::set_network(cfg.eth_guest_ip, cfg.eth_guest_mask,
                         cfg.eth_gateway_ip);
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    eth_nat::set_sta_ip(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                        ((uint32_t)ip[2] << 8) | (uint32_t)ip[3]);
  } else {
    eth_nat::set_sta_ip(0);
  }
  pdp_core::set_trace(cfg.diag_trace);
  if (cfg.diag_break_pc != 0)
    pdp_core::monitor_break_set_pc(cfg.diag_break_pc);
  else
    pdp_core::monitor_break_clear();
  kl11::serial_in_delay_ms = (uint32_t)(cfg.diag_serialdelay_ms < 0 ? 0
                                      : cfg.diag_serialdelay_ms);
}

// Row map for the boot status display:
//   row 0 = PSRAM
//   row 1 = SD card
//   row 2 = /wificonfig.ini + /pdpconfig.ini
//   row 3 = boot drive image
//   row 4 = WiFi
//   row 5 = IP
//   row 6 = selected PDP core
//   row 7 = CPU status
enum {
  ROW_PSRAM = 0, ROW_SD, ROW_CFG, ROW_BOOT, ROW_WIFI, ROW_IP, ROW_CORE, ROW_CPU
};

// Boot drive unit label (e.g. "DL0", "RK0", "RP0") and the configured image
// path for the active boot slot.
static const char* boot_unit_label() {
  return cfg.boot_unit_label();
}
static const String& boot_image_path() {
  if (cfg.boot_kind == AppConfig::BK_RK) return cfg.disk_rk0;
  if (cfg.boot_kind == AppConfig::BK_RP) return cfg.disk_rp0;
  if (cfg.boot_kind == AppConfig::BK_DU) return cfg.disk_du0;
  int slot = (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
               ? (cfg.boot_drive - 'a') : 0;
  const String* paths[4] = { &cfg.disk_a, &cfg.disk_b, &cfg.disk_c, &cfg.disk_d };
  return *paths[slot];
}

static void wifi_connect() {
  const char* ssid = cfg.wifi_ssid.c_str();
  const char* pass = cfg.wifi_password.c_str();
  const char* host = cfg.wifi_hostname.length() ? cfg.wifi_hostname.c_str() : WIFI_HOSTNAME;

  if (cfg.wifi_ssid.length() == 0) {
    LOGE("WiFi SSID is empty - set [wifi] ssid= in /wificonfig.ini");
    tft_status(ROW_WIFI, "WiFi:  ", "no SSID in wificonfig.ini", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);

  LOG("WiFi connecting to \"%s\" (hostname=%s) ...", ssid, host);
  tft_status(ROW_WIFI, "WiFi:  ", "connecting...", TFT_YELLOW);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    LOG("WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
    tft_status(ROW_WIFI, "WiFi:  ", ssid, TFT_GREEN);
    tft_status(ROW_IP,   "IP:    ", WiFi.localIP().toString().c_str(), TFT_GREEN);
    IPAddress ip = WiFi.localIP();
    eth_nat::set_sta_ip(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                        ((uint32_t)ip[2] << 8) | (uint32_t)ip[3]);
    LOG("WiFi gateway=%s", WiFi.gatewayIP().toString().c_str());
    boot_state = BOOT_OK;
  } else {
    LOGE("WiFi connect timed out");
    tft_status(ROW_WIFI, "WiFi:  ", "FAILED", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    eth_nat::set_sta_ip(0);
    boot_state = BOOT_FAIL;
  }
}

static void sd_and_config_init() {
  tft_status(ROW_SD,  "SD:    ", "mounting...", TFT_YELLOW);
  if (sd_mount()) {
    char info[32];
    uint64_t mb = SD_FS.cardSize() / (1024ULL * 1024ULL);
    snprintf(info, sizeof(info), "OK  %llu MB", (unsigned long long)mb);
    tft_status(ROW_SD, "SD:    ", info, TFT_GREEN);
    sd_ok = true;
  } else {
    tft_status(ROW_SD, "SD:    ", "FAILED",     TFT_RED);
    sd_ok = false;
  }

  tft_status(ROW_CFG, "Cfg:   ", "(reading)", TFT_YELLOW);
  if (!sd_ok) {
    config_apply_compiled_defaults(cfg);
    tft_status(ROW_CFG, "Cfg:   ", "defaults (no SD)", TFT_YELLOW);
  } else {
    config_apply_compiled_defaults(cfg);
    bool wifi_existed = config_load_wifi(cfg);
    bool pdp_existed  = config_load_pdp(cfg);
    const char* msg =
        (wifi_existed && pdp_existed) ? "loaded wifi+pdp"
      : (wifi_existed)                ? "wrote default pdpconfig"
      : (pdp_existed)                 ? "wrote default wificonfig"
                                      : "wrote defaults (both)";
    uint16_t col = (wifi_existed && pdp_existed) ? TFT_GREEN : TFT_YELLOW;
    tft_status(ROW_CFG, "Cfg:   ", msg, col);
  }
  config_print(cfg);
  dl11_file::set_enabled(cfg.serial1_enabled);
  emu_control::init();

  // Push pdpconfig.ini runtime flags down before cpu_reset() / guest I/O.
  apply_runtime_pdp_config();

  // Show the boot drive's image path (e.g. "Boot DL0:" / "Boot RK0:").
  char boot_label[16];
  snprintf(boot_label, sizeof(boot_label), "Boot %s:", boot_unit_label());
  tft_status(ROW_BOOT, boot_label, "checking...", TFT_YELLOW);
  const String& bpath = boot_image_path();
  if (!sd_ok) {
    tft_status(ROW_BOOT, boot_label, "skipped (no SD)", TFT_DARKGREY);
  } else if (bpath.length() == 0) {
    tft_status(ROW_BOOT, boot_label, "(no image)", TFT_RED);
  } else {
    const bool boot_present = SD_FS.exists(bpath.c_str());
    tft_status(ROW_BOOT, boot_label,
               boot_present ? bpath.c_str() : "MISSING",
               boot_present ? TFT_GREEN : TFT_RED);
  }
}

// Mount guest drives from /pdpconfig.ini paths. DL0..DL3, RK0, and RP0 are
// independent host slots; boot= only chooses the bootstrap/controller path.
static void disks_mount() {
  if (!sd_ok) { LOGE("disks_mount: SD not available"); return; }
  for (int s = 0; s < DRIVE_COUNT; s++)
    disk_dismount(s);

  const String* paths[4] = {
    &cfg.disk_a, &cfg.disk_b, &cfg.disk_c, &cfg.disk_d
  };
  const char* unit_names[4] = {
    "DL0", "DL1", "DL2", "DL3"
  };
  for (int s = 0; s < 4; s++) {
    if (paths[s]->length() == 0) continue;
    bool ok = disk_mount(s, paths[s]->c_str());
    if (ok && !disk_validate_rl_mounted(s)) {
      uint32_t bytes = disk_size_bytes(s);
      LOGE("disks_mount %s: \"%s\" rejected, RL image size is %u bytes; expected RL01=%u or RL02=%u",
           unit_names[s], paths[s]->c_str(), (unsigned)bytes,
           (unsigned)DISK_RL01_IMAGE_BYTES,
           (unsigned)DISK_RL02_IMAGE_BYTES);
      disk_dismount(s);
      ok = false;
    }
    LOG("disks_mount %s: \"%s\" -> %s",
        unit_names[s], paths[s]->c_str(), ok ? "mounted" : "FAILED");
  }
  if (cfg.disk_rk0.length()) {
    bool ok = disk_mount(DRIVE_RK0, cfg.disk_rk0.c_str());
    LOG("disks_mount RK0: \"%s\" -> %s",
        cfg.disk_rk0.c_str(), ok ? "mounted" : "FAILED");
  }
  if (cfg.disk_rp0.length()) {
    bool ok = disk_mount(DRIVE_RP0, cfg.disk_rp0.c_str());
    LOG("disks_mount RP0 (%s): \"%s\" -> %s",
        cfg.disk_rp0_type.c_str(), cfg.disk_rp0.c_str(),
        ok ? "mounted" : "FAILED");
  }
  if (cfg.disk_du0.length()) {
    bool ok = disk_mount(DRIVE_DU0, cfg.disk_du0.c_str());
    LOG("disks_mount DU0: \"%s\" -> %s",
        cfg.disk_du0.c_str(), ok ? "mounted" : "FAILED");
  }
}

// Status bar drawn in the 40 px strip below the 80x25 console: drive activity
// indicators, IP address, telnet state and emulation speed.
static int boot_drive_slot() {
  if (cfg.boot_kind == AppConfig::BK_RK) return DRIVE_RK0;
  if (cfg.boot_kind == AppConfig::BK_RP) return DRIVE_RP0;
  if (cfg.boot_kind == AppConfig::BK_DU) return DRIVE_DU0;
  if (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
    return cfg.boot_drive - 'a';
  return DRIVE_A;
}

static const char* status_drive_label(int slot) {
  switch (slot) {
    case DRIVE_A:   return "DL0";
    case DRIVE_B:   return "DL1";
    case DRIVE_C:   return "DL2";
    case DRIVE_D:   return "DL3";
    case DRIVE_RK0: return "RK0";
    case DRIVE_RP0: return "RP0";
    case DRIVE_DU0: return "DU0";
    default:        return "?";
  }
}

static void draw_status_bar() {
  static uint32_t prev_io[DRIVE_COUNT] = {0};
  static uint32_t prev_inst = 0;
  static uint32_t prev_ms   = 0;
#if VPDP_BOARD == VPDP_BOARD_FREENOVE_28
  const int sy = CON_ROWS * CELL_H;          // 200

  tft.drawFastHLine(0, sy, TFT_W, TFT_DARKGREY);

  // Original Freenove 320x240 geometry: four compact drive pills across the
  // upper-left half of the 40 px status band.
  int visible_slots[4];
  int pill_count = 0;
  const int boot_slot = boot_drive_slot();
  visible_slots[pill_count++] = boot_slot;
  for (int s = 0; s < DRIVE_COUNT && pill_count < 4; s++) {
    if (s == boot_slot) continue;
    if (!disk_is_mounted(s)) continue;
    visible_slots[pill_count++] = s;
  }

  tft.fillRect(0, sy + 1, 156, 20, TFT_BLACK);
  for (int i = 0; i < pill_count; i++) {
    int s = visible_slots[i];
    uint32_t r = 0, w = 0;
    disk_stats(s, &r, &w);
    bool active = (r + w) != prev_io[s];
    prev_io[s] = r + w;
    uint16_t col = !disk_is_mounted(s) ? 0x2945
                 : active             ? TFT_YELLOW
                                      : TFT_GREEN;
    int bx = 6 + i * 36;
    tft.fillRoundRect(bx, sy + 5, 32, 16, 2, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(status_drive_label(s), bx + 16, sy + 13, 1);
  }
  tft.setTextDatum(TL_DATUM);

  uint32_t now  = millis();
  uint32_t inst = pdp_core::instruction_count();
  float mips = 0.0f;
  if (prev_ms && now > prev_ms && inst >= prev_inst)
    mips = (float)(inst - prev_inst) / (float)(now - prev_ms) / 1000.0f;
  prev_inst = inst;
  prev_ms   = now;

  tft.fillRect(156, sy + 1, TFT_W - 156, TFT_H - sy - 1, TFT_BLACK);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_WHITE : TFT_RED,
                   TFT_BLACK);
  tft.drawString(WiFi.status() == WL_CONNECTED
                   ? WiFi.localIP().toString().c_str() : "WiFi down",
                 158, sy + 6, 1);

  auto draw_net_pill = [&](int bx, const char* label, uint16_t col) {
    tft.fillRoundRect(bx, sy + 22, 26, 15, 2, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, bx + 13, sy + 29, 1);
  };
  const uint16_t COL_NET_OFF    = 0x2945;
  const uint16_t COL_NET_IDLE   = TFT_GREEN;
  const uint16_t COL_NET_ACTIVE = TFT_YELLOW;
  uint16_t tel_col = !telnet_listening() ? COL_NET_OFF
                   : telnet_connected()  ? COL_NET_ACTIVE
                                         : COL_NET_IDLE;
  uint16_t ftp_col = !ftp_listening() ? COL_NET_OFF
                  : ftp_connected()   ? COL_NET_ACTIVE
                                      : COL_NET_IDLE;
  draw_net_pill(158, "TEL", tel_col);
  draw_net_pill(188, "FTP", ftp_col);

  char mips_str[16];
  if (pdp_core::monitor_paused())
    snprintf(mips_str, sizeof(mips_str), "PAUSED");
  else if (mips > 0.0f && mips < 0.01f)
    snprintf(mips_str, sizeof(mips_str), "%.1f KIPS", mips * 1000.0f);
  else
    snprintf(mips_str, sizeof(mips_str), "%.2f MIPS", mips);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(mips_str, TFT_W - 4, sy + 22, 1);

  tft.fillRect(0, sy + 22, 156, TFT_H - sy - 22, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  tft.drawString(title, 6, sy + 24, 2);

  gfx_writeback(tft, 0, sy, TFT_W, TFT_H - sy);
#else
  const int sy = CON_ROWS * CELL_H;
  const int band = TFT_H - sy;
  const int rule_h = 3;
  // Larger pills on the CrowPanel 80 px band; Freenove's 40 px band stays
  // compact but still uses font 2 inside the pills.
  const bool tall = (band >= 60);
  const int pill_w = tall ? 52 : 40;
  const int pill_h = tall ? 28 : 18;
  const int pill_font = 2;
  const int pill_gap = tall ? 8 : 4;
  const int pill_y = sy + rule_h + (tall ? 6 : 2);

  // 3 px rule separating console from status chrome.
  tft.fillRect(0, sy, TFT_W, rule_h, TFT_DARKGREY);
  tft.fillRect(0, sy + rule_h, TFT_W, band - rule_h, TFT_BLACK);

  // Drive pills: leftmost is always the boot unit; remaining up to 3 are
  // other currently mounted drives (scan DL0..DL3, RK0, RP0).
  int visible_slots[4];
  int pill_count = 0;
  const int boot_slot = boot_drive_slot();
  visible_slots[pill_count++] = boot_slot;
  for (int s = 0; s < DRIVE_COUNT && pill_count < 4; s++) {
    if (s == boot_slot) continue;
    if (!disk_is_mounted(s)) continue;
    visible_slots[pill_count++] = s;
  }

  for (int i = 0; i < pill_count; i++) {
    int s = visible_slots[i];
    uint32_t r = 0, w = 0;
    disk_stats(s, &r, &w);
    bool active = (r + w) != prev_io[s];
    prev_io[s] = r + w;
    uint16_t col = !disk_is_mounted(s) ? 0x2945
                 : active             ? TFT_YELLOW
                                      : TFT_GREEN;
    int bx = 6 + i * (pill_w + pill_gap);
    tft.fillRoundRect(bx, pill_y, pill_w, pill_h, 4, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(status_drive_label(s), bx + pill_w / 2, pill_y + pill_h / 2,
                   pill_font);
  }
  tft.setTextDatum(TL_DATUM);

  // Emulation speed over the last interval.
  uint32_t now  = millis();
  uint32_t inst = pdp_core::instruction_count();
  float mips = 0.0f;
  if (prev_ms && now > prev_ms && inst >= prev_inst)
    mips = (float)(inst - prev_inst) / (float)(now - prev_ms) / 1000.0f;
  prev_inst = inst;
  prev_ms   = now;

  char mips_str[16];
  if (pdp_core::monitor_paused())
    snprintf(mips_str, sizeof(mips_str), "PAUSED");
  else if (mips > 0.0f && mips < 0.01f)
    snprintf(mips_str, sizeof(mips_str), "%.1f KIPS", mips * 1000.0f);
  else
    snprintf(mips_str, sizeof(mips_str), "%.2f MIPS", mips);

  // TEL/FTP share the drive-pill row (same size/y). IP sits under them.
  // MIPS stays bottom-right.
  const int mips_reserve = tall ? 110 : 90;   // room for "12.34 MIPS"
  const int ftp_bx = TFT_W - 4 - mips_reserve - pill_w;
  const int tel_bx = ftp_bx - pill_gap - pill_w;

  auto draw_net_pill = [&](int bx, const char* label, uint16_t col) {
    tft.fillRoundRect(bx, pill_y, pill_w, pill_h, 4, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, bx + pill_w / 2, pill_y + pill_h / 2, pill_font);
  };
  const uint16_t COL_NET_OFF    = 0x2945;
  const uint16_t COL_NET_IDLE   = TFT_GREEN;
  const uint16_t COL_NET_ACTIVE = TFT_YELLOW;
  uint16_t tel_col = !telnet_listening() ? COL_NET_OFF
                   : telnet_connected()  ? COL_NET_ACTIVE
                                         : COL_NET_IDLE;
  uint16_t ftp_col = !ftp_listening() ? COL_NET_OFF
                  : ftp_connected()   ? COL_NET_ACTIVE
                                      : COL_NET_IDLE;
  draw_net_pill(tel_bx, "TEL", tel_col);
  draw_net_pill(ftp_bx, "FTP", ftp_col);

  // IP centered under the TEL/FTP pair.
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_WHITE : TFT_RED, TFT_BLACK);
  const int ip_cx = tel_bx + (ftp_bx + pill_w - tel_bx) / 2;
  const int ip_y = pill_y + pill_h + (tall ? 4 : 1);
  tft.drawString(WiFi.status() == WL_CONNECTED
                   ? WiFi.localIP().toString().c_str() : "WiFi down",
                 ip_cx, ip_y, 2);

  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(mips_str, TFT_W - 4, TFT_H - (tall ? 6 : 2), 2);
  tft.setTextDatum(TL_DATUM);

  // [system] title under the drive pills (left side).
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  const int title_y = tall ? (pill_y + pill_h + 6) : (sy + rule_h + 20);
  // On the short Freenove band the title shares the row with drive pills —
  // only draw it when there is vertical room (CrowPanel) or shift right.
  if (tall) {
    tft.drawString(title, 6, title_y, 2);
  } else {
    const int title_x = 6 + pill_count * (pill_w + pill_gap) + 4;
    if (title_x < tel_bx - 8)
      tft.drawString(title, title_x, sy + rule_h + 2, 2);
  }

  gfx_writeback(tft, 0, sy, TFT_W, band);
#endif
}

// Boot (or reboot) the PDP-11 with the currently-mounted drives. cold=true
// re-stamps the bootstrap ROM into high memory and re-zeros guest RAM (used
// by the "Reboot PDP-11" menu item). PDP-11 has no BIOS - boot is just "PC :=
// bootstrap entry"; the ROM is responsible for loading the disk's first
// block and jumping into it.
static void start_cpu(bool cold) {
  // Emulator reset is a transaction across both the guest hardware and the
  // host adapters. Clear deferred commands, file-backed terminal state, and
  // guest-facing console queues before the CPU can produce another byte.
  boot_script_disarm();
  boot_input_disarm();
  emu_control::init();
  dl11_file::disconnect_all();
  dl11_file::reset();
  kl11::reset();
  telnet_reset_guest_io();

  if (cold) pdp_core::cold_boot();
  else      pdp_core::reset();

  // A fresh boot re-reads every disk; clear stale media-change flags so the
  // boot-block reads don't come back as "disk changed".
  for (int s = 0; s < DRIVE_COUNT; s++) disk_take_change(s);

  // m0 stub: PC defaults to 0 from cpu_reset(). m3+ will stamp a bootstrap
  // ROM into high memory and cpu_set_pc() to its entry point here.

  console_init();
  boot_input_arm(cfg);
  boot_script_arm(cfg);
  console_force_redraw();   // render_task repaints the whole console + status bar
}

static bool reload_pdp_config_and_mount(const char* reason) {
  if (!sd_ok) {
    LOGE("%s: SD not available; cannot reload /pdpconfig.ini", reason);
    apply_runtime_pdp_config();
    return false;
  }

  bool pdp_existed = config_load_pdp(cfg);
  LOG("%s: %s /pdpconfig.ini", reason,
      pdp_existed ? "reloaded" : "wrote default");
  config_print(cfg);
  apply_runtime_pdp_config();
  disks_mount();

  // Serial + Telnet banner naming the active config (always /pdpconfig.ini
  // after a variant copy) plus the loaded title/boot/memory summary.
  host_diag_printf(
      "[vpdp1170] emulator reset: config=%s title=\"%s\" boot=%s "
      "mem=%dKW (%s)\r\n",
      PDP_CFG_PATH,
      cfg.title.c_str(),
      cfg.boot_unit_label(),
      cfg.mem_size_kw,
      reason ? reason : "reset");
  return true;
}

// ---- mutex-guarded UI calls (menu state is shared core1 <-> core0) ----
static void ui_open_locked() {
  xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
  ui_open();
  xSemaphoreGive(g_ui_mutex);
}
static void ui_tap_locked(int x, int y) {
  xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
  ui_handle_tap(x, y);
  xSemaphoreGive(g_ui_mutex);
}

static void poll_touch_once();

// ---- core 0: all TFT rendering ----
static void render_task(void* arg) {
  (void)arg;
  bool     was_open  = false;
  uint32_t status_ms = 0;
  for (;;) {
    poll_touch_once();

    bool open = ui_is_open();
    if (was_open && !open) {
      // Menu just closed: clear the strip below the console, full repaint.
      tft.fillRect(0, CON_ROWS * CELL_H, TFT_W, TFT_H - CON_ROWS * CELL_H,
                   TFT_BLACK);
      gfx_writeback(tft, 0, CON_ROWS * CELL_H, TFT_W, TFT_H - CON_ROWS * CELL_H);
      console_force_redraw();
      status_ms = 0;
    }
    was_open = open;

    if (open) {
      xSemaphoreTake(g_ui_mutex, portMAX_DELAY);
      ui_draw(tft);
      xSemaphoreGive(g_ui_mutex);
    } else {
      console_render(tft);
      uint32_t now = millis();
      if (now - status_ms >= 500) { status_ms = now; draw_status_bar(); }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Telnet contains no SD-card operations, so it can run independently on
// core 0. FTP remains on core 1 until the shared storage layer can serialize
// FTP mutations against live PDP-11 disk-image access.
static void net_task(void* arg) {
  (void)arg;
  uint32_t wifi_ms = 0;
  for (;;) {
    telnet_poll();
    eth_nat::host_poll();

    uint32_t now = millis();
    if (now - wifi_ms >= 10000) {
      wifi_ms = now;
      // Only reconnect when fully disconnected. Calling reconnect() while the
      // stack is already in WL_IDLE_STATUS ("sta is connecting") just spams
      // ESP-IDF errors and can delay/abort the in-flight join.
      const wl_status_t st = WiFi.status();
      if (st == WL_DISCONNECTED || st == WL_CONNECTION_LOST ||
          st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
        LOGE("WiFi link down (status=%d) - reconnecting", (int)st);
        WiFi.reconnect();
        eth_nat::set_sta_ip(0);
      } else if (st == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        eth_nat::set_sta_ip(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                            ((uint32_t)ip[2] << 8) | (uint32_t)ip[3]);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  int i;
  delay(200);
  Serial.begin(115200);
  // ESP32-S3 native USB CDC re-enumerates after the post-flash reset; the host
  // serial monitor needs a moment to reconnect. (Elecrow: CDC Disabled so
  // Serial == UART0 on the flash/monitor COM port.)
  for (i = 0; i < 3; i++)
  {
    delay(1000);
    Serial.println(i);
  }  
  Serial.println();
  LOG("%s v%s build %s", APP_TITLE, APP_VERSION, APP_BUILD_DATE);
  LOG("board: %s (VPDP_BOARD=%d  display=%d touch=%d sd=%d  %dx%d console %dx%d @%dx%d)",
      VPDP_BOARD_NAME, VPDP_BOARD, VPDP_DISPLAY_BACKEND, VPDP_TOUCH_BACKEND,
      VPDP_SD_BACKEND, TFT_W, TFT_H, TEXT_COLS, TEXT_ROWS, CELL_W, CELL_H);
  LOG("PDP core: %s", pdp_core::engine_name());

#if VPDP_HAS_WS2812
  strip.begin();
  strip.setBrightness(20);
#endif
  led(32, 0, 0);  // red while booting

#if VPDP_BOARD == VPDP_BOARD_CROWPANEL_7
  // Match CrowPanelBringup order: GT911 power → Wire → STC8H mute+BL → gfx.init.
  // Skipping any of these leaves the panel dark (STC8H BL never arms / RGB FB
  // not scanned). LovyanGFX then owns I2C for GT911 — do not reclaim Wire.
  // Do not call initDMA()/setRotation() after Panel_RGB init (see below).
  pinMode(CROW_GT911_INT_A, OUTPUT);
  pinMode(CROW_GT911_INT_B, OUTPUT);
  pinMode(CROW_GT911_RST, OUTPUT);
  digitalWrite(CROW_GT911_RST, LOW);
  delay(120);
  pinMode(CROW_GT911_RST, INPUT);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  delay(50);
  Wire.beginTransmission(CROW_STC8H_ADDR);
  Wire.write((uint8_t)CROW_BL_MUTE_CMD);
  Wire.endTransmission();
  delay(20);
  Wire.beginTransmission(CROW_STC8H_ADDR);
  Wire.write((uint8_t)CROW_BL_MAX_CMD);
  if (Wire.endTransmission() != 0)
    LOGE("CrowPanel: STC8H backlight cmd failed (I2C 0x%02X)", CROW_STC8H_ADDR);
  else
    LOG("CrowPanel: STC8H backlight ON");
#endif
  tft.init();
#if VPDP_BOARD == VPDP_BOARD_CROWPANEL_7
  // Panel_RGB already runs LCD DMA after init(). Do NOT call initDMA() here —
  // with the SDSPI backend linked it has been observed to leave the PSRAM
  // framebuffer null (StoreProhibited in fillScreen). Orientation is
  // offset_rotation=2 in lgfx_conf.h — do NOT also setRotation().
  LOG("CrowPanel: gfx.init OK  %dx%d  free_psram=%u",
      tft.width(), tft.height(), (unsigned)ESP.getFreePsram());
#else
  tft.setRotation(VPDP_TFT_ROTATION);   // landscape 320x240
#endif
  tft_banner();

  // PSRAM line first
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(ESP.getPsramSize() / 1024));
    tft_status(ROW_PSRAM, "PSRAM: ", buf,
               ESP.getPsramSize() ? TFT_GREEN : TFT_RED);
  }
  tft_status(ROW_SD,   "SD:    ", "(pending)", TFT_DARKGREY);
  tft_status(ROW_CFG,  "Cfg:   ", "(pending)", TFT_DARKGREY);
  {
    char boot_label[16];
    snprintf(boot_label, sizeof(boot_label), "Boot %s:", boot_unit_label());
    tft_status(ROW_BOOT, boot_label, "(pending)", TFT_DARKGREY);
  }
  tft_status(ROW_WIFI, "WiFi:  ", "(pending)", TFT_DARKGREY);
  tft_status(ROW_IP,   "IP:    ", "(none)",    TFT_DARKGREY);
  tft_status(ROW_CORE, "Core:  ", pdp_core::engine_name(),
             pdp_core::is_kek_engine() ? TFT_YELLOW : TFT_GREEN);
  tft_status(ROW_CPU,  "CPU:   ", "(pending)", TFT_DARKGREY);

  sd_and_config_init();
  tft_banner_title();        // refresh banner with cfg.title from pdpconfig.ini
  pdp_core::set_target_memory_kw((uint32_t)cfg.mem_size_kw);
  LOG("PDP core memory: configured=%d KW target=%u bytes active=%u bytes",
      cfg.mem_size_kw,
      (unsigned)pdp_core::target_memory_bytes(),
      (unsigned)pdp_core::memory_size());

  // Allocate the selected PDP core's guest memory after /pdpconfig.ini has
  // provided [system] mem_size_kw.
  tft_status(ROW_CPU, "CPU:   ", "init...", TFT_YELLOW);
  bool cpu_ok = pdp_core::init();
  LOG("PDP core initialized: %s (active memory=%u KW, target memory=%u KW)",
      pdp_core::engine_name(),
      (unsigned)(pdp_core::memory_size() / 2048),
      (unsigned)(pdp_core::target_memory_bytes() / 2048));
  if (!cpu_ok && pdp_core::is_kek_engine())
    LOGE("PDP core selected but not wired yet: %s", pdp_core::engine_name());

  // Acceptance test: prove the selected PDP core executes PDP-11 instructions.
  // Writes MOV/MOV/ADD/BR and asserts R0/R1 state. The scaffold path later
  // re-runs reset/start_cpu(), while the kek path stops here until devices
  // are wired.
  bool selftest_ok = false;
  if (cpu_ok) {
    selftest_ok = pdp_core::selftest();
    tft_status(ROW_CPU, "CPU:   ",
               selftest_ok ? "selftest PASS" : "selftest FAIL",
               selftest_ok ? TFT_GREEN : TFT_RED);
  }


#if VPDP1170_STARTUP_BENCHMARK
  if (cpu_ok && selftest_ok) {
    tft_status(ROW_CPU, "CPU:   ", "benchmark...", TFT_YELLOW);
    bool benchmark_ok = pdp_core::benchmark();
    LOG("PDP core benchmark: %s", benchmark_ok ? "PASS" : "FAIL");
    tft_status(ROW_CPU, "CPU:   ",
               benchmark_ok ? "benchmark PASS" : "benchmark FAIL",
               benchmark_ok ? TFT_GREEN : TFT_RED);
    // Benchmark samples toggle the panic-trace ring; re-apply config so
    // diag_trace=true survives into the guest boot.
    apply_runtime_pdp_config();
  }
#endif

  wifi_connect();

  // ---- boot/services: mount drives and start the selected PDP core.
  if (cpu_ok && (!pdp_core::is_kek_engine() || selftest_ok)) {
    telnet_begin(cfg.telnet_port, cfg.telnet_enabled);
    ftp_begin(cfg.ftp_port, cfg.ftp_enabled,
              cfg.ftp_user.c_str(), cfg.ftp_password.c_str());
    pinMode(BUTTON_PIN, INPUT_PULLUP);   // onboard button opens the menu
    touch_init(&tft);
    ui_init();

    if (pdp_core::is_kek_engine()) {
      disks_mount();
      pdp_core::set_boot_kind(cfg.core_boot_kind());
      const char* boot_lbl = boot_unit_label();
      LOG("--- booting kek PDP-11/70 from %s, console -> TFT ---", boot_lbl);
      bool boot_mounted =
          (cfg.boot_kind == AppConfig::BK_RK) ? disk_is_mounted(DRIVE_RK0) :
          (cfg.boot_kind == AppConfig::BK_RP) ? disk_is_mounted(DRIVE_RP0) :
          (cfg.boot_kind == AppConfig::BK_DU) ? disk_is_mounted(DRIVE_DU0) :
                                               disk_is_mounted(DRIVE_A);
      char boot_row[16];
      snprintf(boot_row, sizeof(boot_row), "Boot %s:", boot_lbl);
      tft_status(ROW_BOOT, boot_row,
                 boot_mounted ? boot_image_path().c_str() : "not mounted",
                 boot_mounted ? TFT_GREEN : TFT_RED);
      char cpu_row[24];
      snprintf(cpu_row, sizeof(cpu_row), "kek %s boot", boot_lbl);
      tft_status(ROW_CPU,  "CPU:   ", cpu_row, TFT_GREEN);
      start_cpu(true);
      led(0, 0, 32);           // blue = PDP-11 boot stub running
    } else {
      disks_mount();
      const char* boot_name = boot_unit_label();
      pdp_core::set_boot_kind(cfg.core_boot_kind());
      LOG("--- booting PDP-11 from %s, console -> TFT ---", boot_name);
      start_cpu(false);
      led(0, 0, 32);          // blue = PDP-11 booting
    }

    // Spin up core-0 display, Telnet, and output-consumer tasks. KEK writes
    // directly to three independent SPSC FIFOs; no sink can block core 1.
    g_ui_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(render_task, "render", 10240, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(net_task,    "net",     8192, NULL, 2, NULL, 0);
    bool tft_output_ok = console_start_output_task();
    bool serial_output_ok = kl11::start_serial_output_task();
    if (!tft_output_ok || !serial_output_ok) {
      LOGE("console sink task creation failed; PDP-11 start cancelled");
      led(32, 0, 0);
      return;
    }
    cpu_running = true;
  } else {
    tft_status(ROW_CPU, "CPU:   ",
               pdp_core::is_kek_engine() ? "kek not wired" : "alloc FAILED",
               TFT_RED);
    led(32, 0, 0);
  }
}

// loop() runs on core 1 and IS the PDP-11: CPU emulation plus FTP and
// settings-menu command handling. It never touches the TFT; render_task owns
// the display, while dedicated core-0 tasks consume TFT, Telnet and USB output.
// Touch handling lives at file scope so render_task can share the same
// double-tap state with menu hit testing. The 750 ms window is wider than the
// original 450 ms because users were missing the second tap of a fast
// double-tap when the timer rolled over. UI_DOUBLE_TAP_MIN_MS rejects a
// bounce that still slips past touch.cpp debounce as two near-instant edges.
static uint32_t g_last_tap_ms = 0;
static int      g_last_tap_x  = 0;
static int      g_last_tap_y  = 0;
#define UI_DOUBLE_TAP_MS     750
#define UI_DOUBLE_TAP_MIN_MS 120
#define UI_DOUBLE_TAP_MAX_DIST_SQ (80 * 80)

// Poll the touchscreen once. When the menu is open, route the tap into
// the menu; when closed, accumulate it as a double-tap candidate that
// opens the menu when two taps land within UI_DOUBLE_TAP_MS of each
// other. Called from render_task every ~20 ms; touch edge events do not
// survive long CPU slices on core 1.
static void poll_touch_once() {
  int tx, ty;
  if (!touch_poll(&tx, &ty)) return;
  if (ui_is_open()) {
    ui_tap_locked(tx, ty);
    return;
  }
  uint32_t now = millis();
  const uint32_t dt = (uint32_t)(now - g_last_tap_ms);
  if (g_last_tap_ms != 0 &&
      dt >= UI_DOUBLE_TAP_MIN_MS &&
      dt < UI_DOUBLE_TAP_MS) {
    const int dx = tx - g_last_tap_x;
    const int dy = ty - g_last_tap_y;
    if (dx * dx + dy * dy <= UI_DOUBLE_TAP_MAX_DIST_SQ) {
      ui_open_locked();
      g_last_tap_ms = 0;
      return;
    }
  }
  g_last_tap_ms = now;
  g_last_tap_x  = tx;
  g_last_tap_y  = ty;
}

static void poll_pcping() {
  // Periodic snapshot of guest CPU state - useful while bringing up
  // disk/OS bootstrap. If PC stays put, the guest is stuck in a tight
  // loop; if PC moves through a small window, it's a finite poll loop.
  // Rate is [diag] pcping in pdpconfig.ini (seconds). 0 disables it.
  static uint32_t s_state_ms = 0;
  if (cfg.diag_pcping_sec <= 0) return;

  const uint32_t interval_ms = (uint32_t)cfg.diag_pcping_sec * 1000U;
  uint32_t s_now = millis();
  if (s_now - s_state_ms < interval_ms) return;

  s_state_ms = s_now;
  uint16_t next_pc = 0;
  uint16_t next_opcode = 0;
  char next_disasm[96] = {0};
  bool has_next = pdp_core::next_instruction(&next_pc, &next_opcode);
  if (!pdp_core::disassemble_next(next_disasm, sizeof(next_disasm))) {
    next_disasm[0] = 0;
  }

  LOG("state: PC=0%o ins=0%o %s R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o PS=0%o inst=%u",
      (unsigned)pdp_core::pc(),
      has_next ? (unsigned)next_opcode : 0,
      next_disasm,
      (unsigned)pdp_core::reg16(0), (unsigned)pdp_core::reg16(1),
      (unsigned)pdp_core::reg16(2), (unsigned)pdp_core::reg16(3),
      (unsigned)pdp_core::reg16(4), (unsigned)pdp_core::reg16(5),
      (unsigned)pdp_core::reg16(6),
      (unsigned)pdp_core::psw(),
      (unsigned)pdp_core::instruction_count());
  // cpu_dump_trace() is available if you need it for stuck-in-loop
  // diagnosis - the cpu_pdp11.h function dumps the last N entries of
  // the trace ring. We leave it off by default so the serial console
  // stays usable for the guest OS.
}

void loop() {
  if (!cpu_running) { delay(100); return; }

  static bool     boot_done = false;
  static bool     btn_stable = true;
  static uint32_t btn_change_ms = 0;

  // Onboard button (GPIO0, active low): press opens the menu.
  // Debounce: ignore chatter shorter than ~40 ms (BOOT pin is noisy with USB).
  bool btn_raw = digitalRead(BUTTON_PIN);
  uint32_t btn_now_ms = millis();
  if (btn_raw != btn_stable) {
    if (btn_change_ms == 0) btn_change_ms = btn_now_ms;
    if ((uint32_t)(btn_now_ms - btn_change_ms) >= 40) {
      if (btn_stable && !btn_raw && !ui_is_open())
        ui_open_locked();
      btn_stable = btn_raw;
      btn_change_ms = 0;
    }
  } else {
    btn_change_ms = 0;
  }

  // Execute deferred VPDP control commands outside the CPU instruction path.
  // This is where SD file operations, runtime media changes, and TT1 file I/O
  // are allowed to block briefly without stalling an emulated UART register.
  emu_control::poll();
  telnet_shell_poll();
  boot_script_poll();
  boot_input_poll();
  if (emu_control::consume_reboot_request()) {
    LOG("EMU command: reboot PDP-11 from /pdpconfig.ini");
    dl11_file::disconnect_all();
    if (reload_pdp_config_and_mount("EMU reboot")) {
      pdp_core::set_boot_kind(cfg.core_boot_kind());
      start_cpu(true);
      boot_done = false;
      led(0, 0, 32);
    } else {
      LOGE("EMU reboot cancelled: /pdpconfig.ini drives could not be mounted");
    }
  }

  // Boot-source or boot-media changed from the menu; remount and cold boot.
  if (ui_consume_boot_change()) {
    const char* boot_name = boot_unit_label();
    LOG("ui: boot from %s", boot_name);
    disks_mount();
    pdp_core::set_boot_kind(cfg.core_boot_kind());
    start_cpu(true);
    boot_done = false;
    led(0, 0, 32);
  }

  // "Reboot PDP-11" from the menu (the menu has already closed itself).
  if (ui_consume_reboot()) {
    LOG("ui: reboot PDP-11 from /pdpconfig.ini");
    dl11_file::disconnect_all();
    if (reload_pdp_config_and_mount("ui reboot")) {
      pdp_core::set_boot_kind(cfg.core_boot_kind());
      start_cpu(true);
      boot_done = false;
      led(0, 0, 32);
    } else {
      LOGE("ui: reboot cancelled: /pdpconfig.ini drives could not be mounted");
    }
  }

  // "Reset ESP32" from the menu. NO serial activity on this path - if the
  // host isn't reading USB-CDC (Arduino IDE Serial Monitor closed, no PC
  // attached, etc.), Serial.write / Serial.printf / our kl11 drain all
  // block on the USB-CDC TX semaphore (default ~5 s timeout, sometimes
  // hangs indefinitely). The user already saw the on-screen confirmation,
  // so we just reset immediately and let any in-flight serial bytes drop.
  if (ui_consume_esp_restart()) {
    ESP.restart();   // does not return
  }

  // While the menu is open the PDP-11 is paused, but FTP remains available.
  if (ui_is_open()) {
    emu_control::poll();
    telnet_shell_poll();
    boot_script_poll();
    boot_input_poll();
    ftp_poll();
    delay(8);
    return;
  }

  // Running: feed the keyboard, run the PDP-11 in small slices, and service
  // FTP between slices. Output sinks drain independently on core 0.
  for (int slice = 0; slice < 5; slice++) {
    while (Serial.available())
      console_key_push((uint8_t)Serial.read());   // -> Serial-in FIFO
    ftp_poll();                  // accept + FTP commands/data against SD root
    emu_control::poll();
    telnet_shell_poll();
    boot_script_poll();
    boot_input_poll();
    poll_pcping();
    pdp_core::run(1000);
    poll_pcping();
  }
  ftp_poll();
  emu_control::poll();
  telnet_shell_poll();
  boot_script_poll();
  boot_input_poll();

  poll_pcping();

  // Status LED: blue while booting, green once the PDP-11 has gone quiet at a prompt.
  if (!boot_done && console_feed_count() > 0 &&
      millis() - console_last_feed_ms() > 800) {
    boot_done = true;
    led(0, 32, 0);
  }
}
