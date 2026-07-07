#include "disk.h"
#include "config.h"
#include "platform.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <string.h>

struct DriveSlot {
  File     file;
  bool     mounted  = false;
  bool     changed  = false;       // media-change flag (set on mount)
  bool     readonly = false;       // image could only be opened read-only
  char     path[64] = {0};
  uint32_t size = 0;
  uint32_t reads = 0;
  uint32_t writes = 0;
};

static DriveSlot g_drv[DRIVE_COUNT];
static char g_last_error[128] = {0};

static void set_last_error(const char* text) {
  strncpy(g_last_error, text ? text : "", sizeof(g_last_error) - 1);
  g_last_error[sizeof(g_last_error) - 1] = 0;
}

const char* disk_last_error() {
  return g_last_error;
}

static bool slot_valid(int s) { return s >= 0 && s < DRIVE_COUNT; }
static const char* slot_name(int s) {
  static char name[4];
  if (s == DRIVE_RK0) return "RK0";
  if (s == DRIVE_RP0) return "RP0";
  if (s >= DRIVE_A && s <= DRIVE_D) {
    name[0] = 'D';
    name[1] = 'L';
    name[2] = '0' + s;
    name[3] = 0;
    return name;
  }
  return "?";
}

bool disk_size_within_tolerance(uint32_t bytes, uint32_t nominal) {
  const uint32_t delta = (nominal * DISK_SIZE_TOLERANCE_PERCENT) / 100u;
  return bytes >= nominal - delta && bytes <= nominal + delta;
}

bool disk_size_is_rk(uint32_t bytes) {
  return disk_size_within_tolerance(bytes, DISK_RK05_IMAGE_BYTES);
}

bool disk_size_is_rl01(uint32_t bytes) {
  return disk_size_within_tolerance(bytes, DISK_RL01_IMAGE_BYTES);
}

bool disk_size_is_rl02(uint32_t bytes) {
  return disk_size_within_tolerance(bytes, DISK_RL02_IMAGE_BYTES);
}

bool disk_size_is_rl(uint32_t bytes) {
  return disk_size_is_rl01(bytes) || disk_size_is_rl02(bytes);
}

static bool validate_slot_image_size(int slot, uint32_t bytes) {
  if (slot >= DRIVE_A && slot <= DRIVE_D) {
    if (disk_size_is_rl(bytes)) return true;
    snprintf(g_last_error, sizeof(g_last_error),
             "RL image size %u is outside RL01/RL02 +/- %u%%",
             (unsigned)bytes, (unsigned)DISK_SIZE_TOLERANCE_PERCENT);
    return false;
  }

  if (slot == DRIVE_RK0) {
    if (disk_size_is_rk(bytes)) return true;
    snprintf(g_last_error, sizeof(g_last_error),
             "RK image size %u is outside RK05 +/- %u%%",
             (unsigned)bytes, (unsigned)DISK_SIZE_TOLERANCE_PERCENT);
    return false;
  }

  const uint32_t MIN_IMAGE = 100u * 1024u;
  const uint32_t MAX_IMAGE = 256u * 1024u * 1024u;
  if (bytes >= MIN_IMAGE && bytes <= MAX_IMAGE) return true;
  snprintf(g_last_error, sizeof(g_last_error),
           "image size %u is outside the supported range",
           (unsigned)bytes);
  return false;
}

bool disk_mount_mode(int slot, const char* path, bool force_readonly) {
  SD_FTP_StorageGuard guard;
  set_last_error("");
  if (!slot_valid(slot) || !path || !*path) {
    set_last_error("invalid drive or path");
    return false;
  }
  if (strlen(path) >= sizeof(g_drv[slot].path)) {
    LOGE("disk_mount[%d]: path too long", slot);
    set_last_error("path is too long");
    return false;
  }

  // Open and validate the replacement before disturbing the current image.
  // Runtime EMU commands therefore leave the existing disk attached if the
  // requested file is missing or invalid.
  bool readonly = force_readonly;
  File f;
  if (!force_readonly) f = SD_MMC.open(path, "r+");
  if (!f) {
    f = SD_MMC.open(path, "r");
    if (!f) {
      LOGE("disk_mount[%d]: cannot open %s", slot, path);
      if (!SD_MMC.exists(path))
        set_last_error("file not found");
      else
        set_last_error("cannot open file (file lock or SD handle limit)");
      return false;
    }
    readonly = true;
  }
  uint32_t sz = (uint32_t)f.size();

  if (!validate_slot_image_size(slot, sz)) {
    LOGE("disk_mount[%s]: %s is %u bytes, invalid for this drive: %s",
         slot_name(slot), path, (unsigned)sz, g_last_error);
    f.close();
    return false;
  }

  if (g_drv[slot].mounted) {
    g_drv[slot].file.flush();
    g_drv[slot].file.close();
  }
  g_drv[slot].file     = f;
  g_drv[slot].mounted  = true;
  g_drv[slot].changed  = true;
  g_drv[slot].readonly = readonly;
  g_drv[slot].size     = sz;
  g_drv[slot].reads    = 0;
  g_drv[slot].writes   = 0;
  strncpy(g_drv[slot].path, path, sizeof(g_drv[slot].path) - 1);
  g_drv[slot].path[sizeof(g_drv[slot].path) - 1] = 0;

  LOG("disk_mount[%s]: %s (%u bytes)%s", slot_name(slot), path, (unsigned)sz,
      readonly ? " [read-only]" : "");
  return true;
}

bool disk_mount(int slot, const char* path) {
  return disk_mount_mode(slot, path, false);
}

void disk_dismount(int slot) {
  SD_FTP_StorageGuard guard;
  if (!slot_valid(slot)) return;
  if (g_drv[slot].mounted) {
    g_drv[slot].file.flush();
    g_drv[slot].file.close();
    LOG("disk_dismount[%s]: %s", slot_name(slot), g_drv[slot].path);
  }
  g_drv[slot].mounted = false;
  g_drv[slot].changed = true;
  g_drv[slot].readonly = false;
  g_drv[slot].size = 0;
  g_drv[slot].path[0] = 0;
}

void disk_flush_all() {
  SD_FTP_StorageGuard guard;
  for (int slot = 0; slot < DRIVE_COUNT; slot++)
    if (g_drv[slot].mounted && !g_drv[slot].readonly)
      g_drv[slot].file.flush();
}

bool disk_reopen_all() {
  struct ReopenInfo {
    bool mounted;
    bool readonly;
    char path[64];
  };
  ReopenInfo saved[DRIVE_COUNT] = {};

  {
    SD_FTP_StorageGuard guard;
    for (int slot = 0; slot < DRIVE_COUNT; slot++) {
      saved[slot].mounted = g_drv[slot].mounted;
      saved[slot].readonly = g_drv[slot].readonly;
      strncpy(saved[slot].path, g_drv[slot].path,
              sizeof(saved[slot].path) - 1);
      saved[slot].path[sizeof(saved[slot].path) - 1] = 0;

      if (g_drv[slot].mounted) {
        if (!g_drv[slot].readonly) g_drv[slot].file.flush();
        g_drv[slot].file.close();
      }
      g_drv[slot].mounted = false;
      g_drv[slot].readonly = false;
      g_drv[slot].size = 0;
      g_drv[slot].path[0] = 0;
    }
  }

  // Give the SDMMC/VFS layer a scheduling point after closing the old handles
  // before opening replacements. This is outside the storage lock so FTP is
  // not blocked during the delay.
  delay(2);

  bool all_ok = true;
  for (int slot = 0; slot < DRIVE_COUNT; slot++) {
    if (!saved[slot].mounted) continue;
    if (!disk_mount_mode(slot, saved[slot].path, saved[slot].readonly)) {
      LOGE("disk_reopen_all[%s]: failed to reopen %s",
           slot_name(slot), saved[slot].path);
      all_ok = false;
    }
  }
  return all_ok;
}

bool disk_is_mounted(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted;
}

bool disk_is_readonly(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted && g_drv[slot].readonly;
}

const char* disk_path(int slot) {
  return slot_valid(slot) ? g_drv[slot].path : "";
}

uint32_t disk_size_bytes(int slot) {
  return slot_valid(slot) && g_drv[slot].mounted ? g_drv[slot].size : 0;
}

int disk_read(int slot, uint32_t byte_offset, void* buf, uint32_t bytes) {
  SD_FTP_StorageGuard guard;
  if (!disk_is_mounted(slot)) return -1;
  if (bytes == 0) return 0;
  DriveSlot& d = g_drv[slot];
  uint8_t* out = (uint8_t*)buf;
  if (!out) return -1;

  if (byte_offset >= d.size) {
    memset(out, 0, bytes);
    d.reads++;
    return (int)bytes;
  }

  uint32_t available = d.size - byte_offset;
  uint32_t read_len = bytes < available ? bytes : available;
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_read[%s]: seek to %u failed", slot_name(slot), (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.read(out, read_len);
  d.reads++;
  if (n != read_len) {
    LOGE("disk_read[%s]: short read %u/%u at off %u",
         slot_name(slot), (unsigned)n, (unsigned)read_len, (unsigned)byte_offset);
    return -1;
  }
  if (read_len < bytes)
    memset(out + read_len, 0, bytes - read_len);
  return (int)bytes;
}

int disk_write(int slot, uint32_t byte_offset, const void* buf, uint32_t bytes) {
  SD_FTP_StorageGuard guard;
  if (!disk_is_mounted(slot)) return -1;
  DriveSlot& d = g_drv[slot];
  if (d.readonly) return -1;
  if (byte_offset > d.size || bytes > d.size - byte_offset) {
    LOGE("disk_write[%s]: out of range off=%u len=%u size=%u",
         slot_name(slot), (unsigned)byte_offset, (unsigned)bytes, (unsigned)d.size);
    return -1;
  }
  if (!d.file.seek(byte_offset)) {
    LOGE("disk_write[%s]: seek to %u failed", slot_name(slot), (unsigned)byte_offset);
    return -1;
  }
  size_t n = d.file.write((const uint8_t*)buf, bytes);
  d.file.flush();          // write-through
  d.writes++;
  if (n != bytes) {
    LOGE("disk_write[%s]: short write %u/%u", slot_name(slot), (unsigned)n, (unsigned)bytes);
    return -1;
  }
  return (int)bytes;
}

void disk_stats(int slot, uint32_t* reads, uint32_t* writes) {
  if (!slot_valid(slot)) { if (reads) *reads = 0; if (writes) *writes = 0; return; }
  if (reads)  *reads  = g_drv[slot].reads;
  if (writes) *writes = g_drv[slot].writes;
}

bool disk_take_change(int slot) {
  if (!slot_valid(slot)) return false;
  bool c = g_drv[slot].changed;
  g_drv[slot].changed = false;
  return c;
}
