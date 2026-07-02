// rh11.h - RH11 / RP04-RP06 secondary disk controller.
//
// This is a pragmatic MASSBUS subset for mounting one RP-family disk as
// secondary storage. It is not a boot controller yet.

#pragma once
#include <stdint.h>

namespace rh11 {

void     reset();
void     tick();
uint16_t read16(uint32_t a);
void     write16(uint32_t a, uint16_t v);
void     media_changed(bool mounted);

}
