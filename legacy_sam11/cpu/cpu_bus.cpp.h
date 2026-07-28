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

#if !H_CPU_BUS
#define H_CPU_BUS 1

static uint16_t read8(const uint16_t a)
{
    return dd11::read8(kt11::decode_instr(a, false, curuser));
}

static uint16_t read16(const uint16_t a)
{
    return dd11::read16(kt11::decode_instr(a, false, curuser));
}

static void write8(const uint16_t a, const uint16_t v)
{
    dd11::write8(kt11::decode_instr(a, true, curuser), v);
}

static void write16(const uint16_t a, const uint16_t v)
{
    dd11::write16(kt11::decode_instr(a, true, curuser), v);
}

static uint16_t memread16(const uint16_t a)
{
    if (isReg(a))
    {
        return R[a & 7];
    }
    return read16(a);
}

static uint16_t memread(uint16_t a, uint8_t l)
{
    if (isReg(a))
    {
        const uint8_t r = a & 7;
        if (l == 2)
        {
            return R[r];
        }
        else
        {
            return R[r] & 0xFF;
        }
    }
    if (l == 2)
    {
        return read16(a);
    }
    return read8(a);
}

static void memwrite16(const uint16_t a, const uint16_t v)
{
    if (isReg(a))
    {
        R[a & 7] = v;
    }
    else
    {
        write16(a, v);
    }
}

static void memwrite(const uint16_t a, const uint8_t l, const uint16_t v)
{
    if (isReg(a))
    {
        const uint8_t r = a & 7;
        if (l == 2)
        {
            R[r] = v;
        }
        else
        {
            R[r] &= 0xFF00;
            R[r] |= v;
        }
        return;
    }
    if (l == 2)
    {
        write16(a, v);
    }
    else
    {
        write8(a, v);
    }
}

static uint16_t fetch16()
{
    const uint16_t val = read16(R[7]);
    R[7] += 2;
    return val;
}

#endif