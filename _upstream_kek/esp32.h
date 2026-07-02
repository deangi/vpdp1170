// Local vpdp1170 Arduino shim for kek's root-level #include "esp32.h".
//
// The upstream ESP32 port normally builds from _upstream_kek/ESP32, where
// esp32.h pulls in SdFat and board-specific serial/disk assumptions. The
// Freenove host sketch already owns SD_MMC, Telnet, FTP, TFT, and board pins,
// so the first kek CPU/MMU bring-up needs only Arduino/FreeRTOS primitives.

#pragma once

#include <Arduino.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

#ifndef DMAMEM
#define DMAMEM
#endif

#ifndef EXTMEM
#define EXTMEM
#endif
