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

#include "dd11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "kl11.h"
#include "kt11.h"
#include "kw11.h"
#include "kwp.h"
#include "ky11.h"
#include "lp11.h"
#include "ms11.h"
#include "rk11.h"
#include "rl11.h"
#include "rh11.h"
#include "dl11_file.h"
#include "sam11.h"
#include "xmem.h"
#include "platform.h"  // for LOG, g_serial_silenced

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

#if KY_PANEL
#define readReturn res =
#else
#define readReturn return
#endif

// (SdFat dependency dropped - disk I/O is handled by our disk.cpp via SD_MMC)

namespace dd11 {

// Runtime gate for the V4B compat absorbs. Default true so V4B / V6 /
// XXDP / RT-11 work out of the box; vpdp1140.ino flips it from
// pdpconfig.ini [diag] v4b_quirks at boot.
bool v4b_quirks_enabled = true;
static uint32_t io_trace_count = 0;

static constexpr bool DD11_TRACE_EXPECTED_BUS_PROBES = false;
static uint16_t read16_impl(uint32_t a);
static void write16_impl(uint32_t a, uint16_t v);

void set_io_trace(uint32_t count)
{
    io_trace_count = count;
}

uint32_t io_trace_remaining()
{
    return io_trace_count;
}

static void trace_io(const char* operation, uint32_t address, uint16_t value)
{
    if (io_trace_count == 0 || address < MAX_RAM_ADDRESS)
        return;
    io_trace_count--;
    LOG("I/O %s @ %06o val=%06o PC=%06o remaining=%u",
        operation, (unsigned)address, (unsigned)value,
        (unsigned)procNS::curPC, (unsigned)io_trace_count);
}

static bool quiet_expected_probe(const uint32_t a)
{
    if (DD11_TRACE_EXPECTED_BUS_PROBES) {
        return false;
    }

#if STRICT_11_40
    // RT-11 sweeps the optional CPU-internal registers between the 11/40
    // register window and CPU error register. They are absent on an 11/40,
    // so the bus error is correct and expected; only suppress its log noise.
    if (a >= 0777710 && a <= 0777764) {
        return true;
    }

    // 2.11BSD probes SR3 to detect split I/D-capable MMUs. A strict 11/40
    // does not implement SR3, so the bus error is correct but expected.
    if (a == DEV_MMU_SR3) {
        return true;
    }
#else
    (void)a;
#endif

    // RT-11 repeatedly reads the CR11/CM11 status register while checking
    // whether an optional card reader is installed.
    return a == DEV_CR_S;
}

uint16_t read8(const uint32_t a)
{
#if !KY_PANEL
    // If the KY_PANEL is enabled, route everything as words
    if (a < MAX_RAM_ADDRESS)
    {
        return ms11::read8(a);
    }
#endif
    uint16_t word = read16_impl(a & ~1);
    uint16_t value = (a % 2 != 0) ? ((word >> 8) & 0xFF) : (word & 0xFF);
    trace_io("READ8 ", a, value);
    return value;
}

void write8(const uint32_t a, const uint16_t v)
{
    if (a >= MAX_RAM_ADDRESS)
        trace_io("WRITE8", a, v & 0xFF);

    // The KW11-P does not support byte writes. In particular, do not use
    // the generic read/modify/write path because reading its CSR acknowledges
    // DONE and clears the pending interrupt.
    if (a >= DEV_KWP_CSR && a <= 0772557)
        return;

#if !KY_PANEL
    // If the KY_PANEL is enabled, route everything as words
    if (a < MAX_RAM_ADDRESS)
    {
        ms11::write8(a, v);
        return;
    }
#endif
    if (a % 2 != 0)
    {
        write16_impl(a & ~1,
                     (read16_impl(a & ~1) & 0xFF) | ((v & 0xFF) << 8));
    }
    else
    {
        write16_impl(a & ~1,
                     (read16_impl(a) & 0xFF00) | (v & 0xFF));
    }
}

static void write16_impl(uint32_t a, uint16_t v)
{
    if (a % 2 != 0)
    {
        if (PRINTSIMLINES)
        {
            Serial.print(F("%% dd11: write16 0"));
            Serial.print(v, OCT);
            Serial.print(F(" to odd address 0"));
            Serial.println(a, OCT);
        }
        LOG("dd11: WRITE odd-address trap @ %06o (val=%06o, PC=%06o)",
            (unsigned)a, (unsigned)v, (unsigned)kd11::curPC);
        longjmp(trapbuf, INTBUS);
    }

#if KY_PANEL
    ky11::write16(a, v);
#endif

    if (a < MAX_RAM_ADDRESS)
    {
        ms11::write16(a, v);
        return;
    }

    switch (a)
    {
    case DEV_CPU_STAT:
        {
            int c14 = v >> 14;
            switch (c14)
            {
            case 0:
                procNS::switchmode(0);  //Kernel
                break;
#if !STRICT_11_40
            case 1:
                procNS::switchmode(1);  // Super
                break;
#endif
            case 3:
                procNS::switchmode(3);  // User
                break;
            default:
                if (PRINTSIMLINES)
                {
                    Serial.print(F("%% invalid current user mode: "));
                    Serial.println(c14, OCT);
                }
                panic();
            }
            int c12 = (v >> 12) & 3;
            switch (c12)
            {
            case 0:
                procNS::prevuser = 0;  // Kernel
                break;
#if !STRICT_11_40
            case 1:
                procNS::prevuser = 1;  // Super
                break;
#endif
            case 3:
                procNS::prevuser = 3;  // User
                break;
            default:
                if (PRINTSIMLINES)
                {
                    Serial.print(F("%% invalid previous user mode: "));
                    Serial.println(c12, OCT);
                }
                panic();
            }
            procNS::PS = v;
        }
        return;

    case DEV_CPU_KER_PC:
        procNS::curPC;
        return;

#if !STRICT_11_40 && USE_11_45
    case DEV_CPU_SUP_SP:
        {
            if (procNS::curuser == 1)
                procNS::R[6] = v;
            else
                procNS::SSP = v;
        }
        return;
#endif

    case DEV_CPU_KER_SP:
        {
            if (procNS::curuser == 0)
                procNS::R[6] = v;
            else
                procNS::KSP = v;
        }
        return;

    case DEV_CPU_USR_SP:
        {
            if (procNS::curuser == 3)
                procNS::R[6] = v;
            else
                procNS::USP = v;
        }
        return;

#if !STRICT_11_40
    case DEV_STACK_LIM:
        kt11::SLR = v | 0377;  // probs wrong
        return;
#endif

    case DEV_KW_LKS:
        kw11::trace_access("KW11-L", "WRITE16", a, v);
        kw11::LKS = v;
        return;

    // Full 8-address KW11-P window. The 4 named registers map to live
    // CSR/CSB/CNTR/unused; the trailing 4 are unused in any KW11-P spec
    // I can find but RSTS V4B's probe sweep touches them, so route them
    // to kwp::write16 too (which silently absorbs unknown offsets,
    // matching the original V4B-friendly absorb range).
    case DEV_KWP_CSR:
    case DEV_KWP_CSB:
    case DEV_KWP_CNTR:
    case DEV_KWP:
    case 0772550:
    case 0772552:
    case 0772554:
    case 0772556:
        kwp::write16(a, v);
        return;

    case DEV_MMU_SR0:
        kt11::write_sr0(v);
        return;

    case DEV_MMU_SR1:
        // MMR1 and MMR2 are maintained by the processor for instruction
        // restart. Program writes do not alter them.
        return;

    case DEV_MMU_SR2:
        return;

#if !STRICT_11_40
    case DEV_MMU_SR3:
        kt11::SR3 = v;
        return;
#endif

#if !KY_PANEL
        // If the KY_PANEL is enabled, then it already got this info, so don't sent it again
    case DEV_CONSOLE_DR:
        ky11::write16(a, v);
        return;
#endif

#if USE_LP
    case DEV_LP_DATA:
    case DEV_LP_STATUS:
        lp11::write16(a, v);
        return;
#endif

    case DEV_RK_DS:
    case DEV_RK_ER:
    case DEV_RK_CS:
    case DEV_RK_WC:
    case DEV_RK_BA:
    case DEV_RK_DA:
    case DEV_RK_DB:
    case DEV_RK_MR:
        rk11::write16(a, v);
        return;

    case DEV_RL_CS:
    case DEV_RL_BS:
    case DEV_RL_DA:
    case DEV_RL_MP:
    case DEV_RL_BAE:
        rl11::write16(a, v);
        return;

    case DEV_RH_CS1:
    case DEV_RH_WC:
    case DEV_RH_BA:
    case DEV_RH_DA:
    case DEV_RH_CS2:
    case DEV_RH_DS:
    case DEV_RH_ER1:
    case DEV_RH_AS:
    case DEV_RH_LA:
    case DEV_RH_DB:
    case DEV_RH_MR:
    case DEV_RH_DT:
    case DEV_RH_SN:
    case DEV_RH_OF:
    case DEV_RH_DC:
    case DEV_RH_CC:
    case DEV_RH_ER2:
    case DEV_RH_ER3:
    case DEV_RH_EC1:
    case DEV_RH_EC2:
    case DEV_RH_BAE:
    case DEV_RH_CS3:
        rh11::write16(a, v);
        return;

    case DEV_CONSOLE_TTY_OUT_DATA:
    case DEV_CONSOLE_TTY_OUT_STATUS:
    case DEV_CONSOLE_TTY_IN_DATA:
    case DEV_CONSOLE_TTY_IN_STATUS:
        kl11::write16(a, v);
        return;

    case DEV_DL_1_TTY_OUT_DATA:
    case DEV_DL_1_TTY_OUT_STATUS:
    case DEV_DL_1_TTY_IN_DATA:
    case DEV_DL_1_TTY_IN_STATUS:
        if (dl11_file::enabled()) {
            dl11_file::write16(a, v);
            return;
        }
        break;

    default:
        break;
    }

#if !STRICT_11_40
    // don't use switch/case for this because there would be like 112 lines of "case DEV_USR_DAT_PAR_R7:"
    if (((a & 0777700) == DEV_SUP_INS_PDR_R0) || ((a & 0777700) == DEV_USR_INS_PDR_R0) || ((a & 0777700) == DEV_KER_INS_PDR_R0))
    {
        kt11::write16(a, v);
        return;
    }
#else
    // don't use switch/case for this because there would be like 112 lines of "case DEV_USR_DAT_PAR_R7:"
    if (((a & 0777700) == DEV_KER_INS_PDR_R0) || ((a & 0777700) == DEV_USR_INS_PDR_R0))
    {
        kt11::write16(a, v);
        return;
    }
#endif

    // Device-probe absorption ranges - gated by v4b_quirks_enabled
    // ([diag] v4b_quirks in pdpconfig.ini). RSTS V4B walks a table of CSRs
    // and writes a tickle value to each; bus error = "not present".
    // V4B's bus-error handler unconditionally HALTs, so we have to
    // absorb the two known-V4B-probed ranges or V4B panics. Same
    // absorbs are safe for V6 / XXDP / RT-11.
    //
    // 0o772100..0o772176 - KE11-A EAE (computational, no vector)
    // 0o776500..0o776516 - Second DL11 console (TT1)
    //
    // For RSTS V7 the TT1 absorb backfires: V7 sees a phantom DL11,
    // allocates it a floating vector, and critical devices (RK, RL)
    // collide on the next vector and get disabled. Setting
    // v4b_quirks=false in pdpconfig.ini reverts to honest bus errors
    // here so V7 can identify absent devices correctly.
    //
    // The broader floating-CSR pools (0o770500..0o770776 and
    // 0o775200..0o775776) are never absorbed - RSTS V7's device
    // count depends on those bus-erroring as "absent".
    //
    // NOTE: 0o772540..0o772546 (KW11-P) is now a real device; see kwp.cpp.
    if (v4b_quirks_enabled &&
        ((a >= 0772100 && a <= 0772176) ||
         (a >= 0776500 && a <= 0776516))) {
        return;
    }

    if (PRINTSIMLINES && !quiet_expected_probe(a))
    {
        Serial.print(F("%% dd11: write to invalid address 0"));
        Serial.println(a, OCT);
    }

    // Diagnostic: log the trapping address with PC so we can see what
    // V4B is probing. LOG is gated by g_serial_silenced so it'll quiet
    // down once we panic; before that, every device-probe bus error
    // prints one line. Cheap and very informative for the next probe.
    if (!quiet_expected_probe(a)) {
        LOG("dd11: WRITE bus-error trap @ %06o (val=%06o, PC=%06o)",
            (unsigned)a, (unsigned)v, (unsigned)kd11::curPC);
    }

    longjmp(trapbuf, INTBUS);
}

void write16(uint32_t a, uint16_t v)
{
    trace_io("WRITE16", a, v);
    write16_impl(a, v);
}

static uint16_t read16_impl(uint32_t a)
{
    if (a % 2 != 0)
    {
        if (PRINTSIMLINES)
        {
            Serial.print(F("%% dd11: read16 from odd address 0"));
            Serial.println(a, OCT);
        }
        LOG("dd11: READ odd-address trap @ %06o (PC=%06o)",
            (unsigned)a, (unsigned)kd11::curPC);
        longjmp(trapbuf, INTBUS);
    }

#if KY_PANEL
    int res = 0;
#endif

    if (a < MAX_RAM_ADDRESS)  // if lower than the device memory, then this is just RAM
    {
        readReturn ms11::read16(a);
    }

    switch (a)  // Switch by address, and read from virtual device as appropriate
    {
    case DEV_CPU_STAT:
        readReturn procNS::PS;
        break;

    case DEV_CPU_KER_PC:
        readReturn procNS::curPC;
        break;

#if USE_11_45
    case DEV_CPU_SUP_SP:
        {
            if (procNS::curuser == 1)
                readReturn procNS::R[6];
            else
                readReturn procNS::SSP;
        }
        break;
#endif

    case DEV_CPU_KER_SP:
        {
            if (procNS::curuser == 0)
                readReturn procNS::R[6];
            else
                readReturn procNS::KSP;
        }
        break;

    case DEV_CPU_USR_SP:
        {
            if (procNS::curuser == 3)
                readReturn procNS::R[6];
            else
                readReturn procNS::USP;
        }
        break;

#if !STRICT_11_40
    case DEV_STACK_LIM:
        readReturn kt11::SLR & 0177400;  // probs wrong
        break;
#endif

    case DEV_KW_LKS:
        kw11::trace_access("KW11-L", "READ16", a, kw11::LKS);
        readReturn kw11::LKS;
        break;

    // Full KW11-P 8-address window; see write16 above.
    case DEV_KWP_CSR:
    case DEV_KWP_CSB:
    case DEV_KWP_CNTR:
    case DEV_KWP:
    case 0772550:
    case 0772552:
    case 0772554:
    case 0772556:
        readReturn kwp::read16(a);
        break;

    case DEV_MMU_SR0:
        readReturn kt11::SR0;
        break;

    case DEV_MMU_SR1:
        readReturn kt11::SR1;
        break;

    case DEV_MMU_SR2:
        readReturn kt11::SR2;
        break;

#if !STRICT_11_40
    case DEV_MMU_SR3:
        readReturn kt11::SR3;
        break;
#endif

    case DEV_CONSOLE_SR:
        readReturn ky11::read16(a);
        break;

#if USE_LP
    case DEV_LP_DATA:
    case DEV_LP_STATUS:
        readReturn lp11::read16(a);
        break;
#endif

    case DEV_RK_DS:
    case DEV_RK_ER:
    case DEV_RK_CS:
    case DEV_RK_WC:
    case DEV_RK_BA:
    case DEV_RK_DA:
    case DEV_RK_DB:
    case DEV_RK_MR:
        readReturn rk11::read16(a);
        break;

    case DEV_RL_CS:
    case DEV_RL_BS:
    case DEV_RL_DA:
    case DEV_RL_MP:
    case DEV_RL_BAE:
        readReturn rl11::read16(a);
        break;

    case DEV_RH_CS1:
    case DEV_RH_WC:
    case DEV_RH_BA:
    case DEV_RH_DA:
    case DEV_RH_CS2:
    case DEV_RH_DS:
    case DEV_RH_ER1:
    case DEV_RH_AS:
    case DEV_RH_LA:
    case DEV_RH_DB:
    case DEV_RH_MR:
    case DEV_RH_DT:
    case DEV_RH_SN:
    case DEV_RH_OF:
    case DEV_RH_DC:
    case DEV_RH_CC:
    case DEV_RH_ER2:
    case DEV_RH_ER3:
    case DEV_RH_EC1:
    case DEV_RH_EC2:
    case DEV_RH_BAE:
    case DEV_RH_CS3:
        readReturn rh11::read16(a);
        break;

    case DEV_CONSOLE_TTY_OUT_DATA:
    case DEV_CONSOLE_TTY_OUT_STATUS:
    case DEV_CONSOLE_TTY_IN_DATA:
    case DEV_CONSOLE_TTY_IN_STATUS:
        readReturn kl11::read16(a);
        break;

    case DEV_DL_1_TTY_OUT_DATA:
    case DEV_DL_1_TTY_OUT_STATUS:
    case DEV_DL_1_TTY_IN_DATA:
    case DEV_DL_1_TTY_IN_STATUS:
        if (dl11_file::enabled()) {
            readReturn dl11_file::read16(a);
            break;
        }
        break;

    default:
        break;
    }

#if !STRICT_11_40
    // don't use switch/case for this because there would be like 112 lines of "case DEV_USR_DAT_PAR_R7:"
    if (((a & 0777700) == DEV_SUP_INS_PDR_R0) || ((a & 0777700) == DEV_USR_INS_PDR_R0) || ((a & 0777700) == DEV_KER_INS_PDR_R0))
    {
        readReturn kt11::read16(a);
    }
#else
    // don't use switch/case for this because there would be like 112 lines of "case DEV_USR_DAT_PAR_R7:"
    if (((a & 0777700) == DEV_KER_INS_PDR_R0) || ((a & 0777700) == DEV_USR_INS_PDR_R0))
    {
        readReturn kt11::read16(a);
    }
#endif

#if KY_PANEL
    // If the panel is enabled, bus access gets written to the front panel, EXCEPT the switch registers, because that would be weird, instead we just do the address there
    if (a != DEV_CONSOLE_SR)
        ky11::write16(a, res);
    else
        ky11::write16(a, 0);
    return res;
#endif

    // Device-probe absorption ranges: see write16 above.
    if (v4b_quirks_enabled &&
        ((a >= 0772100 && a <= 0772176) ||
         (a >= 0776500 && a <= 0776516))) {
        return 0;
    }

    if (PRINTSIMLINES && !quiet_expected_probe(a))
    {
        Serial.print(F("%% dd11: read from invalid address 0"));
        Serial.println(a, OCT);
    }

    // Diagnostic: see write16 above.
    if (!quiet_expected_probe(a)) {
        LOG("dd11: READ bus-error trap @ %06o (PC=%06o)",
            (unsigned)a, (unsigned)kd11::curPC);
    }

    longjmp(trapbuf, INTBUS);
}

uint16_t read16(uint32_t a)
{
    uint16_t value = read16_impl(a);
    trace_io("READ16", a, value);
    return value;
}

};  // namespace dd11
