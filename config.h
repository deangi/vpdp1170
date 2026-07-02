#pragma once

// ---- App metadata ----
#define APP_TITLE       "vpdp1170"
#define APP_VERSION     "V1.1"
#define APP_BUILD_DATE  "2026-07-02"

// ---- PDP core selection ----
// 0 = inherited vpdp1140/sam11 PDP-11/40-derived scaffold.
// 1 = kek PDP-11/70 engine path.
#define VPDP1170_USE_KEK_CORE 1
// Internal bring-up gate. 1 compiles the staged Arduino kek adapter and the
// selected upstream source wrappers. This is expected to reach the CPU/MMU
// self-test stage before full disk/console boot is wired.
#define VPDP1170_BUILD_KEK_ADAPTER 1
#define VPDP1170_TARGET_RAM_BYTES 0x400000u   // PDP-11/70 22-bit, 4 MB

// Mounted disk images, FTP, TT1, and shell file commands may all hold files open.
#define SD_MAX_OPEN_FILES 16

// ---- RGB LED (WS2812) ----
#define LED_PIN         42
#define LED_CHANNEL     0
#define LED_COUNT       1

// ---- Onboard button ----
#define BUTTON_PIN      0

// ---- TFT (ILI9341, configured via TFT_eSPI FNK0104B preset) ----
// Reference only - actual pins live in TFT_eSPI User_Setup_Select.h (FNK0104B):
//   TFT_MISO=13 TFT_MOSI=11 TFT_SCLK=12 TFT_CS=10 TFT_DC=46 TFT_BL=45 @ 40 MHz
#define TFT_W           320
#define TFT_H           240
#define TEXT_COLS       80
#define TEXT_ROWS       25
#define CELL_W          4
#define CELL_H          8

// ---- Capacitive touch FT6336U (I2C) ----
#define TOUCH_SDA       16
#define TOUCH_SCL       15
#define TOUCH_RST       18
#define TOUCH_INT       17
#define TOUCH_I2C_ADDR  0x38

// ---- SD_MMC 4-bit ----
#define SD_MMC_CMD      40
#define SD_MMC_CLK      38
#define SD_MMC_D0       39
#define SD_MMC_D1       41
#define SD_MMC_D2       48
#define SD_MMC_D3       47

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
