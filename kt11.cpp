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

// sam11 software emulation of DEC PDP-11/40 KT11 Memory Management Unit (MMU)

#include "kt11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "sam11_platform.h"
#include "sam11.h"

#include <Arduino.h>

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kt11 {

uint16_t SLR;

struct page {
    uint16_t par;  // Page address register
    uint16_t pdr;  // Page descriptor register
    uint32_t addr() const
    {
        return par & 07777;  // only bits 11-0 are valid
    }
    uint16_t len() const
    {
        return ((pdr >> 8) & 0x7F);  // page length field - bits 14-8
    }
    bool read() const
    {
        return (pdr & 2) == 2;
    }
    bool write() const
    {
        return (pdr & 6) == 6;
    }
    bool ed() const  // expansion director, 0=up, 1=down
    {
        return (pdr & 8) == 8;
    }
};

page instr_pages[4][8];  //0 = kern, 1 = super, 2 = illegal, 3 = user
page data_pages[4][8];   //0 = kern, 1 = super, 2 = illegal, 3 = user
uint16_t SR0, SR1, SR2, SR3;

static constexpr uint16_t MMR0_MME = 0000001;
static constexpr uint16_t MMR0_PAGE = 0000176;
static constexpr uint16_t MMR0_RO = 0020000;
static constexpr uint16_t MMR0_PL = 0040000;
static constexpr uint16_t MMR0_NR = 0100000;
static constexpr uint16_t MMR0_FREEZE = 0160000;
#if STRICT_11_40
static constexpr uint16_t MMR0_IMPLEMENTED_1140 = 0160557;
static constexpr uint16_t MMR0_WRITABLE_1140 = 0160401;
#else
static constexpr uint16_t MMR0_IMPLEMENTED = 0177777;
static constexpr uint16_t MMR0_WRITABLE = 0171401;
#endif

static constexpr uint16_t PDR_ACF = 0000007;
static constexpr uint16_t PDR_ACS = 0000006;
static constexpr uint16_t PDR_ED = 0000010;
static constexpr uint16_t PDR_W = 0000100;
static constexpr uint16_t PDR_A = 0000200;
static constexpr uint16_t PDR_PLF = 0077400;
#if STRICT_11_40
static constexpr uint16_t PDR_IMPLEMENTED = PDR_PLF | PDR_W | PDR_ED | PDR_ACS;
#else
static constexpr uint16_t PDR_IMPLEMENTED = PDR_PLF | PDR_W | PDR_A | PDR_ED | PDR_ACF;
#endif
static constexpr uint16_t PDR_WRITE_MASK = PDR_IMPLEMENTED & ~(PDR_A | PDR_W);

static bool updates_enabled()
{
    return (SR0 & MMR0_FREEZE) == 0;
}

void begin_instruction(const uint16_t pc)
{
    if (updates_enabled())
    {
        SR1 = 0;
        SR2 = pc;
    }
}

void record_reg_change(const uint8_t reg, const int8_t delta)
{
    if (!updates_enabled() || delta == 0)
        return;

    const uint8_t entry = (((uint8_t)delta & 037) << 3) | (reg & 07);
    if ((SR1 & 0377) == 0)
        SR1 = entry;
    else if ((SR1 & 0177400) == 0)
        SR1 |= (uint16_t)entry << 8;
}

void write_sr0(const uint16_t v)
{
#if STRICT_11_40
    SR0 = ((SR0 & ~MMR0_WRITABLE_1140) |
           (v & MMR0_WRITABLE_1140)) & MMR0_IMPLEMENTED_1140;
#else
    SR0 = ((SR0 & ~MMR0_WRITABLE) |
           (v & MMR0_WRITABLE)) & MMR0_IMPLEMENTED;
#endif
}

static bool page_length_error(const page &p, const uint16_t block)
{
    return p.ed() ? block < p.len() : block > p.len();
}

static void abort_access(const uint16_t a, const uint8_t user,
                         const bool data_space, const uint16_t errors)
{
    if (updates_enabled())
    {
        const uint16_t page = (a >> 13) & 07;
        const uint16_t mode = (user & 03) << 5;
        const uint16_t space = data_space ? 0000020 : 0;
        SR0 = (SR0 & ~MMR0_PAGE) | mode | space | (page << 1);
        SR0 |= errors;
    }
    longjmp(trapbuf, INTMMUERR);
}

static uint32_t translate(const uint16_t a, const bool w, const uint8_t user,
                          const bool data_space)
{
    if (!(SR0 & MMR0_MME))
    {
        // With memory management disabled, the top 8 KiB virtual I/O page
        // is relocated to the top 8 KiB of the 18-bit UNIBUS address space.
        return a >= 0160000 ? (uint32_t)a + 0600000 : a;
    }

    const uint16_t i = a >> 13;
    const uint16_t block = (a >> 6) & 0177;
    const uint16_t disp = a & 077;
    page &p = data_space ? data_pages[user][i] : instr_pages[user][i];
    const uint16_t acf = p.pdr & PDR_ACF;
    uint16_t errors = 0;

    if (w)
    {
        if (acf == 1 || acf == 2)
            errors |= MMR0_RO;
        else if (acf != 6)
            errors |= MMR0_NR;
    }
    else if (acf != 2 && acf != 5 && acf != 6)
    {
        errors |= MMR0_NR;
    }

    if (page_length_error(p, block))
        errors |= MMR0_PL;
    if (errors)
        abort_access(a, user, data_space, errors);

    if (w)
        p.pdr |= PDR_W;

    return ((((uint32_t)p.addr() + block) << 6) + disp) & 0777777;
}

uint32_t decode_instr(const uint16_t a, const bool w, const uint8_t user)
{
    return translate(a, w, user, false);
}

uint32_t decode_data(const uint16_t a, const bool w, const uint8_t user)
{
#if STRICT_11_40
    return translate(a, w, user, false);
#else
    const uint16_t mask = user == 3 ? 1 : user == 1 ? 2 : 4;
    return translate(a, w, user, (SR3 & mask) != 0);
#endif
}

uint16_t read16(const uint32_t a)
{
    uint8_t i = ((a & 017) >> 1);

    // ~~~ Instructions Space

    if ((a >= DEV_KER_INS_PDR_R0) && (a <= DEV_KER_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, instr area\r\n", i, users_char[0]);
        return instr_pages[0][i].pdr;
    }
    if ((a >= DEV_KER_INS_PAR_R0) && (a <= DEV_KER_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, instr area\r\n", i, users_char[0]);
        return instr_pages[0][i].par;
    }

#if !STRICT_11_40
    if ((a >= DEV_SUP_INS_PDR_R0) && (a <= DEV_SUP_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, instr area\r\n", i, users_char[1]);
        return instr_pages[1][i].pdr;
    }
    if ((a >= DEV_SUP_INS_PAR_R0) && (a <= DEV_SUP_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, instr area\r\n", i, users_char[1]);
        return instr_pages[1][i].par;
    }
#endif

    if ((a >= DEV_USR_INS_PDR_R0) && (a <= DEV_USR_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, instr area\r\n", i, users_char[3]);
        return instr_pages[3][i].pdr;
    }
    if ((a >= DEV_USR_INS_PAR_R0) && (a <= DEV_USR_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, instr area\r\n", i, users_char[3]);
        return instr_pages[3][i].par;
    }

    // ~~~ Data space

#if !STRICT_11_40
    if ((a >= DEV_KER_DAT_PDR_R0) && (a <= DEV_KER_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, data area\r\n", i, users_char[0]);
        return data_pages[0][i].pdr;
    }
    if ((a >= DEV_KER_DAT_PAR_R0) && (a <= DEV_KER_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, data area\r\n", i, users_char[0]);
        return data_pages[0][i].par;
    }

#if !STRICT_11_40
    if ((a >= DEV_SUP_DAT_PDR_R0) && (a <= DEV_SUP_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, data area\r\n", i, users_char[1]);
        return data_pages[1][i].pdr;
    }
    if ((a >= DEV_SUP_DAT_PAR_R0) && (a <= DEV_SUP_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, data area\r\n", i, users_char[1]);
        return data_pages[1][i].par;
    }
#endif

    if ((a >= DEV_USR_DAT_PDR_R0) && (a <= DEV_USR_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr read: page %i, user %c, data area\r\n", i, users_char[3]);
        return data_pages[3][i].pdr;
    }
    if ((a >= DEV_USR_DAT_PAR_R0) && (a <= DEV_USR_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par read: page %i, user %c, data area\r\n", i, users_char[3]);
        return data_pages[3][i].par;
    }
#endif

    if (PRINTSIMLINES)
    {
        Serial.print(F("%% kt11::read16 invalid read from "));
        Serial.println(a, OCT);
    }
    longjmp(trapbuf, INTBUS);
}

void write16(const uint32_t a, const uint16_t v)
{
    uint8_t i = ((a & 017) >> 1);

    // ~~~ Instructions space

    if ((a >= DEV_KER_INS_PDR_R0) && (a <= DEV_KER_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, instr area\r\n", i, users_char[0]);
        instr_pages[0][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_KER_INS_PAR_R0) && (a <= DEV_KER_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, instr area\r\n", i, users_char[0]);
        instr_pages[0][i].par = v & 07777;
        instr_pages[0][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }

#if !STRICT_11_40
    if ((a >= DEV_SUP_INS_PDR_R0) && (a <= DEV_SUP_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, instr area\r\n", i, users_char[1]);
        instr_pages[1][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_SUP_INS_PAR_R0) && (a <= DEV_SUP_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, instr area\r\n", i, users_char[1]);
        instr_pages[1][i].par = v & 07777;
        instr_pages[1][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }
#endif

    if ((a >= DEV_USR_INS_PDR_R0) && (a <= DEV_USR_INS_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, instr area\r\n", i, users_char[3]);
        instr_pages[3][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_USR_INS_PAR_R0) && (a <= DEV_USR_INS_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, instr area\r\n", i, users_char[3]);
        instr_pages[3][i].par = v & 07777;
        instr_pages[3][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }

    // ~~~ Data space

#if !STRICT_11_40
    if ((a >= DEV_KER_DAT_PDR_R0) && (a <= DEV_KER_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, data area\r\n", i, users_char[0]);
        data_pages[0][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_KER_DAT_PAR_R0) && (a <= DEV_KER_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, data area\r\n", i, users_char[0]);
        data_pages[0][i].par = v & 07777;
        data_pages[0][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }

#if !STRICT_11_40
    if ((a >= DEV_SUP_DAT_PDR_R0) && (a <= DEV_SUP_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, data area\r\n", i, users_char[1]);
        data_pages[1][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_SUP_DAT_PAR_R0) && (a <= DEV_SUP_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, data area\r\n", i, users_char[1]);
        data_pages[1][i].par = v & 07777;
        data_pages[1][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }
#endif

    if ((a >= DEV_USR_DAT_PDR_R0) && (a <= DEV_USR_DAT_PDR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: pdr write: page %i, user %c, data area\r\n", i, users_char[3]);
        data_pages[3][i].pdr = v & PDR_WRITE_MASK;
        return;
    }
    if ((a >= DEV_USR_DAT_PAR_R0) && (a <= DEV_USR_DAT_PAR_R7))
    {
        if (PRINTSIMLINES && DEBUG_MMU)
            _printf("%%%% kt11: par write: page %i, user %c, data area\r\n", i, users_char[3]);
        data_pages[3][i].par = v & 07777;
        data_pages[3][i].pdr &= ~(PDR_A | PDR_W);
        return;
    }
#endif

    if (PRINTSIMLINES)
    {
        Serial.print(F("%% kt11::write16 0"));
        Serial.print(v, OCT);
        Serial.print(F(" from invalid address 0"));
        Serial.println(a, OCT);
    }
    longjmp(trapbuf, INTBUS);
}

};  // namespace kt11
