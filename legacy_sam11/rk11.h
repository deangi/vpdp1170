/*
Modified BSD License

Copyright (c) 2021 Chloe Lunn

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// sam11 software emulation of DEC PDP-11/40 RK11 RK Disk Controller

/*

Device Registers:

0777400: Drive Status Register (RKDS)

.15.14.13.12.11.10.09.08.07.06.05.04.03.02.01
 --------------------------------------------
|        |  |  |  |  |  |  |  |  |  |        | 
 --------------------------------------------

Bits:


*/

#include "pdp1140.h"

// SdFat dependency dropped for vpdp1140; m3 will replace this stub with a
// disk.cpp-backed implementation, so the host's RK05 image files are
// accessed through SD_MMC, not SdFat.

namespace rk11 {

#define NUM_RK_DRIVES (4)
extern bool attached_drives[NUM_RK_DRIVES];

void reset();
void write16(uint32_t a, uint16_t v);
uint16_t read16(uint32_t a);
// Called per CPU step from cpu_pdp11.cpp's cpu_run loop. Counts down a
// deferred RKINT so the guest's WAIT instruction has time to execute
// before we deliver the disk-done IRQ.
void tick();
void media_changed(int unit, bool mounted);
};  // namespace rk11

enum
{
    RKOVR = (1 << 14),
    RKNXD = (1 << 7),
    RKNXC = (1 << 6),
    RKNXS = (1 << 5)
};
