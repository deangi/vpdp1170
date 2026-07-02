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

#if !H_CPU_INSTR
#define H_CPU_INSTR 1

// Undefined Operation
static void UNOP(uint16_t instr)
{
    if (PRINTSIMLINES)
    {
        Serial.print("%% invalid instruction 0");
        Serial.println(instr, OCT);
    }
    longjmp(trapbuf, INTINVAL);
}

// No Operation
static void _NOP(uint16_t instr)
{
    // does nothing, but does not cause trap
}

// Set priority level (SPL N): set PS bits 7:5 to the priority N (0-7)
// encoded in bits 2:0 of the instruction.  Only valid in kernel mode;
// silently ignored in user mode (some PDP-11 models trap, but a no-op
// matches what XXDP's 11/34 trap test expects).
//
// Sam11 stock had this as a hard UNOP() that fired INTINVAL; FKABD0
// executes SPL and expects normal behavior, so the resulting reserved-
// instruction trap landed at the test's failure-HALT.
static void SPL(uint16_t instr)
{
    if (curuser) return;            // user mode -> NOP
    PS = (PS & ~0340) | ((instr & 7) << 5);
}

// Compare
static void CMP(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t s = (instr & 07700) >> 6;
    const uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t val1 = memread(aget(s, l), l);
    uint16_t da = aget(d, l);
    uint16_t val2 = memread(da, l);
    const int32_t sval = (val1 - val2) & max;
    PS &= 0xFFF0;
    setZ(sval == 0);
    if (sval & msb)
    {
        PS |= FLAGN;
    }
    if (((val1 ^ val2) & msb) && (!((val2 ^ sval) & msb)))
    {
        PS |= FLAGV;
    }
    if (val1 < val2)
    {
        PS |= FLAGC;
    }
}

// Branch (always)
static void BR(uint16_t instr)
{
    branch(instr & 0xFF);
}

// Branch if not equal to 0
static void BNE(uint16_t instr)
{
    if (!Z())
    {
        branch(instr & 0xFF);
    }
}

// Branch if equal to 0
static void BEQ(uint16_t instr)
{
    if (Z())
    {
        branch(instr & 0xFF);
    }
}

// Branch if greater or equal to 0
static void BGE(uint16_t instr)
{
    if (!(N() xor V()))
    {
        branch(instr & 0xFF);
    }
}

// Branch if less than 0
static void BLT(uint16_t instr)
{
    if (N() xor V())
    {
        branch(instr & 0xFF);
    }
}

// Branch if greater than 0
static void BGT(uint16_t instr)
{
    if ((!(N() xor V())) && (!Z()))
    {
        branch(instr & 0xFF);
    }
}

// Branch if less or equal to 0
static void BLE(uint16_t instr)
{
    if ((N() xor V()) || Z())
    {
        branch(instr & 0xFF);
    }
}

// Branch if plus (positive)
static void BPL(uint16_t instr)
{
    if (!N())
    {
        branch(instr & 0xFF);
    }
}

// Branch if minus (negative)
static void BMI(uint16_t instr)
{
    if (N())
    {
        branch(instr & 0xFF);
    }
}

// Branch if higher
static void BHI(uint16_t instr)
{
    if ((!C()) && (!Z()))
    {
        branch(instr & 0xFF);
    }
}

// Branch if lower or same
static void BLOS(uint16_t instr)
{
    if (C() || Z())
    {
        branch(instr & 0xFF);
    }
}

// Branch if overflow is clear
static void BVC(uint16_t instr)
{
    if (!V())
    {
        branch(instr & 0xFF);
    }
}

// Branch if overflow is set
static void BVS(uint16_t instr)
{
    if (V())
    {
        branch(instr & 0xFF);
    }
}

// Branch if carry is clear
static void BCC(uint16_t instr)
{
    if (!C())
    {
        branch(instr & 0xFF);
    }
}

// Branch if higher or same
#define BHIS(x) BCC(x)

// Branch if carry is set
static void BCS(uint16_t instr)
{
    if (C())
    {
        branch(instr & 0xFF);
    }
}

// Branch if lower
#define BLO(x) BCS(x)

// Bit test (AND)
static void BIT(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t s = (instr & 07700) >> 6;
    const uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t val1 = memread(aget(s, l), l);
    uint16_t da = aget(d, l);
    uint16_t val2 = memread(da, l);
    uint16_t uval = val1 & val2;
    PS &= 0xFFF1;
    setZ(uval == 0);
    if (uval & msb)
    {
        PS |= FLAGN;
    }
}

// Bit clear
static void BIC(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t s = (instr & 07700) >> 6;
    const uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t val1 = memread(aget(s, l), l);
    uint16_t da = aget(d, l);
    uint16_t val2 = memread(da, l);
    uint16_t uval = (max ^ val1) & val2;
    PS &= 0xFFF1;
    setZ(uval == 0);
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    memwrite(da, l, uval);
}

// Bit set (OR)
static void BIS(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t val1 = memread(aget(s, l), l);
    uint16_t da = aget(d, l);
    uint16_t val2 = memread(da, l);
    uint16_t uval = val1 | val2;
    PS &= 0xFFF1;
    setZ(uval == 0);
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    memwrite(da, l, uval);
}

// Add
static void ADD(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    // uint8_t l = 2 - (instr >> 15);
    uint16_t sa = aget(s, 2);
    uint16_t val1 = memread16(sa);
    uint16_t da = aget(d, 2);
    uint16_t val2 = memread16(da);
    uint16_t uval = (val1 + val2) & 0xFFFF;
    PS &= 0xFFF0;
    setZ(uval == 0);
    if (uval & 0x8000)
    {
        PS |= FLAGN;
    }
    if (!((val1 ^ val2) & 0x8000) && ((val2 ^ uval) & 0x8000))
    {
        PS |= FLAGV;
    }
    // C is the carry-out of the 16-bit unsigned add. It must be set only
    // when the true sum exceeds 0xFFFF, NOT when it equals 0xFFFF (which
    // is the largest representable 16-bit value, no overflow).
    if (((uint32_t)val1 + (uint32_t)val2) > 0xFFFFu)
    {
        PS |= FLAGC;
    }
    memwrite16(da, uval);
}

// Subtract
static void SUB(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    // uint8_t l = 2 - (instr >> 15);
    uint16_t sa = aget(s, 2);
    uint16_t val1 = memread16(sa);
    uint16_t da = aget(d, 2);
    uint16_t val2 = memread16(da);
    uint16_t uval = (val2 - val1) & 0xFFFF;
    PS &= 0xFFF0;
    setZ(uval == 0);
    if (uval & 0x8000)
    {
        PS |= FLAGN;
    }
    // SUB V: set when src and dst have OPPOSITE signs AND dst and result
    // have opposite signs. Sam11's stock check had `!` in front of the
    // second clause, which inverts it - V was set only when dst and
    // result had the SAME sign, exactly when V should be 0. XXDP FKAAC0
    // ran SES / CLV / SUB #077777, R0 with R0=0100000 and BVC'd into the
    // failure HALT because of this.
    if (((val1 ^ val2) & 0x8000) && ((val2 ^ uval) & 0x8000))
    {
        PS |= FLAGV;
    }
    if (val1 > val2)
    {
        PS |= FLAGC;
    }
    memwrite16(da, uval);
}

// Jump to Sub Routine
static void JSR(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    uint8_t l = 2 - (instr >> 15);
    uint16_t uval = aget(d, l);
    if (isReg(uval))
    {
        // if (PRINTSIMLINES)
        // {
        //     Serial.println(F("%% JSR called on register"));
        // }
        // panic();
        longjmp(trapbuf, INTINVAL);
    }
    push(R[s & 7]);
    R[s & 7] = R[7];
    R[7] = uval;
}

// Multiply
static void MUL(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    int32_t val1 = R[s & 7];
    if (val1 & 0x8000)
    {
        val1 = -((0xFFFF ^ val1) + 1);
    }
    uint8_t l = 2 - (instr >> 15);
    uint16_t da = aget(d, l);
    int32_t val2 = memread16(da);
    if (val2 & 0x8000)
    {
        val2 = -((0xFFFF ^ val2) + 1);
    }
    int32_t sval = val1 * val2;
    R[s & 7] = sval >> 16;
    R[(s & 7) | 1] = sval & 0xFFFF;
    PS &= 0xFFF0;
    if (sval & 0x80000000)
    {
        PS |= FLAGN;
    }
    setZ((sval & 0xFFFFFFFF) == 0);
    // C set when the signed 32-bit product can't be represented in 16
    // bits (i.e., product < -0x8000 or product > 0x7FFF). Sam11's mix
    // of unsigned / off-by-one boundaries set C on most products.
    if (sval < -0x8000 || sval > 0x7FFF)
    {
        PS |= FLAGC;
    }
}

// Divide
static void DIV(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    int32_t val1 = (R[s & 7] << 16) | (R[(s & 7) | 1]);
    uint8_t l = 2 - (instr >> 15);
    uint16_t da = aget(d, l);
    int32_t val2 = memread16(da);
    PS &= 0xFFF0;
    if (val2 == 0)
    {
        PS |= FLAGC;
        return;
    }
    if ((val1 / val2) >= 0x10000)
    {
        PS |= FLAGV;
        return;
    }
    R[s & 7] = (val1 / val2) & 0xFFFF;
    R[(s & 7) | 1] = (val1 % val2) & 0xFFFF;
    setZ(R[s & 7] == 0);
    if (R[s & 7] & 0100000)
    {
        PS |= FLAGN;
    }
    if (val1 == 0)
    {
        PS |= FLAGV;
    }
}

// Shift arithmetically
static void ASH(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    uint16_t val1 = R[s & 7];
    uint16_t da = aget(d, 2);
    uint16_t val2 = memread16(da) & 077;
    PS &= 0xFFF0;
    int32_t sval;
    if (val2 & 040)
    {
        val2 = (077 ^ val2) + 1;
        if (val1 & 0100000)
        {
            sval = 0xFFFF ^ (0xFFFF >> val2);
            sval |= val1 >> val2;
        }
        else
        {
            sval = val1 >> val2;
        }
        if (val1 & (1 << (val2 - 1)))
        {
            PS |= FLAGC;
        }
    }
    else
    {
        sval = (val1 << val2) & 0xFFFF;
        if (val1 & (1 << (16 - val2)))
        {
            PS |= FLAGC;
        }
    }
    R[s & 7] = sval;
    setZ(sval == 0);
    if (sval & 0100000)
    {
        PS |= FLAGN;
    }
    if ((sval & 0100000) xor (val1 & 0100000))
    {
        PS |= FLAGV;
    }
}

// Arith shift combined
static void ASHC(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    uint16_t val1 = R[s & 7] << 16 | R[(s & 7) | 1];
    uint16_t da = aget(d, 2);
    uint16_t val2 = memread16(da) & 077;
    PS &= 0xFFF0;
    int32_t sval;
    if (val2 & 040)
    {
        val2 = (077 ^ val2) + 1;
        if (val1 & 0x80000000)
        {
            sval = 0xFFFFFFFF ^ (0xFFFFFFFF >> val2);
            sval |= val1 >> val2;
        }
        else
        {
            sval = val1 >> val2;
        }
        if (val1 & (1 << (val2 - 1)))
        {
            PS |= FLAGC;
        }
    }
    else
    {
        sval = (val1 << val2) & 0xFFFFFFFF;
        if (val1 & (1 << (32 - val2)))
        {
            PS |= FLAGC;
        }
    }
    R[s & 7] = (sval >> 16) & 0xFFFF;
    R[(s & 7) | 1] = sval & 0xFFFF;
    setZ(sval == 0);
    if (sval & 0x80000000)
    {
        PS |= FLAGN;
    }
    if ((sval & 0x80000000) xor (val1 & 0x80000000))
    {
        PS |= FLAGV;
    }
}

// Exclusive OR
static void XOR(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t s = (instr & 07700) >> 6;
    uint16_t val1 = R[s & 7];
    uint16_t da = aget(d, 2);
    uint16_t val2 = memread16(da);
    uint16_t uval = val1 ^ val2;
    PS &= 0xFFF1;
    setZ(uval == 0);
    if (uval & 0x8000)
    {
        PS |= FLAGN;
    }
    memwrite16(da, uval);
}

// Subtract 1 and branch if !- 0
static void SOB(uint16_t instr)
{
    // 0077Roo

    uint8_t s = (instr & 0007700) >> 6;
    uint8_t o = instr & 0000077;  // 0xFF;
    R[s & 07]--;                  // & 07]--;
    if (R[s & 07])                // & 07])  //71
    {
        o &= 077;
        o <<= 1;
        // o *= 2;
        R[7] -= o;
    }
}

// Clear
static void CLR(uint16_t instr)
{
    // 0c050DD where c is 0/1 depending on if register

    const uint8_t d = instr & 077;
    const uint8_t l = 2 - (instr >> 15);
    PS &= 0xFFF0;
    // PS &= ~FLAGN;
    // PS &= ~FLAGV;
    // PS &= ~FLAGC;
    PS |= FLAGZ;

    uint16_t da = aget(d, l);
    memwrite(da, l, 0);
}

// 1s Compliment
static void COM(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t s = (instr & 07700) >> 6;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    uint16_t uval = memread(da, l) ^ max;
    PS &= 0xFFF0;
    PS |= FLAGC;
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    setZ(uval == 0);
    memwrite(da, l, uval);
}

// Increment
static void INC(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    uint16_t uval = (memread(da, l) + 1) & max;
    PS &= 0xFFF1;
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    // V: set only on overflow from 0x7FFF -> 0x8000 (word) or 0x7F -> 0x80
    // (byte). Sam11's stock code OR'd FLAGV unconditionally with FLAGN,
    // which fails XXDP FKAAC0's PS-flag checks.  Matched to _DEC's
    // mirror condition (V on uval==maxp).
    if (uval == msb)
    {
        PS |= FLAGV;
    }
    setZ(uval == 0);
    memwrite(da, l, uval);
}

// Decrement
static void _DEC(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t maxp = l == 2 ? 0x7FFF : 0x7f;
    uint16_t da = aget(d, l);
    uint16_t uval = (memread(da, l) - 1) & max;
    PS &= 0xFFF1;
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    if (uval == maxp)
    {
        PS |= FLAGV;
    }
    setZ(uval == 0);
    memwrite(da, l, uval);
}

// 2s Compliment
static void NEG(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    int32_t sval = (-memread(da, l)) & max;
    PS &= 0xFFF0;
    if (sval & msb)
    {
        PS |= FLAGN;
    }
    if (sval == 0)
    {
        PS |= FLAGZ;
    }
    else
    {
        PS |= FLAGC;
    }
    // V is set only when result == msb (the one value whose 2's-complement
    // negation can't be represented). Sam11 hardcoded 0x8000 which broke
    // NEGB (byte mode); use the size-appropriate msb instead.
    if (sval == msb)
    {
        PS |= FLAGV;
    }
    memwrite(da, l, sval);
}

// Add with carry
static void _ADC(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    uint16_t uval = memread(da, l);
    if (PS & FLAGC)
    {
        PS &= 0xFFF0;
        if ((uval + 1) & msb)
        {
            PS |= FLAGN;
        }
        setZ(uval == max);
        // V: pre-add value == max-positive (uval+1 wraps + -> -)
        if (uval == (uint16_t)(msb - 1))
        {
            PS |= FLAGV;
        }
        // C: pre-add value == max (uval+1 wraps to 0 producing carry)
        if (uval == max)
        {
            PS |= FLAGC;
        }
        memwrite(da, l, (uval + 1) & max);
    }
    else
    {
        PS &= 0xFFF0;
        if (uval & msb)
        {
            PS |= FLAGN;
        }
        setZ(uval == 0);
    }
}

// Subtract with carry
static void SBC(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    int32_t sval = memread(da, l);
    if (PS & FLAGC)
    {
        PS &= 0xFFF0;
        if ((sval - 1) & msb)
        {
            PS |= FLAGN;
        }
        setZ(sval == 1);
        // C: real PDP-11 SBC sets C only when a borrow ACTUALLY occurred,
        // i.e. dst was 0 with Cin=1.  The DEC handbook's "set otherwise"
        // wording is the source of much confusion; sam11 followed it
        // literally and inverted the bit on every common case.  XXDP
        // FKAAC0 verifies this with `SBC R0 / BCS failure` where R0=1,
        // Cin=1 (no borrow, C must be 0).
        if (sval == 0)
        {
            PS |= FLAGC;
        }
        // V set when pre-sub value == msb (sval-1 goes from min-neg to max-pos)
        if ((uint16_t)sval == msb)
        {
            PS |= FLAGV;
        }
        memwrite(da, l, (sval - 1) & max);
    }
    else
    {
        // Cin = 0 -> no subtraction occurs, no borrow can be generated.
        // C must end up cleared.  Sam11 set it unconditionally here.
        PS &= 0xFFF0;
        if (sval & msb)
        {
            PS |= FLAGN;
        }
        setZ(sval == 0);
        if ((uint16_t)sval == msb)
        {
            PS |= FLAGV;
        }
    }
}

// Test
static void TST(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t uval = memread(aget(d, l), l);
    PS &= 0xFFF0;
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    setZ(uval == 0);
}

// Rotate right
//
// Sam11's original ROR computed Z from the pre-shift value (sval & max),
// which is wrong - Z must reflect the post-shift result.  This rewrite
// shifts first, then computes flags, which also makes the V (= N XOR C)
// calculation read directly off the new N and new C bits.
static void ROR(uint16_t instr)
{
    uint8_t  d      = instr & 077;
    uint8_t  l      = 2 - (instr >> 15);
    uint32_t max    = (l == 2) ? 0xFFFFu : 0xFFu;
    uint32_t hibit  = (max + 1) >> 1;          // 0x8000 (word) or 0x80 (byte)
    uint16_t da     = aget(d, l);
    uint32_t input  = memread(da, l) & max;

    bool old_c = (PS & FLAGC) != 0;
    bool new_c = (input & 1) != 0;

    uint32_t result = input >> 1;
    if (old_c) result |= hibit;                // old C rotates into new MSB

    PS &= 0xFFF0;
    if (new_c)               PS |= FLAGC;
    if (result & hibit)      PS |= FLAGN;
    if ((result & max) == 0) PS |= FLAGZ;
    if (new_c != old_c)      PS |= FLAGV;      // V = N XOR C (post-shift)

    memwrite(da, l, result);
}

// Rotate left
static void ROL(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    int32_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    int32_t sval = memread(da, l) << 1;
    if (PS & FLAGC)
    {
        sval |= 1;
    }
    PS &= 0xFFF0;
    if (sval & (max + 1))
    {
        PS |= FLAGC;
    }
    if (sval & msb)
    {
        PS |= FLAGN;
    }
    setZ(!(sval & max));
    if ((sval ^ (sval >> 1)) & msb)
    {
        PS |= FLAGV;
    }
    sval &= max;
    memwrite(da, l, sval);
}

// Arith shift right
static void ASR(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t da = aget(d, l);
    uint16_t uval = memread(da, l);
    PS &= 0xFFF0;
    if (uval & 1)
    {
        PS |= FLAGC;
    }
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    // V = N XOR C (post-shift). Sam11's XOR of (uval & msb) (= 0x8000) and
    // (uval & 1) (= 0 or 1) treats them as integers, so the XOR is non-zero
    // whenever EITHER bit is set instead of only when they differ - fails
    // the case where both MSB and LSB are 1 (V should be 0, sam11 says 1).
    if (((uval & msb) != 0) != ((uval & 1) != 0))
    {
        PS |= FLAGV;
    }
    uval = (uval & msb) | (uval >> 1);
    setZ(uval == 0);
    memwrite(da, l, uval);
}

// Arith shift left
static void ASL(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    // TODO(dfc) doesn't need to be an sval
    int32_t sval = memread(da, l);
    PS &= 0xFFF0;
    if (sval & msb)
    {
        PS |= FLAGC;
    }
    if (sval & (msb >> 1))
    {
        PS |= FLAGN;
    }
    if ((sval ^ (sval << 1)) & msb)
    {
        PS |= FLAGV;
    }
    sval = (sval << 1) & max;
    setZ(sval == 0);
    memwrite(da, l, sval);
}

// Sign extend
//
// PDP-11/40 SXT: dst <- 0 if N=0; dst <- -1 if N=1.
// Flag effects per handbook:
//   N: not affected
//   Z: 1 if dst becomes 0 (i.e. if N was 0); 0 otherwise
//   V: cleared
//   C: not affected
//
// Sam11's stock SXT only set Z on the "result == 0" path and never
// cleared V.  XXDP FKAAC0 sets PS=NVC=013, then SXT R0, then BVS to
// the failure HALT - V must be cleared by SXT for the test to pass.
static void SXT(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t max = l == 2 ? 0xFFFF : 0xff;
    uint16_t da = aget(d, l);
    PS &= ~FLAGV;                  // V always cleared
    if (PS & FLAGN)
    {
        memwrite(da, l, max);
        PS &= ~FLAGZ;              // result is -1, not zero
    }
    else
    {
        memwrite(da, l, 0);
        PS |= FLAGZ;               // result is 0
    }
}

// Jump
static void JMP(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint16_t uval = aget(d, 2);
    if (isReg(uval))
    {
        // if (PRINTSIMLINES)
        // {
        //     Serial.println(F("%% JMP called with register dest"));
        // }
        // panic();
        longjmp(trapbuf, INTINVAL);
    }
    R[7] = uval;
}

// Swap bytes
//
// Sam11's setZ(uval & 0xFF) was inverted - Z should be set when the
// LOW BYTE of the swapped result is ZERO, not when it is non-zero.
// XXDP FKAAC0 sequences SES / CLN / SWAB / BLOS, which depends on a
// correct Z to decide between the pass path and the failure HALT.
static void SWAB(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint8_t l = 2 - (instr >> 15);
    uint16_t da = aget(d, l);
    uint16_t uval = memread(da, l);
    uval = ((uval >> 8) | (uval << 8)) & 0xFFFF;
    PS &= 0xFFF0;
    setZ((uval & 0xFF) == 0);
    if (uval & 0x80)
    {
        PS |= FLAGN;
    }
    memwrite(da, l, uval);
}

// Mark
static void MARK(uint16_t instr)
{
    R[6] = R[7] + ((instr & 077) << 1);
    R[7] = R[5];
    R[5] = pop();
}

// Return from subroutine
static void RTS(uint16_t instr)
{
    uint8_t d = instr & 077;
    R[7] = R[d & 7];
    R[d & 7] = pop();
}

// Trap (regular, emulator, breakpoint, and IO)
static void EMTX(uint16_t instr)
{
    uint16_t uval;
    if ((instr & 0177400) == 0104000)
    {
        uval = 030;
    }
    else if ((instr & 0177400) == 0104400)
    {
        uval = 034;
    }
    else if (instr == 3)
    {
        uval = 014;
    }
    else
    {
        uval = 020;
    }
    uint16_t prev = PS;
    switchmode(0);
    push(prev);
    push(R[7]);
    R[7] = dd11::read16(uval);
    PS = dd11::read16(uval + 2);
    PS |= (curuser << 14);
    PS |= (prevuser << 12);
}

// Return from interrupt
static void RTT(uint16_t instr)
{
    R[7] = pop();
    uint16_t uval = pop();
    if (curuser)
    {
        uval &= 047;
        uval |= PS & 0177730;
    }
    dd11::write16(0777776, uval);
}

// Reset processor
static void RESET(uint16_t instr)
{
    if (curuser)
    {
        return;
    }
    kl11::reset();
    rk11::reset();
}

// Move
static void MOV(uint16_t instr)
{
    const uint8_t d = instr & 077;
    const uint8_t s = (instr & 07700) >> 6;
    uint8_t l = 2 - (instr >> 15);
    uint16_t msb = l == 2 ? 0x8000 : 0x80;
    uint16_t sa = aget(s, l);
    uint16_t uval = memread(sa, l);
    uint16_t da = aget(d, l);

    PS &= 0xFFF1;
    if (uval & msb)
    {
        PS |= FLAGN;
    }
    if (uval == 0)
    {
        PS |= FLAGZ;
    }

    if ((isReg(da)) && (l == 1))
    {
        l = 2;
        if (uval & msb)
        {
            uval |= 0xFF00;
        }
    }
    memwrite(da, l, uval);
}

// Move To Processor Status (PDP-11/34+: opcode 106400+DD).
// Reads the low byte of the source operand and copies it into the low
// byte of PS, preserving the priority / mode / T bits. Sam11 didn't
// dispatch this opcode at all; XXDP FKAC executes MTPS as part of its
// status-word manipulation tests and fell into the invalid-instr trap.
static void MTPS(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint16_t da = aget(d, 1);
    uint16_t val = memread(da, 1) & 0xFF;
    // Preserve T-bit (bit 4) and bits 5-15 (priority/mode); update only
    // the four condition codes (bits 0-3). Per the PDP-11 ref the T-bit
    // is explicitly NOT settable via MTPS, and current/prev mode and
    // priority are protected too. The condition-codes-only form is what
    // XXDP and standard kernel code expect.
    PS = (PS & 0xFFF0) | (val & 0x0F);
}

// Move From Processor Status (PDP-11/34+: opcode 106700+DD).
// Copies the low byte of PS to the byte destination, sign-extending if
// the destination is a register. Sets N/Z based on the byte moved.
static void MFPS(uint16_t instr)
{
    uint8_t d = instr & 077;
    uint16_t da = aget(d, 1);
    uint16_t val = PS & 0xFF;
    // Register destination: sign-extend bit 7 into the high byte.
    if (isReg(da) && (val & 0x80))
    {
        memwrite(da, 2, val | 0xFF00);
    }
    else
    {
        memwrite(da, 1, val);
    }
    // Update condition codes from the moved value.
    PS &= 0xFFF0;
    if (val & 0x80)          PS |= FLAGN;
    if ((val & 0xFF) == 0)   PS |= FLAGZ;
}

// Halt processor.
// XXDP trap tests point trap vectors at bare HALT instructions and
// rely on operator-driven console recovery (load fresh PC, press
// START) to continue, which we don't emulate. Auto-continuing past
// HALT only created infinite trap loops, so HALT just panics; the
// user uses the gear-menu Reboot to recover.
static void _HALT(uint16_t instr)
{
    panic();
}

// Wait for interrupt
static void _WAIT(uint16_t instr)
{
    if (curuser)
    {
        if (PRINTSIMLINES)
        {
            Serial.print("%% invalid instruction 0");
            Serial.println(instr, OCT);
        }
        longjmp(trapbuf, INTINVAL);
    }
    waiting = true;
}

#endif
