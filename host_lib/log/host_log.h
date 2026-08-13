#pragma once
#include <Arduino.h>

// Define HOST_LOG_TAG before including this header (e.g. "vpdp1170").
#ifndef HOST_LOG_TAG
#define HOST_LOG_TAG "host"
#endif

// Set true to mute USB-Serial LOG/LOGE (panic dump). TFT/Telnet are not gated.
extern volatile bool g_serial_silenced;

#define LOG(fmt, ...)   do { if (!g_serial_silenced) Serial.printf("[" HOST_LOG_TAG "] " fmt "\r\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...)  do { if (!g_serial_silenced) Serial.printf("[" HOST_LOG_TAG " ERR] " fmt "\r\n", ##__VA_ARGS__); } while (0)
