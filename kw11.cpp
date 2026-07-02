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

// sam11 software emulation of DEC PDP-11/40 KW11 Line Clock

#include "kw11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "pdp1140.h"
#include "sam11_platform.h"
#include "platform.h"

#define LKS_COMPROMISE 100  // factor to compromise the ticks by, 0 == disable. Higher number or disabled is more accurate date/time in OS, but slows down processor speed

#ifndef LKS_ACC
#define LKS_ACC LKS_SHIFT_TICK
#endif

#if LKS_ACC == LKS_LOW_ACC || LKS_ACC == LKS_HIGH_ACC
#include <elapsedMillis.h>
#endif

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kw11 {

uint16_t LKS;
static uint32_t s_clock_trace_count = 0;

void set_clock_trace(uint32_t count)
{
    s_clock_trace_count = count;
}

uint32_t clock_trace_remaining()
{
    return s_clock_trace_count;
}

void trace_access(const char* device, const char* operation,
                  uint32_t address, uint16_t value)
{
    if (s_clock_trace_count == 0) return;
    s_clock_trace_count--;
    LOG("CLOCK %s %s @ %06o val=%06o PC=%06o remaining=%u",
        device, operation, (unsigned)address, (unsigned)value,
        (unsigned)procNS::curPC, (unsigned)s_clock_trace_count);
}

void trace_interrupt(const char* device, const char* event,
                     uint16_t vector, uint8_t priority)
{
    if (s_clock_trace_count == 0) return;
    s_clock_trace_count--;
    LOG("CLOCK %s IRQ %s vec=%03o BR%u PC=%06o remaining=%u",
        device, event, (unsigned)vector, (unsigned)priority,
        (unsigned)procNS::curPC, (unsigned)s_clock_trace_count);
}

#if LKS_ACC == LKS_SHIFT_TICK
uint16_t time;
#define LKS_PER (16384)
#elif LKS_ACC == LKS_LOW_ACC
elapsedMillis time;
#define LKS_PER LKS_PERIOD_MS
#elif LKS_ACC == LKS_HIGH_ACC
elapsedMicros time;
#define LKS_PER LKS_PERIOD_US
#endif

void reset()
{
    LKS = 1 << 7;
    time = 0;
}

#if LKS_COMPROMISE
int loop_time = 0;
#endif

bool lks_ticked = false;

void tick()
{
#if LKS_ACC == LKS_SHIFT_TICK
    ++time;
#endif

#if LKS_COMPROMISE
    ++loop_time;
#endif

#if !LKS_COMPROMISE

    lks_ticked = !!(time >= (LKS_PER));  // normal mode, just use settings -- //tick = (time >> 8 == 1 << 6);  //0b0100000000000000, 16384, 040000, 0x4000

#else
    if (loop_time >= LKS_COMPROMISE)  // compromise, use loop ticks to tell if you should check the time ticks
    {
        loop_time = 0;
        lks_ticked = !!(time >= (LKS_PER));
    }
#endif

    if (lks_ticked)
    {
        time = 0;
        lks_ticked = false;
        LKS |= (1 << 7);
        if (LKS & (1 << 6))
        {
            trace_interrupt("KW11-L", "request", INTCLOCK, 6);
            procNS::interrupt(INTCLOCK, 6);
        }
    }
}
};  // namespace kw11
