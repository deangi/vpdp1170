// rh11.h - RH11 / RP04-RP06 secondary disk controller.
//
// This is a pragmatic MASSBUS subset for mounting one RP-family disk as
// RP0. Host boot installs bootrom_rp0 when boot_kind is BK_RP.

#pragma once
#include <stdint.h>

namespace rh11 {

void     reset();
void     tick();
uint16_t read16(uint32_t a);
void     write16(uint32_t a, uint16_t v);
void     media_changed(bool mounted);

}
