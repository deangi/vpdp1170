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

// sam11 software emulation of DEC PDP-11/45 FP11 Floating Point Processor (FPP)

#include "pdp1140.h"

#if USE_FP

namespace fp11 {

enum
{
    ERRNOP = 002,    // No OP
    ERROVER = 010,   // overflow
    ERRUNDER = 012,  // underflow
}

enum
{
    signmask = 0100000,
    expbias = 0200,
    expmask = 077600,
    expshift = 07,
    hiddenmask = 0200,
    fractionmask = 0177,
    wordcount = 4,
    wordbase = 0200000,
    wordmask = 0177777,
    wordbits = 16,
}

extern uint32_t PC;        // Program Counter
extern uint16_t FEA;       // Error PC
extern uint16_t FEC;       // Error code
extern uint16_t FPS;       // Status/Error bits
extern uint8_t modelen;    // Num words in instruction: 1 = immediate, 2 = real, 4 = double
extern uint8_t precislen;  // Num words in number: 2 = real, 4 = double
extern uint32_t res[8];    // Results/Working registers (actually 7)
extern uint32_t SCR;       // Scratchpad -> 2x 16-bits to make a 32-bit number
extern uint32_t AC[6];     // Accumilator Registers

void zero(uint32_t* number);
void test(uint32_t* number);
void trap(int err);
void copy(uint32_t* number, uint32_t* operand);
void testI(uint32_t* number);
void pack(uint32_t* number, uint32_t exponent, uint8_t sign);

uint32_t aget(uint32_t instr, uint8_t l);

int modF(uint32_t* number);

int writeF(uint32_t a, uint32_t v);
uint32_t readF(uint32_t a, uint32_t v);

int write16(uint32_t a, uint16_t v);
uint16_t read16(uint32_t a);

int step(uint32_t instr);

};  // namespace fp11

#endif
