#pragma once
#include <stdint.h>

// Guest media slots. Legacy A..D identifiers map to RL11 units DL0..DL3.
// RP0 is a secondary RH11/RP disk and is not part of the boot-drive set.
enum {
  DRIVE_A = 0,   // DL0
  DRIVE_B = 1,   // DL1
  DRIVE_C = 2,   // DL2
  DRIVE_D = 3,   // DL3
  DRIVE_RP0 = 4, // RP0
  DRIVE_COUNT = 5
};

// Mount an image file (path on the SD card) into a drive slot.
// Basic size validation is performed here; the selected PDP-11 controller
// remains responsible for enforcing its media geometry.
// Returns true on success.
bool disk_mount(int slot, const char* path);
bool disk_mount_mode(int slot, const char* path, bool force_readonly);
const char* disk_last_error();

// Close the image and free the slot.
void disk_dismount(int slot);
void disk_flush_all();

// Close and reopen every currently mounted image, preserving path and
// read-only mode. Used before a PDP cold reboot so the new boot does not
// depend on long-lived SD_MMC File handles.
bool disk_reopen_all();

bool        disk_is_mounted(int slot);
bool        disk_is_readonly(int slot);   // true if the image opened read-only
const char* disk_path(int slot);          // mounted path, or ""
uint32_t    disk_size_bytes(int slot);    // 0 if not mounted

// Byte-level transfer. Returns bytes transferred, or -1 on error.
int disk_read (int slot, uint32_t byte_offset, void* buf, uint32_t bytes);
int disk_write(int slot, uint32_t byte_offset, const void* buf, uint32_t bytes);

// Diagnostics counter (reads/writes since boot).
void disk_stats(int slot, uint32_t* reads, uint32_t* writes);

// Media-change flag: set true whenever an image is (re)mounted. Returns the
// current value and clears it.
bool disk_take_change(int slot);
