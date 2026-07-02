//-------------------------------------------------------------------------------
// vpdp1170 - DEC PDP-11/70 emulator on Freenove ESP32-S3 2.8" Display
//
// Cloned from vpdp1140 on 2026-07-01 as the host scaffold for a future
// PDP-11/70 emulator. The current baseline still uses the vpdp1140/sam11
// 11/40-derived core while the 11/70 CPU, 22-bit MMU, bus, and devices are
// being replaced with a PDP-11/70-capable engine.
//
// Initial source candidate for the 11/70 engine is Folkert van Heusden's
// kek emulator (MIT), kept under _upstream_kek for reference/import work.
//
// The ESP32-S3 host scaffolding (TFT console, telnet, USB serial, SD images,
// touch settings menu, wificonfig.ini + pdpconfig.ini, dual-core split)
// carries over from v8088 unchanged; only the CPU core, I/O page dispatch,
// and disk/console wiring are PDP-11-specific.
//
// Requires the TFT_eSPI library to have FNK0104B selected in
// User_Setup_Select.h (this is the same setup used by all Freenove
// tutorials for this board).
// Board: Freenove ESP32-S3 with 2.8" TFT and capacitive touch screen
//  (WROVER2 w/8Mb PSRAM, 16Mb flash)
// Board: ESP32S3 Dev Module
// Partitioning: H16mb Flash (3mb program/9.9 spiffs)
// Need to go to tools : USB CDC on boot - enable, enable OPI PSRAM, set flash to 16MB
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

//------------------------------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include "Freenove_WS2812_Lib_for_ESP32.h"

#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "appconfig.h"
#include "pdp_core.h"
#include "kl11.h"        // kl11::drain_serial_out() in loop()
#include "dd11.h"  // dd11::v4b_quirks_enabled gate
#include "kw11.h"
#include "kwp.h"   // kwp::enabled gate
#include "disk.h"
#include "rl11.h"
#include "console.h"
#include "telnet.h"
#include "telnet_shell.h"
#include "ftp.h"
#include "touch.h"
#include "ui.h"
#include "dl11_file.h"
#include "emu_control.h"

static TFT_eSPI tft;
static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);
AppConfig cfg;             // non-static so ui.cpp (System Info screen,
                           // title display) can read it via the extern in
                           // appconfig.h. Only vpdp1170.ino writes it.
static bool sd_ok = false;
static bool cpu_running = false;   // true once the PDP-11 is booting in loop()

// The PDP-11 runs on core 1 (loop); all TFT rendering runs on core 0
// (render_task). The settings menu is the only shared mutable UI state -
// this mutex guards it. The 80x25 console grid is updated lock-free; a
// torn read just produces one self-correcting frame.
static SemaphoreHandle_t g_ui_mutex = nullptr;

enum BootState { BOOT_RUNNING, BOOT_OK, BOOT_FAIL };
static BootState boot_state = BOOT_RUNNING;

static void led(uint8_t r, uint8_t g, uint8_t b) {
  strip.setLedColorData(0, r, g, b);
  strip.show();
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
}

static void tft_banner() {
  tft.fillScreen(TFT_BLACK);
  tft_banner_title();
  tft.setCursor(4, 22);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("build %s", APP_BUILD_DATE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void tft_status(int row, const char* label, const char* value, uint16_t color) {
  int y = 50 + row * 18;
  tft.fillRect(0, y, TFT_W, 18, TFT_BLACK);
  tft.setCursor(4, y);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.print(label);
  tft.setTextColor(color, TFT_BLACK);
  tft.print(value);
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

// Boot drive unit label (e.g. "DL0", "DL1", "DL2", "DL3", "RK0") and the
// configured image path for the slot named by cfg.boot_drive ('a'..'d').
static const char* boot_unit_label() {
  int slot = (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
               ? (cfg.boot_drive - 'a') : 0;
  static const char* rl_names[4] = { "DL0", "DL1", "DL2", "DL3" };
  if (slot == 0 && cfg.boot_kind == AppConfig::BK_RK) return "RK0";
  return rl_names[slot];
}
static const String& boot_image_path() {
  int slot = (cfg.boot_drive >= 'a' && cfg.boot_drive <= 'd')
               ? (cfg.boot_drive - 'a') : 0;
  if (slot == 0 && cfg.boot_kind == AppConfig::BK_RK) return cfg.disk_rk0;
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
    boot_state = BOOT_OK;
  } else {
    LOGE("WiFi connect timed out");
    tft_status(ROW_WIFI, "WiFi:  ", "FAILED", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
  }
}

static void sd_and_config_init() {
  tft_status(ROW_SD,  "SD:    ", "mounting...", TFT_YELLOW);
  if (sd_mount()) {
    char info[32];
    uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
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

  // Push the V4B-quirks flag down to dd11 so its probe-absorb ranges
  // honor what pdpconfig.ini said. Must happen before cpu_reset() / any
  // guest memory access.
  dd11::v4b_quirks_enabled = cfg.v4b_quirks;
  dd11::set_io_trace((uint32_t)(cfg.diag_io_trace < 0
                                  ? 0 : cfg.diag_io_trace));
  kw11::set_clock_trace((uint32_t)(cfg.diag_clock_trace < 0
                                     ? 0 : cfg.diag_clock_trace));
  kl11::set_console_trace((uint32_t)(cfg.diag_console_trace < 0
                                      ? 0 : cfg.diag_console_trace));
  kwp::enabled             = cfg.kwp_enabled;
  pdp_core::set_trace(cfg.diag_trace);
  kl11::serial_in_delay_ms = (uint32_t)(cfg.diag_serialdelay_ms < 0 ? 0
                                      : cfg.diag_serialdelay_ms);

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
    tft_status(ROW_BOOT, boot_label,
               SD_MMC.exists(bpath) ? bpath.c_str() : "MISSING",
               SD_MMC.exists(bpath) ? TFT_GREEN : TFT_RED);
  }
}

// Mount the four guest drives from /pdpconfig.ini paths.
// When boot=rk0 we substitute disk_rk0 into slot 0 (so the RK11 controller
// sees the RK05 image as drive 0) and the corresponding unit name flips
// from "DL0" to "RK0".
static void disks_mount() {
  if (!sd_ok) { LOGE("disks_mount: SD not available"); return; }
  for (int s = 0; s < DRIVE_COUNT; s++)
    disk_dismount(s);

  const bool rk_boot = (cfg.boot_kind == AppConfig::BK_RK);
  const String* paths[4] = {
    rk_boot ? &cfg.disk_rk0 : &cfg.disk_a,
    &cfg.disk_b, &cfg.disk_c, &cfg.disk_d
  };
  const char* unit_names[4] = {
    rk_boot ? "RK0" : "DL0", "DL1", "DL2", "DL3"
  };
  for (int s = 0; s < 4; s++) {
    if (paths[s]->length() == 0) continue;
    bool ok = disk_mount(s, paths[s]->c_str());
    bool is_rl_slot = !(rk_boot && s == DRIVE_A);
    if (ok && is_rl_slot && !rl11::validate_mounted_media(s)) {
      uint32_t bytes = disk_size_bytes(s);
      LOGE("disks_mount %s: \"%s\" rejected, RL image size is %u bytes; expected RL01=%u or RL02=%u",
           unit_names[s], paths[s]->c_str(), (unsigned)bytes,
           (unsigned)rl11::RL01_IMAGE_BYTES,
           (unsigned)rl11::RL02_IMAGE_BYTES);
      disk_dismount(s);
      ok = false;
    }
    LOG("disks_mount %s: \"%s\" -> %s",
        unit_names[s], paths[s]->c_str(), ok ? "mounted" : "FAILED");
  }
  if (cfg.disk_rp0.length()) {
    bool ok = disk_mount(DRIVE_RP0, cfg.disk_rp0.c_str());
    LOG("disks_mount RP0 (%s): \"%s\" -> %s",
        cfg.disk_rp0_type.c_str(), cfg.disk_rp0.c_str(),
        ok ? "mounted" : "FAILED");
  }
}

// Status bar drawn in the 40 px strip below the 80x25 console: drive activity
// indicators, IP address, telnet state and emulation speed.
static void draw_status_bar() {
  static uint32_t prev_io[DRIVE_COUNT] = {0};
  static uint32_t prev_inst = 0;
  static uint32_t prev_ms   = 0;
  const int sy = CON_ROWS * CELL_H;          // 200

  tft.drawFastHLine(0, sy, TFT_W, TFT_DARKGREY);

  // Drive indicators DL0..DL3 (or RK0/DL1/DL2/DL3 when booting RK05) -
  // green=mounted, yellow=active, dim=empty.
  const char* unit_labels[4] = {
    (cfg.boot_kind == AppConfig::BK_RK) ? "RK0" : "DL0",
    "DL1", "DL2", "DL3"
  };
  for (int s = 0; s < 4; s++) {
    uint32_t r = 0, w = 0;
    disk_stats(s, &r, &w);
    bool active = (r + w) != prev_io[s];
    prev_io[s] = r + w;
    uint16_t col = !disk_is_mounted(s) ? 0x2945
                 : active             ? TFT_YELLOW
                                      : TFT_GREEN;
    int bx = 6 + s * 36;
    tft.fillRoundRect(bx, sy + 5, 32, 16, 2, col);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(unit_labels[s], bx + 16, sy + 13, 1);
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

  tft.fillRect(156, sy + 1, TFT_W - 156, TFT_H - sy - 1, TFT_BLACK);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_WHITE : TFT_RED, TFT_BLACK);
  tft.drawString(WiFi.status() == WL_CONNECTED
                   ? WiFi.localIP().toString().c_str() : "WiFi down",
                 158, sy + 6, 1);
  // TEL/FTP pills match vApple2: dim when unavailable, green when listening,
  // yellow when a client is connected.
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

  // MIPS gets right-aligned to the screen edge via TR_DATUM so it's
  // always at the rightmost column regardless of how many digits.
  char mips_str[16];
  snprintf(mips_str, sizeof(mips_str), "%.2f MIPS", mips);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(mips_str, TFT_W - 4, sy + 22, 1);
  tft.setTextDatum(TL_DATUM);   // restore for the title row below

  // [system] title from pdpconfig.ini, drawn below the drive indicators
  // (left half, the empty strip under the DL0..DL3 boxes). Falls
  // back to APP_TITLE if the user left the field blank.
  tft.fillRect(0, sy + 22, 156, TFT_H - sy - 22, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  tft.drawString(title, 6, sy + 24, 2);
}

// Boot (or reboot) the PDP-11 with the currently-mounted drives. cold=true
// re-stamps the bootstrap ROM into high memory and re-zeros guest RAM (used
// by the "Reboot PDP-11" menu item). PDP-11 has no BIOS - boot is just "PC :=
// bootstrap entry"; the ROM is responsible for loading the disk's first
// block and jumping into it.
static void start_cpu(bool cold) {
  if (cold) pdp_core::cold_boot();
  else      pdp_core::reset();

  // A fresh boot re-reads every disk; clear stale media-change flags so the
  // boot-block reads don't come back as "disk changed".
  for (int s = 0; s < DRIVE_COUNT; s++) disk_take_change(s);

  // m0 stub: PC defaults to 0 from cpu_reset(). m3+ will stamp a bootstrap
  // ROM into high memory and cpu_set_pc() to its entry point here.

  console_init();
  for (size_t i = 0; i < cfg.boot_input_len; i++)
    console_key_push(cfg.boot_input[i]);
  if (cfg.boot_input_len)
    LOG("console: injected %u boot input bytes", (unsigned)cfg.boot_input_len);
  console_force_redraw();   // render_task repaints the whole console + status bar
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

// ---- core 0: all TFT rendering ----
static void render_task(void* arg) {
  (void)arg;
  bool     was_open  = false;
  uint32_t status_ms = 0;
  for (;;) {
    bool open = ui_is_open();
    if (was_open && !open) {
      // Menu just closed: clear the strip below the console, full repaint.
      tft.fillRect(0, CON_ROWS * CELL_H, TFT_W, TFT_H - CON_ROWS * CELL_H,
                   TFT_BLACK);
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

    uint32_t now = millis();
    if (now - wifi_ms >= 10000) {
      wifi_ms = now;
      if (WiFi.status() != WL_CONNECTED) {
        LOGE("WiFi link down - reconnecting");
        WiFi.reconnect();
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
  // serial monitor needs a moment to reconnect. 
  for (i = 0; i < 3; i++)
  {
    delay(1000);
    Serial.println(i);
  }  
  Serial.println();
  LOG("%s v%s build %s", APP_TITLE, APP_VERSION, APP_BUILD_DATE);
  LOG("PDP core selected: %s (VPDP1170_USE_KEK_CORE=%d, VPDP1170_BUILD_KEK_ADAPTER=%d)",
      pdp_core::engine_name(), VPDP1170_USE_KEK_CORE, VPDP1170_BUILD_KEK_ADAPTER);
  LOG("PDP core memory: active=%u bytes, target=%u bytes",
      (unsigned)pdp_core::memory_size(),
      (unsigned)pdp_core::target_memory_bytes());
  if (!pdp_core::is_kek_engine())
    LOG("kek core disabled; running inherited 11/40 scaffold");

  strip.begin();
  strip.setBrightness(20);
  led(32, 0, 0);  // red while booting

  tft.init();
  tft.setRotation(1);     // landscape 320x240
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

  // Allocate the selected PDP core's guest memory before WiFi/SD take their share.
  tft_status(ROW_CPU, "CPU:   ", "init...", TFT_YELLOW);
  bool cpu_ok = pdp_core::init();
  LOG("PDP core initialized: %s (active memory=%u bytes, target memory=%u bytes)",
      pdp_core::engine_name(),
      (unsigned)pdp_core::memory_size(),
      (unsigned)pdp_core::target_memory_bytes());
  if (!cpu_ok && pdp_core::is_kek_engine())
    LOGE("PDP core selected but not wired yet: %s", pdp_core::engine_name());

  // Acceptance test: prove the selected PDP core executes PDP-11 instructions.
  // Writes MOV/MOV/ADD/BR and asserts R0/R1 state. The scaffold path later
  // re-runs reset/start_cpu(), while the kek path stops here until devices
  // are wired.
  bool selftest_ok = false;
  if (cpu_ok) {
    selftest_ok = pdp_core::selftest();
    LOG("PDP core selftest: %s", selftest_ok ? "PASS" : "FAIL");
    tft_status(ROW_CPU, "CPU:   ",
               selftest_ok ? "selftest PASS" : "selftest FAIL",
               selftest_ok ? TFT_GREEN : TFT_RED);
  }

  sd_and_config_init();
  tft_banner_title();        // refresh banner with cfg.title from pdpconfig.ini

  wifi_connect();

  // ---- boot/services: mount drives for the 11/40 scaffold, or stop at the
  // kek CPU/MMU adapter milestone until PDP-visible devices are wired.
  if (cpu_ok && (!pdp_core::is_kek_engine() || selftest_ok)) {
    telnet_begin(cfg.telnet_port, cfg.telnet_enabled);
    ftp_begin(cfg.ftp_port, cfg.ftp_enabled,
              cfg.ftp_user.c_str(), cfg.ftp_password.c_str());
    pinMode(BUTTON_PIN, INPUT_PULLUP);   // onboard button opens the menu
    touch_init();
    ui_init();

    if (pdp_core::is_kek_engine()) {
      disks_mount();
      pdp_core::set_boot_kind(cfg.boot_kind == AppConfig::BK_RK ? 1 : 0);
      if (cfg.boot_kind == AppConfig::BK_RK) {
        LOG("--- booting kek PDP-11/70 from RK0, console -> TFT ---");
        bool rk0_mounted = disk_is_mounted(DRIVE_A);
        tft_status(ROW_BOOT, "Boot RK0:",
                   rk0_mounted ? boot_image_path().c_str() : "not mounted",
                   rk0_mounted ? TFT_GREEN : TFT_RED);
        tft_status(ROW_CPU,  "CPU:   ", "kek RK0 boot", TFT_GREEN);
      } else {
        LOG("--- kek PDP-11/70 CPU/MMU adapter online; only RK0 boot is wired so far ---");
        tft_status(ROW_BOOT, "Boot:  ", "kek RK0 only", TFT_YELLOW);
        tft_status(ROW_CPU,  "CPU:   ", "kek smoke ROM", TFT_GREEN);
      }
      start_cpu(true);
      led(0, 0, 32);           // blue = PDP-11 boot stub running
    } else {
      disks_mount();
      const char* boot_name;
      if (cfg.boot_kind == AppConfig::BK_RK) boot_name = "RK0";
      else boot_name = (cfg.boot_drive == 'a') ? "DL0"
                     : (cfg.boot_drive == 'b') ? "DL1"
                     : (cfg.boot_drive == 'c') ? "DL2"
                     : (cfg.boot_drive == 'd') ? "DL3" : "?";
      pdp_core::set_boot_kind(cfg.boot_kind == AppConfig::BK_RK ? 1 : 0);
      LOG("--- booting PDP-11 from %s, console -> TFT ---", boot_name);
      start_cpu(false);
      led(0, 0, 32);          // blue = PDP-11 booting
    }
    cpu_running = true;

    // Spin up core-0 display and Telnet tasks. The PDP-11 and all SD-card
    // activity remain on core 1.
    g_ui_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(render_task, "render", 10240, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(net_task,    "net",     8192, NULL, 2, NULL, 0);
  } else {
    tft_status(ROW_CPU, "CPU:   ",
               pdp_core::is_kek_engine() ? "kek not wired" : "alloc FAILED",
               TFT_RED);
    led(32, 0, 0);
  }
}

// loop() runs on core 1 and IS the PDP-11: CPU emulation plus FTP, touch and
// settings-menu logic. It never touches the TFT; render_task owns the display
// and net_task owns Telnet on core 0.
// Touch handling lives at file scope so cpu_run's per-slice work and the
// menu-open early-return path can share the same state. The 750 ms
// window is wider than the original 450 ms because users were missing
// the second tap of a fast double-tap when the timer rolled over.
static uint32_t g_last_tap_ms = 0;
#define UI_DOUBLE_TAP_MS 750

// Poll the touchscreen once. When the menu is open, route the tap into
// the menu; when closed, accumulate it as a double-tap candidate that
// opens the menu when two taps land within UI_DOUBLE_TAP_MS of each
// other. Called from loop() once per slice so a fast double-tap can't
// fall between two touch_poll calls (cpu_run runs ~40 ms total, and
// the FT6336U doesn't buffer events for us).
static void poll_touch_once() {
  int tx, ty;
  if (!touch_poll(&tx, &ty)) return;
  if (ui_is_open()) {
    ui_tap_locked(tx, ty);
    return;
  }
  uint32_t now = millis();
  if ((uint32_t)(now - g_last_tap_ms) < UI_DOUBLE_TAP_MS) {
    ui_open_locked();
    g_last_tap_ms = 0;
  } else {
    g_last_tap_ms = now;
  }
}

void loop() {
  if (!cpu_running) { delay(100); return; }

  static bool     boot_done = false;
  static bool     btn_prev  = true;

  poll_touch_once();

  // Onboard button (GPIO0, active low): press opens the menu.
  bool btn_now = digitalRead(BUTTON_PIN);
  if (btn_prev && !btn_now && !ui_is_open()) ui_open_locked();
  btn_prev = btn_now;

  // Execute deferred VPDP control commands outside the CPU instruction path.
  // This is where SD file operations, runtime media changes, and TT1 file I/O
  // are allowed to block briefly without stalling an emulated UART register.
  emu_control::poll();
  telnet_shell_poll();
  if (emu_control::consume_reboot_request()) {
    if (pdp_core::is_kek_engine()) {
      LOGE("EMU reboot ignored: kek disk/boot path is not wired yet");
    } else {
      LOG("EMU command: reboot PDP-11");
      dl11_file::disconnect_all();
      if (disk_reopen_all()) {
        start_cpu(true);
        boot_done = false;
        led(0, 0, 32);
      } else {
        LOGE("EMU reboot cancelled: one or more disk images could not be reopened");
      }
    }
  }

  // Boot-source or boot-media changed from the menu; remount and cold boot.
  if (ui_consume_boot_change()) {
    if (pdp_core::is_kek_engine()) {
      LOGE("ui: boot change noted, but kek disk/boot path is not wired yet");
    } else {
      const char* boot_name = (cfg.boot_kind == AppConfig::BK_RK) ? "RK0" : "DL0";
      LOG("ui: boot from %s", boot_name);
      disks_mount();
      pdp_core::set_boot_kind(cfg.boot_kind == AppConfig::BK_RK ? 1 : 0);
      start_cpu(true);
      boot_done = false;
      led(0, 0, 32);
    }
  }

  // "Reboot PDP-11" from the menu (the menu has already closed itself).
  if (ui_consume_reboot()) {
    if (pdp_core::is_kek_engine()) {
      LOGE("ui: reboot ignored: kek disk/boot path is not wired yet");
    } else {
      LOG("ui: reboot PDP-11");
      if (disk_reopen_all()) {
        start_cpu(true);
        boot_done = false;
        led(0, 0, 32);
      } else {
        LOGE("ui: reboot cancelled: one or more disk images could not be reopened");
      }
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
    ftp_poll();
    delay(8);
    return;
  }

  // Running: feed the keyboard, run the PDP-11 in small slices, service FTP
  // between slices, and
  // drain the KL11->host output FIFOs (USB-Serial + TFT) so the 8 KB
  // rings stay near empty during steady-state output.
  for (int slice = 0; slice < 5; slice++) {
    // Per-slice touch poll: cpu_run(8000) takes ~8 ms, so polling here
    // catches the second tap of a fast double-tap that would otherwise
    // fall between two loop iterations (~40 ms gap). Cheap - touch_poll
    // is a single I2C transaction to the FT6336U.
    poll_touch_once();
    while (Serial.available())
      console_key_push((uint8_t)Serial.read());   // -> Serial-in FIFO
    ftp_poll();                  // accept + FTP commands/data against SD root
    emu_control::poll();
    telnet_shell_poll();
    pdp_core::run(8000);
    console_drain_tft();         // TFT-out FIFO -> ANSI parser -> cell grid
    if (!pdp_core::is_kek_engine()) {
      kl11::drain_serial_out();  // Serial-out FIFO -> Serial.write
    }
  }
  ftp_poll();
  emu_control::poll();
  telnet_shell_poll();
  console_drain_tft();
  if (!pdp_core::is_kek_engine()) {
    kl11::drain_serial_out();
  }


  // Periodic snapshot of guest CPU state - useful while bringing up
  // disk/OS bootstrap. If PC stays put, the guest is stuck in a tight
  // loop; if PC moves through a small window, it's a finite poll loop.
  // Rate is [diag] pcping in pdpconfig.ini (seconds). 0 disables it.
  static uint32_t s_state_ms = 0;
  uint32_t s_now = millis();
  if (cfg.diag_pcping_sec > 0 &&
      s_now - s_state_ms >= (uint32_t)cfg.diag_pcping_sec * 1000U) {
    s_state_ms = s_now;
    LOG("state: PC=0%o R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o PS=0%o inst=%u",
        (unsigned)pdp_core::pc(),
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

  // Status LED: blue while booting, green once the PDP-11 has gone quiet at a prompt.
  if (!boot_done && console_feed_count() > 0 &&
      millis() - console_last_feed_ms() > 800) {
    boot_done = true;
    led(0, 32, 0);
  }
}
