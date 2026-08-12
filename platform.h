#pragma once
#include <Arduino.h>
#include "config.h"

// Compile-time DZ11 + second Telnet. Set to 0 to strip from the binary for
// ~10–15% higher status-bar MIPS when multi-user serial is unused (see
// docs/dz11.md). Runtime [dz11] in wificonfig.ini is ignored when this is 0.
#ifndef VPDP_ENABLE_DZ11
#define VPDP_ENABLE_DZ11 1
#endif

// Set true at the end of panic() so the post-HALT trace ring is the last
// thing on the USB-Serial monitor. Reset in cpu_reset() so a reboot from
// the touch menu re-enables serial output. TFT + Telnet are not gated.
extern volatile bool g_serial_silenced;

// With USB CDC On Boot Disabled (required for Elecrow), Serial is UART0 —
// the same port that shows ESP-ROM lines. Single-stream LOG is correct.
#define LOG(fmt, ...)   do { if (!g_serial_silenced) Serial.printf("[vpdp1170] " fmt "\r\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...)  do { if (!g_serial_silenced) Serial.printf("[vpdp1170 ERR] " fmt "\r\n", ##__VA_ARGS__); } while (0)
