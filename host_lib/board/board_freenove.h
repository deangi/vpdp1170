#pragma once
#ifndef HOST_LIB_BOARD_FREENOVE_H
#define HOST_LIB_BOARD_FREENOVE_H

// Freenove ESP32-S3 Display 2.8" (FNK0104B) — current production host board.
// Selected when VPDP_BOARD == VPDP_BOARD_FREENOVE_28 (default).

#define VPDP_BOARD_NAME        "Freenove 2.8\""
#define VPDP_DISPLAY_BACKEND   VPDP_DISPLAY_TFT_ESPI
#define VPDP_TOUCH_BACKEND     VPDP_TOUCH_FT6336U
#define VPDP_SD_BACKEND        VPDP_SD_SDMMC4

// ---- RGB LED (WS2812) ----
#define VPDP_HAS_WS2812     1
#define LED_PIN             42
#define LED_CHANNEL         0
#define LED_COUNT           1

// ---- Onboard button ----
#define BUTTON_PIN          0

// ---- TFT (ILI9341 via TFT_eSPI FNK0104B preset) ----
// Pins live in TFT_eSPI User_Setup_Select.h:
//   TFT_MISO=13 TFT_MOSI=11 TFT_SCLK=12 TFT_CS=10 TFT_DC=46 TFT_BL=45 @ 40 MHz
#define TFT_W               320
#define TFT_H               240
#define TEXT_COLS           80
#define TEXT_ROWS           25
#define CELL_W              4
#define CELL_H              8
#define VPDP_STATUS_BAND_H  (TFT_H - (TEXT_ROWS * CELL_H))  // 40 px
#define VPDP_TFT_ROTATION   1

// ---- Capacitive touch FT6336U (I2C) ----
#define TOUCH_SDA           16
#define TOUCH_SCL           15
#define TOUCH_RST           18
#define TOUCH_INT           17
#define TOUCH_I2C_ADDR      0x38

// ---- SD_MMC 4-bit ----
#define SD_MMC_CMD          40
#define SD_MMC_CLK          38
#define SD_MMC_D0           39
#define SD_MMC_D1           41
#define SD_MMC_D2           48
#define SD_MMC_D3           47

#endif  // HOST_LIB_BOARD_FREENOVE_H
