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

// sam11 software emulation of DEC PDP-11/40 KD11-A processor
// Mostly 11/40 KD11-A with KE11/KG11 extensions from 11/45 KB11-B

// this is all kinds of wrong
#include "pdp1140.h"

#if USE_11_45 && !STRICT_11_40

#include <setjmp.h>

extern jmp_buf trapbuf;

#define ITABN 16

extern pdp11::intr itab[ITABN];

namespace kb11 {

enum
{
    FLAGN = 8,
    FLAGZ = 4,
    FLAGV = 2,
    FLAGC = 1
};

//R[2][8];
extern volatile int32_t R[8];  // R6 = SP, R7 = PC

extern volatile uint16_t curPC;    // R7
extern volatile uint16_t PS;       // Processor Status
extern volatile uint16_t USP;      // R6 (user)
extern volatile uint16_t SSP;      // R6 (Super)
extern volatile uint16_t KSP;      // R6 (kernel)
extern volatile uint8_t curuser;   // 0: kernel, 1: supervisor, 2: illegal, 3: user
extern volatile uint8_t prevuser;  // 0: kernel, 1: supervisor, 2: illegal, 3: user
extern bool trapped;

bool isReg(const uint16_t a);
void step();
void reset(void);
void switchmode(uint8_t newm);

void trapat(uint16_t vec);
void interrupt(uint8_t vec, uint8_t pri);
void cancelinterrupt(uint8_t vec);
void handleinterrupt();

bool N();
bool Z();
bool V();
bool C();

};  // namespace kb11

#endif
