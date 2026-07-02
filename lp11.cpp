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

// sam11 software emulation of DEC PDP-11/40 LP11 Line Printer

#include "lp11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "sam11_platform.h"
#include "sam11.h"

#include <Arduino.h>

#if USE_LP

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

#define LP_THROTTLE 500

namespace lp11 {

uint16_t LPS;
uint16_t LPB;

int loop_time = 0;

void poll()
{
    if (!(LPS & 0200))
    {
        if (++loop_time > LP_THROTTLE)
        {
#ifdef LP_PRINTER  // if the platform has an LP printer defined, e.g. it could be Serial2
            LP_PRINTER.write((LPB & 0177));
#endif
            LPS |= 0200;
            if (LPS & (1 << 6))
            {
                procNS::interrupt(INTLP, 4);
            }
        }
    }
}

void write16(uint32_t a, uint16_t v)
{
    switch (a)
    {
    case DEV_LP_STATUS:
        if (v & (1 << 6))
        {
            bool was_off = (LPS & (1 << 6)) == 0;
            LPS |= (1 << 6);
            if (was_off && (LPS & 0200))
                procNS::interrupt(INTLP, 4);
        }
        else
        {
            LPS &= ~(1 << 6);
            procNS::cancelinterrupt(INTLP);
        }
        break;
    case DEV_LP_DATA:
        LPB = v & 0177;
        LPS &= 0177577;
        procNS::cancelinterrupt(INTLP);
        loop_time = 0;
        break;
    default:
        {
            if (PRINTSIMLINES)
            {
                _printf("%%%% lp11: write to invalid address 0%06o\n", a);
            }
            //panic();
        }
    }
}

uint16_t read16(uint32_t a)
{
    switch (a)
    {
    case DEV_LP_STATUS:
        return LPS;
    case DEV_LP_DATA:
        return 0;  // LPB cannot be read
    default:
        {
            if (PRINTSIMLINES)
            {
                _printf("%%%% lp11: read from invalid address 0%06o\n", a);
            }
            //panic();
        }
    }
    return 0;
}

void reset()
{
    procNS::cancelinterrupt(INTLP);
    LPS = 0200;
    LPB = 0;
    loop_time = 0;
}
};  // namespace lp11

#endif
