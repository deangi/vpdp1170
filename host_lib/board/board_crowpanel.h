#pragma once
#ifndef HOST_LIB_BOARD_CROWPANEL_H
#define HOST_LIB_BOARD_CROWPANEL_H

// Elecrow CrowPanel Advance 7" (ESP32-S3, SC7277 RGB 800x480, GT911).
// Selected when VPDP_BOARD == VPDP_BOARD_CROWPANEL_7.
//
// Learnings from CrowPanelBringup (local-only sketch, not in git):
//   - Rear DIP S1/S0 mux GPIO4/5/6 between SPK and TF. Factory ships S1=0
//     S0=0 (no SD). For TF set S1=1 S0=1 and power-cycle.
//   - SD is SPI MOSI=6 MISO=4 SCK=5; card CS is hard-tied → soft CS = NC.
//     Stable clock is 20 MHz (40 MHz HS mode fails through the mux).
//   - RGB FB in PSRAM; PCLK 15 MHz. Call display()/writeback after draws.
//   - STC8H backlight @ 0x30: mute+max before gfx.init. Runtime dim/bright
//     uses IDF i2c_master_write_to_device on I2C_NUM_0 (0=max .. 244=min) —
//     do NOT Wire.begin() again after Lovyan owns GT911.
//   - Panel + touch offset_rotation = 2; do NOT also setRotation().
//   - Do NOT call initDMA() after Panel_RGB init (framebuffer can end up null).
//   - Never drive GPIO45 as "TFT_BL" — that pin is RGB data B3 on this panel.

#define VPDP_BOARD_NAME        "CrowPanel Advance 7\""
#define VPDP_DISPLAY_BACKEND   VPDP_DISPLAY_LOVYANGFX
#define VPDP_TOUCH_BACKEND     VPDP_TOUCH_GT911
#define VPDP_SD_BACKEND        VPDP_SD_SPI_IDF

// ---- No WS2812 on this board; backlight is STC8H ----
#define VPDP_HAS_WS2812     0
#define LED_PIN             (-1)
#define LED_CHANNEL         0
#define LED_COUNT           0

#define BUTTON_PIN          0   // BOOT

// ---- RGB panel geometry (LovyanGFX Panel_RGB / Bus_RGB) ----
#define TFT_W               800
#define TFT_H               480
#define TEXT_COLS           80
#define TEXT_ROWS           25
#define CELL_W              10  // Terminus 8x16 glyphs centered in 10x16 cells
#define CELL_H              16
#define VPDP_STATUS_BAND_H  (TFT_H - (TEXT_ROWS * CELL_H))  // 80 px
#define VPDP_TFT_ROTATION   0   // orientation via offset_rotation=2 in lgfx conf
#ifndef CROW_PCLK_HZ
#define CROW_PCLK_HZ        15000000
#endif

// ---- GT911 touch (I2C; Lovyan owns the bus after gfx.init) ----
#define TOUCH_SDA           15
#define TOUCH_SCL           16
#define TOUCH_RST           (-1)
#define TOUCH_INT           (-1)
#define TOUCH_I2C_ADDR      0x5D

// ---- Backlight STC8H (I2C, pre-gfx only) ----
#define CROW_STC8H_ADDR     0x30
#define CROW_BL_MAX_CMD     0
#define CROW_BL_MUTE_CMD    249   // mute amp before BL (bring-up order)
#define CROW_BL_OFF_CMD     245

// ---- GT911 reset / power pins (must run before Wire + gfx.init) ----
#define CROW_GT911_RST      1
#define CROW_GT911_INT_A    19
#define CROW_GT911_INT_B    20

// ---- SPI TF slot (DIP must be S1=1 S0=1) ----
#define CROW_SD_MOSI        6
#define CROW_SD_MISO        4
#define CROW_SD_SCK         5
#define CROW_SD_SPI_KHZ     20000
#define CROW_SD_MOUNT_POINT "/sdcard"

#endif  // HOST_LIB_BOARD_CROWPANEL_H
