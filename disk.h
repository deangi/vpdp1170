#pragma once
#include <stdint.h>

// Guest media slots. A..D map to RL11 units DL0..DL3. RK0 and RP0 have
// dedicated host slots so RK/RL/RP media can be mounted independently.
enum {
  DRIVE_A = 0,   // DL0
  DRIVE_B = 1,   // DL1
  DRIVE_C = 2,   // DL2
  DRIVE_D = 3,   // DL3
  DRIVE_RK0 = 4, // RK0
  DRIVE_RP0 = 5, // RP0
  DRIVE_DU0 = 6, // DU0 (UDA50/MSCP)
  DRIVE_COUNT = 7
};

static constexpr uint32_t DISK_SIZE_TOLERANCE_PERCENT = 20;
static constexpr uint32_t DISK_RK05_IMAGE_BYTES = 2494464u;
static constexpr uint32_t DISK_RL01_IMAGE_BYTES = 5242880u;
static constexpr uint32_t DISK_RL02_IMAGE_BYTES = 10485760u;
// RP0 (Massbus) packs stay under this bound (RP06 ~174 MB).
static constexpr uint32_t DISK_RP_MAX_IMAGE_BYTES = 256u * 1024u * 1024u;
// DU0 (UDA50/MSCP) images are autosized; allow up to just under 4 GiB
// (uint32 byte offsets / FatFS 32-bit size). PiDP-style packs are often >1 GB.
static constexpr uint32_t DISK_DU_MIN_IMAGE_BYTES = 100u * 1024u;
static constexpr uint32_t DISK_DU_MAX_IMAGE_BYTES = 0xFFFFFE00u;  // 4GiB-512, 512-aligned
static constexpr uint32_t DISK_RP_MIN_IMAGE_BYTES = 100u * 1024u;

bool disk_size_within_tolerance(uint32_t bytes, uint32_t nominal);
bool disk_size_is_rk(uint32_t bytes);
bool disk_size_is_rl01(uint32_t bytes);
bool disk_size_is_rl02(uint32_t bytes);
bool disk_size_is_rl(uint32_t bytes);

// RL01/RL02 helpers used by UI / shell / mount paths (kek does not use
// the legacy rl11 controller for geometry checks).
const char* disk_rl_image_type_name(uint32_t bytes);   // "RL01" / "RL02" / "invalid"
const char* disk_rl_mounted_media_type(int slot);      // "RL01" / "RL02" / "empty" / "invalid"
bool        disk_validate_rl_mounted(int slot);        // true if slot has valid RL01/RL02 size

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
// Shared read-only disk cache. Eight 8 KB blocks are shared by every mounted
// slot and may hold different ranges of one image or ranges from several
// images. Mount/dismount and intersecting writes invalidate blocks
// automatically; a cold emulator reset uses disk_cache_invalidate_all().
void disk_cache_invalidate_all();

// Diagnostics counter (reads/writes since boot).
void disk_stats(int slot, uint32_t* reads, uint32_t* writes);

// Media-change flag: set true whenever an image is (re)mounted. Returns the
// current value and clears it.
bool disk_take_change(int slot);
