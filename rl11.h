// rl11.h - RL11 / RL01-RL02 disk controller (vpdp1140 m3, fresh implementation).
//
// The sam11 upstream's rl11.cpp was unfinished (JS-syntax bug, references to
// undeclared globals, SdFat-based I/O) so we wrote our own from scratch
// against the DEC RL11 hardware spec, backed by disk_read/disk_write in
// disk.cpp.
//
// Drives 0..3 (RLCS bits 9:8) map to disk slots 0..3 = config dl0..dl3.
// In practice most current configs mount RL images in slots 0/1; empty slots
// come up as "not attached" so RL11 reports an error if accessed.

#include "pdp1140.h"

#ifndef H_RL11
#define H_RL11

namespace rl11 {

extern bool attached[4];

static constexpr uint32_t RL01_IMAGE_BYTES = 5242880u;
static constexpr uint32_t RL02_IMAGE_BYTES = 10485760u;

void     reset();
void     tick();
uint16_t read16(uint32_t a);
void     write16(uint32_t a, uint16_t v);
void     media_changed(int unit, bool mounted);
bool     valid_image_size(uint32_t bytes);
bool     validate_mounted_media(int unit);
const char* image_type_name(uint32_t bytes);
const char* mounted_media_type(int unit);

};

#endif
