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

// sam11 software emulation of DEC KD11-A processor
// Mostly 11/40 KD11-A with KE11/KG11 extensions from 11/45 KB11-B

#include "kd11.h"

#if !USE_11_45 || STRICT_11_40

#include "bootrom.h"
#include "dd11.h"
#include "kl11.h"
#include "kt11.h"
#include "kw11.h"
#include "ky11.h"
#include "ms11.h"
#include "sam11_platform.h"
#include "rk11.h"
#include "sam11.h"

// (SdFat dependency dropped - our rk11.cpp stub and disk.cpp handle disk I/O)

pdp11::intr itab[ITABN];

namespace kd11 {

// signed integer registers -- change to R[2][8]
volatile int32_t R[8];  // R6 = SP, R7 = PC

volatile uint16_t PS;     // Processor Status
volatile uint16_t curPC;  // R7, address of current instruction
volatile uint16_t KSP;    // R6 (kernel), stack pointer
volatile uint16_t USP;    // R6 (user), stack pointer

volatile uint8_t curuser;   // 0: kernel, 1: illegal, 2: illegal, 3: user
volatile uint8_t prevuser;  // 0: kernel, 1: illegal, 2: illegal, 3: user

bool trapped = false;
bool cont_with = false;
bool waiting = false;

#include "./cpu/cpu_bus.cpp.h"

void reset(void)
{
    ky11::reset();
    uint16_t i;
    for (i = 0; i < 7; i++)
    {
        R[i] = 0;
    }
    for (i = 0; i < ITABN; i++)
    {
        itab[i].vec = 0;
        itab[i].pri = 0;
    }
    kt11::SLR = 0400;
    PS = 0;
    KSP = 0;
    USP = 0;
    curuser = 0;
    prevuser = 0;
    kt11::SR0 = 0;
    kt11::SR1 = 0;
    kt11::SR2 = 0;
    kt11::SR3 = 0;
    curPC = 0;
    kw11::reset();
    ms11::clear();
    for (i = 0; i < BOOT_LEN; i++)
    {
        dd11::write16(BOOT_START + (i * 2), bootrom_rk0[i]);
    }
    R[7] = BOOT_START;
    kl11::reset();
    rk11::reset();
    waiting = false;

#ifdef PIN_OUT_PROC_RUN
    digitalWrite(PIN_OUT_PROC_RUN, LED_ON);
#endif
}

#include "./cpu/cpu_core.cpp.h"

void switchmode(uint8_t newm)
{
#if STRICT_11_40
    if (newm)
        newm = 3;
#endif

    prevuser = curuser;
    curuser = newm;
    if (prevuser == 3)
    {
        USP = R[6];
    }
    else
    {
        KSP = R[6];
    }

    if (curuser == 3)
    {
#ifdef PIN_OUT_USER_MODE
        digitalWrite(PIN_OUT_USER_MODE, LED_ON);
#endif
#ifdef PIN_OUT_SUPER_MODE
        digitalWrite(PIN_OUT_SUPER_MODE, LED_OFF);
#endif
#ifdef PIN_OUT_KERNEL_MODE
        digitalWrite(PIN_OUT_KERNEL_MODE, LED_OFF);
#endif
        R[6] = USP;
    }
    else
    {
#ifdef PIN_OUT_USER_MODE
        digitalWrite(PIN_OUT_USER_MODE, LED_OFF);
#endif
#ifdef PIN_OUT_SUPER_MODE
        digitalWrite(PIN_OUT_SUPER_MODE, LED_OFF);
#endif
#ifdef PIN_OUT_KERNEL_MODE
        digitalWrite(PIN_OUT_KERNEL_MODE, LED_ON);
#endif
        R[6] = KSP;
    }
    PS &= 0007777;
    PS |= ((curuser & 03) << 14);
    PS |= ((prevuser & 03) << 12);
}

#include "./cpu/cpu_debug.cpp.h"  // includes debug functions
#include "./cpu/cpu_instr.cpp.h"  // includes the actual instruction functions

// Move from previous instr
static void MFPI(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint16_t da = aget(d, 2);
    uint16_t uval;
    if (da == 0170006)
    {
        if (curuser == prevuser)
        {
            uval = R[6];
        }
        else
        {
            if (prevuser == 3)
            {
                uval = USP;
            }
            else
            {
                uval = KSP;
            }
        }
    }
    else if (isReg(da))
    {
        // if (PRINTSIMLINES)
        // {
        //     Serial.println(F("%% invalid MFPI instruction"));
        // }
        // panic();
        longjmp(trapbuf, INTINVAL);
    }
    else
    {
        uval = dd11::read16(kt11::decode_instr((uint16_t)da, false, prevuser));
    }
    push(uval);
    PS &= 0xFFF0;
    PS |= FLAGC;
    setZ(uval == 0);
    if (uval & 0x8000)
    {
        PS |= FLAGN;
    }
}

// Move to previous instr
static void MTPI(uint16_t instr)
{
    uint32_t sa = 0;
    uint8_t d = instr & 077;
    uint16_t da = aget(d, 2);
    uint16_t uval = pop();
    if (da == 0170006)
    {
        if (curuser == prevuser)
        {
            R[6] = uval;
        }
        else if (prevuser == 3)
        {
            USP = uval;
        }
        else
        {
            KSP = uval;
        }
    }
    else if (isReg(da))
    {
        // if (PRINTSIMLINES)
        // {
        //     Serial.println(F("%% invalid MTPI instruction"));
        // }
        // panic();
        // longjmp(trapbuf, INTINVAL);
        R[da & 7] = uval;
    }
    else
    {
        sa = kt11::decode_instr(da, true, prevuser);
        dd11::write16(sa, uval);
    }
    PS &= 0xFFF0;
    // PS |= FLAGC;
    setZ(uval == 0);
    if (uval & 0x8000)
    {
        PS |= FLAGN;
    }
    return;
}

// Move from previous data (UNOP)
static void MFPD(uint16_t instr)
{
    UNOP(instr);
}

// Move to previous data (UNOP)
static void MTPD(uint16_t instr)
{
    UNOP(instr);
}

void step()
{
    if (waiting)
        return;

    debug_step();

    curPC = R[7];
    kt11::begin_instruction(curPC);
    uint16_t instr = dd11::read16(kt11::decode_instr(R[7], false, curuser));
    // return;
    R[7] += 2;

    debug_print();

// this is  a set of switch cases which jump to the functions required
#include "./cpu/cpu_jmp_tab.cpp.h"

    if (PRINTSIMLINES)
    {
        Serial.print("%% invalid instruction 0");
        Serial.println(instr, OCT);
    }
    longjmp(trapbuf, INTINVAL);
}

#include "./cpu/cpu_irq.cpp.h"

};  // namespace kd11

#endif
