#pragma once

// ---- App metadata ----
#define APP_TITLE       "vpdp1170"
#define APP_VERSION     "V2.6"
#define APP_BUILD_DATE  "2026-08-05"

// ---- Board selection ----
// Arduino IDE compiles each .cpp separately, so a #define in the .ino does NOT
// reach console.cpp / ui.cpp / touch.cpp. Set the board HERE (the one place all
// translation units include). Flip between:
//   VPDP_BOARD_FREENOVE_28  — Freenove 2.8" (COM18)
//   VPDP_BOARD_CROWPANEL_7  — Elecrow CrowPanel Advance 7" (COM3)
// Display is board-abstracted (gfx.h). CrowPanel SD-SPI and GT911 (via Lovyan
// getTouch) are wired; the settings menu still uses the Freenove 320×240 layout
// in the top-left corner on the 800×480 panel.
#define VPDP_BOARD_FREENOVE_28   1
#define VPDP_BOARD_CROWPANEL_7   2
#ifndef VPDP_BOARD
#define VPDP_BOARD VPDP_BOARD_FREENOVE_28
#endif

#define VPDP_DISPLAY_TFT_ESPI    1
#define VPDP_DISPLAY_LOVYANGFX   2
#define VPDP_TOUCH_FT6336U       1
#define VPDP_TOUCH_GT911         2
#define VPDP_SD_SDMMC4           1
#define VPDP_SD_SPI_IDF          2

#if VPDP_BOARD == VPDP_BOARD_FREENOVE_28
#include "board_freenove.h"
#elif VPDP_BOARD == VPDP_BOARD_CROWPANEL_7
#include "board_crowpanel.h"
#else
#error "Unknown VPDP_BOARD — use VPDP_BOARD_FREENOVE_28 or VPDP_BOARD_CROWPANEL_7"
#endif

// ---- PDP core ----
// Guest CPU/MMU/bus is always the kek PDP-11/70 adapter. The inherited
// sam11/11/40 scaffold is kept under legacy_sam11/ for reference only and
// is not compiled by the Arduino sketch.
#define VPDP1170_TARGET_RAM_BYTES 0x400000u   // PDP-11/70 22-bit, 4 MB

// Run the deterministic kek microbenchmark suite once during setup, before
// the selected guest is booted. Disable after performance characterization
// if the extra startup delay is undesirable.
#define VPDP1170_STARTUP_BENCHMARK 0

// Mounted disk images, FTP, TT1, and shell file commands may all hold files open.
#define SD_MAX_OPEN_FILES 16

// ---- File paths on SD (defaults; overridden by config files) ----
// m15: config split into two files so users can carry named variants
// (/wificonfig-home.ini, /pdpconfig-rt11.ini, ...) and pick via menu.
#define WIFI_CFG_PATH    "/wificonfig.ini"
#define PDP_CFG_PATH     "/pdpconfig.ini"
#define WIFI_CFG_PREFIX  "wificonfig-"      // variant discovery prefix
#define PDP_CFG_PREFIX   "pdpconfig-"

#define DEFAULT_DL0_IMG "/rt11sj.dsk"      // RT-11 SJ V5.x on RL02 (10 MB)
#define DEFAULT_DL1_IMG ""                  // DL1 dismounted by default
#define DEFAULT_DL2_IMG ""                  // DL2 dismounted by default
#define DEFAULT_DL3_IMG ""                  // DL3 dismounted by default

// ---- Network ----
#define TELNET_PORT     23
#define FTP_PORT        21
#define FTP_DEFAULT_USER "esp32"
#define FTP_DEFAULT_PASS "esp32"

// ---- Disk geometries ----
// RL02: 512 cylinders x 2 heads x 40 sectors x 256 words = 10 485 760 bytes.
#define RL02_BYTES      10485760UL
#define RL02_CYL        512
#define RL02_HEADS      2
#define RL02_SEC        40
#define RL02_WORDS_PER_SEC 256
#define RL02_BYTES_PER_SEC (RL02_WORDS_PER_SEC * 2)   // 512 bytes/sector

// RP04/RP05/RP06 via RH11. All use 19 heads, 22 sectors/track, 512 bytes/sector.
#define RP_HEADS       19
#define RP_SECTORS     22
#define RP_BYTES_PER_SEC 512
#define RP04_CYL       411
#define RP05_CYL       411
#define RP06_CYL       815

// ---- Boot tuning ----
#define WIFI_CONNECT_TIMEOUT_MS  20000
