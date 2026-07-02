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

// sam11 software emulation of DEC DD11 UNIBUS Backplane
#include "pdp1140.h"

/* DD11 - DEC UNIBUS Backplane - "MUD"
 * =================================
 * 
 * If the KD11 processor is the heart of a PDP 11/40 then the DD11 Backplane
 * is the skeleton. The backplane held all the cards that made up the entire
 * PDP 11/40 processor, device controller cards, and peripheral devices.
 * 
 * The DD11 was one of the simpler versions of backplane, offering up to 9
 * slots with the "standard" 18-bit address, 16-bit data size.
 * 
 * The UNIBUS standard was a simple asynchronous bus, with separate, parallel
 * address and data lines, along with power routing, control/handshake lines
 * and interrupts.
 * 
 * For speed, the dd11 software unit does not route any control lines, but
 * instead the units that require control or interrupts call the bus or cpu
 * units directly in a first-come-first-serve order (due to the single thread
 * linear structure of the software). 
 * 
 * As the PDP-11/40 does not have a system processor clock in the way that we 
 * would understand it currently, the processor execution speed relies on the 
 * speed and timeout of the UNIBUS.
 * 
 * Whilst there is some speed drop due to the emulated instructions, in general
 * this is also true here, with slow emulated devices slowing down processor 
 * execution speed. Specifically, SRAM and SD Card read/write speeds.
 * 
 * Each PDP-11 processor that used UNIBUS had a fixed "UNIBUS Map". This Map
 * was in fact just a list of fixed addresses where compatible controllers,
 * registers, and device cards would "sit", and if not present the bus would
 * simply fire the timeout interrupt. 
 * 
 * e.g. The RK11 disk controller always sits at address 0777400 on an 18-bit
 * address system
 * 
 * Later processors and UNIBUS standards with 22-bits worked in a similar way,
 * but with prepended addresses of 017, e.g. 017777400 for the RK11.
 * 
 * Due to the limit of 18-bit address lines, the 11/40 cpu can only access up
 * to 256KB of space, this includes all the device addresses on the UNIBUS.
 * Actual system memory sits at addresses 0->0760000, thus only 248KB.
 * 
 */

namespace dd11 {

// operations on uint32_t types are insanely expensive
union addr {
    uint8_t bytes[4];
    uint32_t value;
};

// Controls whether the V4B-compatibility probe absorbs are active. When
// true (default), reads/writes to KE11-A EAE (0o772100..0o772176) and
// TT1 (0o776500..0o776516) are silently absorbed instead of generating
// a bus error. Set false from vpdp1140.ino at boot for RSTS V7 mode.
extern bool v4b_quirks_enabled;

void set_io_trace(uint32_t count);
uint32_t io_trace_remaining();
uint16_t read8(uint32_t addr);
uint16_t read16(uint32_t addr);
void write8(uint32_t a, uint16_t v);
void write16(uint32_t a, uint16_t v);
};  // namespace dd11
