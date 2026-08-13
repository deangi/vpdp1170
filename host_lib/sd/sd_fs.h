#pragma once

// Board-neutral SD filesystem handle.
//   Freenove  : SD_FS == SD_MMC (4-bit SD_MMC)
//   CrowPanel : SD_FS == crow_sd facade over ESP-IDF SDSPI (CS=NC) at /sdcard
//
// Call sites should #include "sd_fs.h" and use SD_FS.open/exists/… instead of
// SD_MMC. FTP already talks POSIX VFS at /sdcard and needs no change once
// sd_mount() succeeds on either backend.

#include "config.h"
#include <FS.h>

#if VPDP_SD_BACKEND == VPDP_SD_SDMMC4

#include <SD_MMC.h>
#define SD_FS SD_MMC

#else

// Match Arduino SD/SD_MMC cardType() values without pulling the SPI SD library.
#ifndef CARD_NONE
#define CARD_NONE    0
#define CARD_MMC     1
#define CARD_SD      2
#define CARD_SDHC    3
#define CARD_UNKNOWN 4
#endif

// Thin FS + cardSize/cardType surface matching what this project uses from SD_MMC.
class CrowSdFs {
public:
  fs::File open(const char* path, const char* mode = FILE_READ);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* pathFrom, const char* pathTo);
  uint64_t cardSize() const;
  uint8_t cardType() const;
};

extern CrowSdFs SD_FS;

// IDF SDSPI mount used by sd_mount() on CrowPanel.
bool crow_sd_mount();

#endif
