#include "ui.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"
#include "config.h"
#include "platform.h"
#include "disk.h"
#include "rl11.h"
#include "rh11.h"
#include "pdp_core.h"
#include "telnet.h"
#include "ftp.h"
#include "appconfig.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <string.h>

#ifndef TFT_BL
#define TFT_BL 45
#endif

// ---- colours ----
#define COL_BG       0x0009
#define COL_TITLE    0x001F
#define COL_ITEM     0x4208
#define COL_ITEM_HI  0x0320     // mounted-drive green
#define COL_ACCENT   0x07E0
#define COL_DANGER   0xC000
#define COL_TEXT     TFT_WHITE
#define COL_DIM      0x9CD3

// ---- layout (big finger targets) ----
#define MENU_VISIBLE 4
#define ITEM_Y0      26
#define ITEM_H       44          // button drawn 42 px tall + 2 px gap
#define NAV_Y        204
#define NAV_H        34

#define HIT_NONE -1
#define HIT_BACK -2
#define HIT_UP   -3
#define HIT_DOWN -4

enum Screen { SC_CLOSED, SC_MAIN, SC_DRIVES, SC_DRIVE, SC_PICKER, SC_INFO, SC_BRIGHT,
              // m15: config-variant picker + confirmation screens
              SC_WIFI_PICKER, SC_PDP_PICKER, SC_CONFIRM_COPY, SC_CONFIRM_RESET };

static Screen   g_screen = SC_CLOSED;
static bool     g_dirty  = false;
static bool     g_reboot = false;
static bool     g_boot_change = false;
static bool     g_esp_restart = false;   // m15: full ESP32 hardware reset requested
// SC_CONFIRM_RESET dual-use: true = ESP.restart(), false = emulator restart
// (reload /pdpconfig.ini, remount, zero RAM, cold boot; keep WiFi/Telnet/FTP).
static bool     g_confirm_esp_reset = true;
static int      g_sel    = 0;          // drive index for SC_DRIVE / SC_PICKER
static int      g_scroll = 0;
static uint8_t  g_bright = 255;

#define MAX_FILES 16
static char g_files[MAX_FILES][44];
static int  g_file_count = 0;

// m15: config variants scanned from SD root for the WiFi / PDP pickers.
#define MAX_VARIANTS 16
static char g_variants[MAX_VARIANTS][44];
static int  g_variant_count = 0;

// m15: copy source/destination pending across the confirmation screens.
// Set when the user taps a variant in SC_WIFI_PICKER / SC_PDP_PICKER;
// consumed in SC_CONFIRM_COPY when the user confirms.
// g_pending_src sized to hold "/<prefix><variant>.ini" - variant is up
// to 42 chars, prefix is up to 11, plus "/" + ".ini" + null = 64.
static char  g_pending_src[64];      // e.g. "/wificonfig-home.ini"
static char  g_pending_dst[24];      // e.g. "/wificonfig.ini"
static char  g_pending_label[44];    // human-friendly name shown in the title

#define MAX_ITEMS 20
static char g_title[40];
static char g_items[MAX_ITEMS][44];
static int  g_count = 0;

// -------------------------------------------------------------------------
void ui_init() {
  g_screen = SC_CLOSED;
  g_dirty = false;
  g_reboot = false;
  g_boot_change = false;
  g_esp_restart = false;
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, g_bright);
}

bool ui_is_open()             { return g_screen != SC_CLOSED; }
bool ui_consume_reboot()      { bool r = g_reboot;      g_reboot      = false; return r; }
bool ui_consume_boot_change() { bool r = g_boot_change; g_boot_change = false; return r; }
bool ui_consume_esp_restart() { bool r = g_esp_restart; g_esp_restart = false; return r; }

// ---- scan SD root for mountable images ----
static void scan_files() {
  SD_FTP_StorageGuard guard;
  g_file_count = 0;
  fs::File root = SD_MMC.open("/");
  if (!root) return;
  for (fs::File f = root.openNextFile(); f && g_file_count < MAX_FILES;
       f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char* n = f.name();
      const char* slash = strrchr(n, '/');
      const char* base = slash ? slash + 1 : n;
      const char* dot = strrchr(base, '.');
      if (dot && (!strcasecmp(dot, ".dsk") || !strcasecmp(dot, ".hdd") ||
                  !strcasecmp(dot, ".img") || !strcasecmp(dot, ".ima"))) {
        strncpy(g_files[g_file_count], base, 43);
        g_files[g_file_count][43] = 0;
        g_file_count++;
      }
    }
    f.close();
  }
  root.close();
}

static String& cfg_disk_path(int slot) {
  switch (slot) {
    case DRIVE_A: return cfg.disk_a;
    case DRIVE_B: return cfg.disk_b;
    case DRIVE_C: return cfg.disk_c;
    case DRIVE_D: return cfg.disk_d;
    case DRIVE_RK0: return cfg.disk_rk0;
    case DRIVE_RP0: return cfg.disk_rp0;
    default:      return cfg.disk_d;
  }
}

static const char* unit_name(int slot) {
  switch (slot) {
    case DRIVE_A: return "DL0";
    case DRIVE_B: return "DL1";
    case DRIVE_C: return "DL2";
    case DRIVE_D: return "DL3";
    case DRIVE_RK0: return "RK0";
    case DRIVE_RP0: return "RP0";
    default: return "?";
  }
}

static bool slot_cfg_has_path(int slot) {
  return cfg_disk_path(slot).length() > 0;
}

static bool slot_live(int slot) {
  return slot >= DRIVE_A && slot < DRIVE_COUNT;
}

static bool slot_has_image(int slot) {
  return slot_live(slot) ? disk_is_mounted(slot) : cfg_disk_path(slot).length() > 0;
}

static bool valid_rl_file_size(const char* path, uint32_t* size_out = nullptr) {
  SD_FTP_StorageGuard guard;
  fs::File file = SD_MMC.open(path, "r");
  if (!file || file.isDirectory()) {
    if (file) file.close();
    if (size_out) *size_out = 0;
    return false;
  }
  uint32_t bytes = (uint32_t)file.size();
  file.close();
  if (size_out) *size_out = bytes;
  return rl11::valid_image_size(bytes);
}

static const char* slot_display_path(int slot) {
  if (slot_live(slot) && disk_is_mounted(slot) && disk_path(slot)[0])
    return disk_path(slot);
  return cfg_disk_path(slot).c_str();
}

// ---- build the item list / title for the current screen ----
static void rebuild() {
  g_count = 0;
  switch (g_screen) {
    case SC_MAIN:
      strcpy(g_title, "PDP-11/70 Settings");
      strcpy(g_items[g_count++], "Drives");
      strcpy(g_items[g_count++], "WiFi Config");
      strcpy(g_items[g_count++], "PDP Config");
      strcpy(g_items[g_count++], "System Info");
      strcpy(g_items[g_count++], "Brightness");
      strcpy(g_items[g_count++], "Restart Emulator");
      strcpy(g_items[g_count++], "Reset ESP32");
      break;
    case SC_DRIVES:
      strcpy(g_title, "Drives");
      {
        strcpy(g_items[g_count++],
               cfg.boot_kind == AppConfig::BK_RL ? "Boot RL0 [active]" : "Boot RL0");
        strcpy(g_items[g_count++],
               cfg.boot_kind == AppConfig::BK_RK ? "Boot RK0 [active]" : "Boot RK0");
        strcpy(g_items[g_count++],
               cfg.boot_kind == AppConfig::BK_RP ? "Boot RP0 [active]" : "Boot RP0");
        if (slot_cfg_has_path(DRIVE_RK0) || disk_is_mounted(DRIVE_RK0))
          snprintf(g_items[g_count++], 44, "RK0 %s",
                   slot_display_path(DRIVE_RK0));
        else
          strcpy(g_items[g_count++], "RK0 (empty)");
        if (slot_cfg_has_path(DRIVE_RP0) || disk_is_mounted(DRIVE_RP0))
          snprintf(g_items[g_count++], 44, "RP0 %s%s",
                   slot_display_path(DRIVE_RP0),
                   (disk_is_mounted(DRIVE_RP0) && disk_is_readonly(DRIVE_RP0))
                     ? " [RO]" : "");
        else
          strcpy(g_items[g_count++], "RP0 (empty)");
        for (int s = DRIVE_A; s <= DRIVE_D; s++) {
          if (slot_has_image(s))
            snprintf(g_items[g_count++], 44, "%s %s%s", unit_name(s),
                     slot_display_path(s),
                     (slot_live(s) && disk_is_readonly(s)) ? " [RO]" : "");
          else
            snprintf(g_items[g_count++], 44, "%s (empty)", unit_name(s));
        }
      }
      break;
    case SC_DRIVE: {
      snprintf(g_title, sizeof(g_title), "Drive %s", unit_name(g_sel));
      strcpy(g_items[g_count++],
             (slot_cfg_has_path(g_sel) || slot_has_image(g_sel))
               ? "Change Image" : "Mount Image");
      if (slot_cfg_has_path(g_sel) || slot_has_image(g_sel))
        strcpy(g_items[g_count++], "Dismount");
      break;
    }
    case SC_PICKER: {
      snprintf(g_title, sizeof(g_title), "Mount into %s", unit_name(g_sel));
      for (int i = 0; i < g_file_count; i++)
        strncpy(g_items[g_count++], g_files[i], 43);
      if (g_count == 0)
        strcpy(g_items[g_count++], "(no disk images)");
      break;
    }
    case SC_BRIGHT:
      snprintf(g_title, sizeof(g_title), "Brightness  %d%%",
               (g_bright * 100) / 255);
      strcpy(g_items[g_count++], "-  Dimmer");
      strcpy(g_items[g_count++], "+  Brighter");
      break;
    case SC_INFO:
      strcpy(g_title, "System Info");
      break;
    case SC_WIFI_PICKER:
      strcpy(g_title, "Select WiFi Config");
      if (g_variant_count == 0) {
        strcpy(g_items[g_count++], "(no variants found)");
      } else {
        for (int i = 0; i < g_variant_count && g_count < MAX_ITEMS; i++)
          strncpy(g_items[g_count++], g_variants[i], 43);
      }
      break;
    case SC_PDP_PICKER:
      strcpy(g_title, "Select PDP Config");
      if (g_variant_count == 0) {
        strcpy(g_items[g_count++], "(no variants found)");
      } else {
        for (int i = 0; i < g_variant_count && g_count < MAX_ITEMS; i++)
          strncpy(g_items[g_count++], g_variants[i], 43);
      }
      break;
    case SC_CONFIRM_COPY:
      snprintf(g_title, sizeof(g_title), "Apply: %s", g_pending_label);
      strcpy(g_items[g_count++], "Yes, copy");
      strcpy(g_items[g_count++], "Cancel");
      break;
    case SC_CONFIRM_RESET:
      if (g_confirm_esp_reset) {
        strcpy(g_title, "Reset ESP32 now?");
        strcpy(g_items[g_count++], "Yes, reset now");
        strcpy(g_items[g_count++], "Not yet");
      } else {
        strcpy(g_title, "Restart Emulator now?");
        strcpy(g_items[g_count++], "Yes, restart now");
        strcpy(g_items[g_count++], "Not yet");
      }
      break;
    default: break;
  }
  if (g_scroll > g_count - MENU_VISIBLE) g_scroll = g_count - MENU_VISIBLE;
  if (g_scroll < 0) g_scroll = 0;
}

static void go(Screen s) { g_screen = s; g_scroll = 0; rebuild(); g_dirty = true; }

void ui_open() { go(SC_MAIN); }

// -------------------------------------------------------------------------
static int list_hit(int x, int y) {
  if (y >= ITEM_Y0 && y < ITEM_Y0 + MENU_VISIBLE * ITEM_H && x >= 6 && x < 314) {
    return (y - ITEM_Y0) / ITEM_H;            // 0..3
  }
  if (y >= NAV_Y) {
    if (g_count > MENU_VISIBLE) {
      if (x < 156) return HIT_BACK;
      if (x < 236) return HIT_UP;
      return HIT_DOWN;
    }
    return HIT_BACK;
  }
  return HIT_NONE;
}

static void do_back() {
  switch (g_screen) {
    case SC_MAIN:           g_screen = SC_CLOSED; break;
    case SC_DRIVES:         go(SC_MAIN);   break;
    case SC_DRIVE:          go(SC_DRIVES); break;
    case SC_PICKER:         go(SC_DRIVES); break;
    case SC_INFO:           go(SC_MAIN);   break;
    case SC_BRIGHT:         go(SC_MAIN);   break;
    case SC_WIFI_PICKER:    go(SC_MAIN);   break;
    case SC_PDP_PICKER:     go(SC_MAIN);   break;
    case SC_CONFIRM_COPY:   go(SC_MAIN);   break;
    case SC_CONFIRM_RESET:  go(SC_MAIN);   break;
    default: break;
  }
}

// Scan SD root for /<prefix>NAME.ini variants. Populates g_variants[][]
// and g_variant_count. Called when the user opens SC_WIFI_PICKER or
// SC_PDP_PICKER (matches the existing scan_files() pattern, but pulls
// from appconfig.cpp so we don't duplicate the SD iteration here).
static void scan_variants(const char* prefix) {
  g_variant_count = config_list_variants(prefix, g_variants, MAX_VARIANTS);
}

static void activate(int idx) {        // idx = absolute item index
  if (idx < 0 || idx >= g_count) return;
  switch (g_screen) {
    case SC_MAIN:
      if      (idx == 0) go(SC_DRIVES);
      else if (idx == 1) { scan_variants(WIFI_CFG_PREFIX); go(SC_WIFI_PICKER); }
      else if (idx == 2) { scan_variants(PDP_CFG_PREFIX);  go(SC_PDP_PICKER);  }
      else if (idx == 3) go(SC_INFO);
      else if (idx == 4) go(SC_BRIGHT);
      else if (idx == 5) { g_reboot = true; g_screen = SC_CLOSED; }
      else if (idx == 6) {
        g_confirm_esp_reset = true;
        go(SC_CONFIRM_RESET);
      }
      break;
    case SC_DRIVES:
      if (idx == 0) {
        cfg.boot_kind = AppConfig::BK_RL;
        cfg.boot_drive = 'a';
        g_boot_change = true;
        g_screen = SC_CLOSED;
        g_dirty = false;
      } else if (idx == 1) {
        cfg.boot_kind = AppConfig::BK_RK;
        cfg.boot_drive = 'a';
        g_boot_change = true;
        g_screen = SC_CLOSED;
        g_dirty = false;
      } else if (idx == 2) {
        cfg.boot_kind = AppConfig::BK_RP;
        cfg.boot_drive = 'a';
        g_boot_change = true;
        g_screen = SC_CLOSED;
        g_dirty = false;
      } else if (idx == 3) {
        g_sel = DRIVE_RK0;
        go(SC_DRIVE);
      } else if (idx == 4) {
        g_sel = DRIVE_RP0;
        if (slot_cfg_has_path(DRIVE_RP0) || disk_is_mounted(DRIVE_RP0))
          go(SC_DRIVE);
        else {
          scan_files();
          go(SC_PICKER);
        }
      } else {
        g_sel = idx - 5;                 // 5..8 -> DL0..DL3
        if (slot_has_image(g_sel)) go(SC_DRIVE);
        else { scan_files(); go(SC_PICKER); }
      }
      break;
    case SC_DRIVE:
      if (idx == 0) { scan_files(); go(SC_PICKER); }
      else if (g_sel == DRIVE_RK0) {
        cfg.disk_rk0 = "";
        disk_dismount(DRIVE_RK0);
        if (cfg.boot_kind == AppConfig::BK_RK) {
          g_boot_change = true;
          g_screen = SC_CLOSED;
          g_dirty = false;
        } else {
          go(SC_DRIVES);
        }
      } else if (g_sel == DRIVE_RP0) {
        cfg.disk_rp0 = "";
        disk_dismount(DRIVE_RP0);
        rh11::media_changed(false);
        if (cfg.boot_kind == AppConfig::BK_RP) {
          g_boot_change = true;
          g_screen = SC_CLOSED;
          g_dirty = false;
        } else {
          go(SC_DRIVES);
        }
      } else {
        cfg_disk_path(g_sel) = "";
        if (slot_live(g_sel))
          disk_dismount(g_sel);
        go(SC_DRIVES);
      }
      break;
    case SC_PICKER: {
      if (g_file_count == 0 || idx >= g_file_count) break;
      char path[48];
      snprintf(path, sizeof(path), "/%s", g_files[idx]);
      bool ok = false;
      if (g_sel == DRIVE_RK0) {
        cfg.disk_rk0 = path;
        ok = disk_mount(DRIVE_RK0, path);
        if (ok && cfg.boot_kind == AppConfig::BK_RK) {
          g_boot_change = true;
          g_screen = SC_CLOSED;
          g_dirty = false;
        }
        LOG("ui: mount RK0: %s -> %s", path, ok ? "ok" : "FAIL");
      } else if (g_sel == DRIVE_RP0) {
        cfg.disk_rp0 = path;
        ok = disk_mount(DRIVE_RP0, path);
        if (ok) rh11::media_changed(true);
        if (ok && cfg.boot_kind == AppConfig::BK_RP) {
          g_boot_change = true;
          g_screen = SC_CLOSED;
          g_dirty = false;
        }
        LOG("ui: mount RP0: %s -> %s", path, ok ? "ok" : "FAIL");
      } else {
        uint32_t bytes = 0;
        if (!valid_rl_file_size(path, &bytes)) {
          LOGE("ui: mount %c: %s rejected, RL image size is %u bytes; expected RL01=%u or RL02=%u +/- %u%%",
               'A' + g_sel, path, (unsigned)bytes,
               (unsigned)rl11::RL01_IMAGE_BYTES,
               (unsigned)rl11::RL02_IMAGE_BYTES,
               (unsigned)DISK_SIZE_TOLERANCE_PERCENT);
          go(SC_DRIVES);
          break;
        }
        cfg_disk_path(g_sel) = path;
        ok = slot_live(g_sel) ? disk_mount(g_sel, path) : true;
        if (ok && slot_live(g_sel) && !rl11::validate_mounted_media(g_sel)) {
          uint32_t mounted_bytes = disk_size_bytes(g_sel);
          disk_dismount(g_sel);
          LOGE("ui: mount %c: %s rejected after mount, RL image size is %u bytes",
               'A' + g_sel, path, (unsigned)mounted_bytes);
          ok = false;
        }
        LOG("ui: mount %c: %s -> %s", 'A' + g_sel, path, ok ? "ok" : "FAIL");
      }
      if (g_screen != SC_CLOSED)
        go(SC_DRIVES);
      break;
    }
    case SC_BRIGHT:
      if (idx == 0) g_bright = (g_bright > 40) ? g_bright - 40 : 8;
      else          g_bright = (g_bright < 215) ? g_bright + 40 : 255;
      ledcWrite(TFT_BL, g_bright);
      rebuild(); g_dirty = true;
      break;
    case SC_WIFI_PICKER:
    case SC_PDP_PICKER: {
      if (g_variant_count == 0) break;       // "(no variants found)" placeholder
      if (idx >= g_variant_count) break;
      // Build the full source path and remember the active filename to copy to.
      const bool wifi = (g_screen == SC_WIFI_PICKER);
      const char* prefix = wifi ? WIFI_CFG_PREFIX : PDP_CFG_PREFIX;
      const char* dst    = wifi ? WIFI_CFG_PATH   : PDP_CFG_PATH;
      snprintf(g_pending_src, sizeof(g_pending_src),
               "/%s%s.ini", prefix, g_variants[idx]);
      strncpy(g_pending_dst, dst, sizeof(g_pending_dst) - 1);
      g_pending_dst[sizeof(g_pending_dst) - 1] = 0;
      strncpy(g_pending_label, g_variants[idx], sizeof(g_pending_label) - 1);
      g_pending_label[sizeof(g_pending_label) - 1] = 0;
      // WiFi changes need a full chip reset; PDP config only needs an
      // emulator restart so Telnet/FTP stay connected.
      g_confirm_esp_reset = wifi;
      go(SC_CONFIRM_COPY);
      break;
    }
    case SC_CONFIRM_COPY:
      if (idx == 0) {
        bool ok = config_copy_file(g_pending_src, g_pending_dst);
        LOG("ui: copy %s -> %s %s",
            g_pending_src, g_pending_dst, ok ? "OK" : "FAIL");
        go(SC_CONFIRM_RESET);
      } else {
        go(SC_MAIN);
      }
      break;
    case SC_CONFIRM_RESET:
      if (idx == 0) {
        // No LOG here: this handler runs with g_ui_mutex held, and
        // Serial.printf on USB-CDC can block indefinitely when no host
        // is reading the port. Set a one-shot flag and let loop() act
        // after the mutex is released.
        if (g_confirm_esp_reset)
          g_esp_restart = true;
        else
          g_reboot = true;   // reload pdpconfig, remount, cold boot
        g_screen = SC_CLOSED;   // close menu so loop() resumes
        g_dirty  = false;
      } else {
        go(SC_MAIN);
      }
      break;
    default: break;
  }
}

bool ui_handle_tap(int x, int y) {
  if (g_screen == SC_CLOSED) return false;
  int h = list_hit(x, y);
  if      (h == HIT_BACK) do_back();
  else if (h == HIT_UP)   { if (g_scroll > 0) { g_scroll--; g_dirty = true; } }
  else if (h == HIT_DOWN) { if (g_scroll + MENU_VISIBLE < g_count) { g_scroll++; g_dirty = true; } }
  else if (h >= 0)        activate(g_scroll + h);
  return true;
}

// -------------------------------------------------------------------------
static TFT_eSPI* T = nullptr;

static void draw_button(int x, int y, int w, int hh, const char* label,
                         uint16_t bg, uint16_t fg) {
  T->fillRoundRect(x, y, w, hh, 4, bg);
  T->drawRoundRect(x, y, w, hh, 4, COL_DIM);
  T->setTextColor(fg, bg);
  T->setTextDatum(ML_DATUM);
  T->drawString(label, x + 8, y + hh / 2, 2);
  T->setTextDatum(TL_DATUM);
}

static void draw_nav() {
  if (g_count > MENU_VISIBLE) {
    const char* bk = (g_screen == SC_MAIN) ? "Close" : "Back";
    draw_button(6,   NAV_Y, 144, NAV_H, bk,  COL_ITEM, COL_TEXT);
    draw_button(158, NAV_Y, 74,  NAV_H, " Up",   COL_ITEM, COL_TEXT);
    draw_button(240, NAV_Y, 74,  NAV_H, " Down", COL_ITEM, COL_TEXT);
  } else {
    const char* bk = (g_screen == SC_MAIN) ? "Close Menu" : "Back";
    draw_button(6, NAV_Y, 308, NAV_H, bk, COL_ITEM, COL_TEXT);
  }
}

static void draw_list() {
  T->fillScreen(COL_BG);
  T->fillRect(0, 0, 320, 24, COL_TITLE);
  T->setTextColor(COL_TEXT, COL_TITLE);
  T->setTextDatum(ML_DATUM);
  T->drawString(g_title, 6, 12, 2);
  T->setTextDatum(TL_DATUM);

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = g_scroll + i;
    if (idx >= g_count) break;
    int y = ITEM_Y0 + i * ITEM_H;
    uint16_t bg = COL_ITEM;
    if (g_screen == SC_DRIVES && idx == 0 && cfg.boot_kind == AppConfig::BK_RL) bg = COL_ITEM_HI;
    if (g_screen == SC_DRIVES && idx == 1 && cfg.boot_kind == AppConfig::BK_RK) bg = COL_ITEM_HI;
    if (g_screen == SC_DRIVES && idx == 2 && cfg.boot_kind == AppConfig::BK_RP) bg = COL_ITEM_HI;
    if (g_screen == SC_DRIVES && idx == 3 &&
        (cfg.disk_rk0.length() || disk_is_mounted(DRIVE_RK0))) bg = COL_ITEM_HI;
    if (g_screen == SC_DRIVES && idx == 4 &&
        (cfg.disk_rp0.length() || disk_is_mounted(DRIVE_RP0))) bg = COL_ITEM_HI;
    if (g_screen == SC_DRIVES && idx >= 5 && idx < 9 && slot_has_image(idx - 5)) bg = COL_ITEM_HI;
    // SC_MAIN items 5 (Restart Emulator) and 6 (Reset ESP32) are destructive.
    if (g_screen == SC_MAIN && (idx == 5 || idx == 6)) bg = COL_DANGER;
    if (g_screen == SC_DRIVE && idx == 1) bg = COL_DANGER;
    if (g_screen == SC_PICKER && idx == 0) bg = COL_ITEM_HI;   // create-new
    // m15: highlight the "Yes" buttons on the two confirmation screens.
    if (g_screen == SC_CONFIRM_RESET && idx == 0) bg = COL_DANGER;
    if (g_screen == SC_CONFIRM_COPY  && idx == 0) bg = COL_ITEM_HI;
    draw_button(6, y, 308, 42, g_items[idx], bg, COL_TEXT);
  }
  draw_nav();
}

static void draw_info() {
  T->fillScreen(COL_BG);
  T->fillRect(0, 0, 320, 24, COL_TITLE);
  T->setTextColor(COL_TEXT, COL_TITLE);
  T->setTextDatum(ML_DATUM);
  T->drawString(g_title, 6, 12, 2);
  T->setTextDatum(TL_DATUM);
  T->setTextColor(COL_TEXT, COL_BG);

  char line[64];
  int y = 30;
  const char* title = cfg.title.length() ? cfg.title.c_str() : APP_TITLE;
  snprintf(line, sizeof(line), "%s", title);
  T->drawString(line, 10, y, 2); y += 18;
  snprintf(line, sizeof(line), "SW %s  build %s", APP_VERSION, APP_BUILD_DATE);
  T->drawString(line, 10, y, 2); y += 18;
  snprintf(line, sizeof(line), "Core: %s", pdp_core::engine_name());
  T->drawString(line, 10, y, 2); y += 18;
  snprintf(line, sizeof(line), "RAM: %uKW active / %uKW config",
           (unsigned)(pdp_core::memory_size() / 2048),
           (unsigned)(pdp_core::target_memory_bytes() / 2048));
  T->drawString(line, 10, y, 2); y += 18;

  {
    const char* boot_dev = cfg.boot_unit_label();
    const char* boot_img = "";
    if (cfg.boot_kind == AppConfig::BK_RK) {
      if (disk_is_mounted(DRIVE_RK0) && disk_path(DRIVE_RK0)[0])
        boot_img = disk_path(DRIVE_RK0);
      else if (cfg.disk_rk0.length())
        boot_img = cfg.disk_rk0.c_str();
    } else if (cfg.boot_kind == AppConfig::BK_RP) {
      if (disk_is_mounted(DRIVE_RP0) && disk_path(DRIVE_RP0)[0])
        boot_img = disk_path(DRIVE_RP0);
      else if (cfg.disk_rp0.length())
        boot_img = cfg.disk_rp0.c_str();
    } else {
      if (disk_is_mounted(DRIVE_A) && disk_path(DRIVE_A)[0])
        boot_img = disk_path(DRIVE_A);
      else if (cfg.disk_a.length())
        boot_img = cfg.disk_a.c_str();
    }
    snprintf(line, sizeof(line), "Boot: %s", boot_dev);
    T->drawString(line, 10, y, 2); y += 18;
    snprintf(line, sizeof(line), "Image: %s",
             (boot_img && boot_img[0]) ? boot_img : "(none)");
    T->drawString(line, 10, y, 2); y += 18;
  }

  if (cfg.boot_kind != AppConfig::BK_RP) {
    const char* rp_img = "";
    if (disk_is_mounted(DRIVE_RP0) && disk_path(DRIVE_RP0)[0])
      rp_img = disk_path(DRIVE_RP0);
    else if (cfg.disk_rp0.length())
      rp_img = cfg.disk_rp0.c_str();
    snprintf(line, sizeof(line), "RP0: %s",
             (rp_img && rp_img[0]) ? rp_img : "(none)");
    T->drawString(line, 10, y, 2); y += 20;
  } else {
    y += 2;
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "WiFi: %s", WiFi.SSID().c_str());
    T->setTextColor(COL_ACCENT, COL_BG); T->drawString(line, 10, y, 2); y += 18;
    snprintf(line, sizeof(line), "IP:   %s", WiFi.localIP().toString().c_str());
    T->drawString(line, 10, y, 2); y += 20;
  } else {
    T->setTextColor(COL_DANGER, COL_BG);
    T->drawString("WiFi: disconnected", 10, y, 2); y += 20;
  }

  T->setTextColor(COL_TEXT, COL_BG);
  if (!telnet_enabled())
    snprintf(line, sizeof(line), "Telnet: disabled");
  else if (telnet_connected())
    snprintf(line, sizeof(line), "Telnet %u: client %s",
             telnet_port(), telnet_client_ip());
  else
    snprintf(line, sizeof(line), "Telnet %u: no client", telnet_port());
  T->drawString(line, 10, y, 2); y += 18;

  if (!ftp_enabled())
    snprintf(line, sizeof(line), "FTP: disabled");
  else if (ftp_connected())
    snprintf(line, sizeof(line), "FTP %u: client", ftp_port());
  else if (ftp_listening())
    snprintf(line, sizeof(line), "FTP %u: listening", ftp_port());
  else
    snprintf(line, sizeof(line), "FTP %u: not listening", ftp_port());
  T->drawString(line, 10, y, 2);

  draw_nav();
}

void ui_draw(TFT_eSPI& tft) {
  if (g_screen == SC_CLOSED || !g_dirty) return;
  T = &tft;
  if (g_screen == SC_INFO) draw_info();
  else                     draw_list();
  g_dirty = false;
}
