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

#if !H_CPU_CORE
#define H_CPU_CORE 1

bool isReg(const uint16_t a)
{
    return (a & 0177770) == 0170000;
}

static void push(const uint16_t v)
{
    R[6] -= 2;
    kt11::record_reg_change(6, -2);
    write16(R[6], v);
}

static uint16_t pop()
{
    const uint16_t val = read16(R[6]);
    R[6] += 2;
    kt11::record_reg_change(6, 2);
    return val;
}

static void branch(int16_t o)
{
    if (o & 0x80)
    {
        o = -(((~o) + 1) & 0xFF);
    }
    o <<= 1;
    R[7] += o;
}

bool N()
{
    return (uint8_t)PS & FLAGN;
}

bool Z()
{
    return (uint8_t)PS & FLAGZ;
}

bool V()
{
    return (uint8_t)PS & FLAGV;
}

bool C()
{
    return (uint8_t)PS & FLAGC;
}

void setZ(bool b)
{
    if (b)
        PS |= FLAGZ;
}

// aget resolves the operand to a vaddress.
// if the operand is a register, an address in
// the range [0170000,0170007). This address range is
// technically a valid IO page, but dd11 doesn't map
// any addresses here, so we can safely do this.
static uint16_t aget(uint8_t v, uint8_t l)
{
    uint32_t addr = 0;  // 32-bit so that when we trim it to 16 we lose the overflow

    if (((v & 7) >= 6) || (v & 010))
    {
        l = 2;
    }
    if ((v & 070) == 000)
    {
        return 0170000 | (v & 07);
    }

    switch (v & 060)
    {
    case 000:
        v &= 007;
        addr = R[v & 07];
        break;
    case 020:
        addr = R[v & 07];
        R[v & 07] += l;
        kt11::record_reg_change(v & 07, l);
        break;
    case 040:
        R[v & 07] -= l;
        kt11::record_reg_change(v & 07, -((int8_t)l));
        addr = R[v & 07];
        break;
    case 060:
        addr = fetch16();
        addr += R[v & 07];
        break;
    }
    // addr &= 0xFFFF;

    if (v & 010)
    {
        addr = read16(addr);
        //        return addr;
    }

    return addr;
}

#endif
