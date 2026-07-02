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

#include "fp11.h"

#include "pdp1140.h"

#if USE_FP

#include "sam11.h"

#if USE_11_45 && !STRICT_11_40
#include "kb11.h"  // 11/45
#define procNS kb11
#else
#include "kd11.h"  // 11/40
#define procNS kd11
#endif

namespace fp11 {
uint32_t PC;        // Program Counter
uint16_t FEA;       // Error PC
uint16_t FEC;       // Error code
uint16_t FPS;       // Status/Error bits
uint8_t modelen;    // Num words in instruction: 1 = immediate, 2 = real, 4 = double
uint8_t precislen;  // Num words in number: 2 = real, 4 = double
uint32_t res[8];    // Results/Working registers (actually 7)
uint32_t SCR;       // Scratchpad -> 4x 16-bits to make a 64-bit number
uint32_t AC[6];     // Accumilator Registers

uint32_t incV(uint32_t v)
{
    return ((v & 0200000) | ((v + 2) & 0177777));
}

void zero(uint32_t* number)
{
    if (precislen == 2)
        *number &= 0xFFFF0000;
    if (precislen == 4)
        *number &= 0x00000000;
}

void test(uint32_t* number)
{
    FPS &= 0177760;
    if (!(*number & (expmask << 16)))
    {
        FPS |= 4;  // Z bit
    }
    if (*number & (signmask << 16))
    {
        FPS |= 8;  // N Bit
    }
}

void trap(int err)
{
    FPS |= 0100000;  // Set FER - floating point error
    FEC = err;
    FEA = (PC - 2) & 0177777;
    if (!(FPS & 040000))  // check interrupt enable
    {
        longjmp(trapbuf, INTFPP);  // call interrupt
    }
}

void copy(uint32_t* number, uint32_t* operand)
{
    *number = *operand;
}

void testI(uint32_t* number)
{
    FPS &= 0177760;  //   8 - N,  4 - Z,  2 - V,  1 - C
    if (*number < 0)
        FPS |= 8;  // N Bit
    if (*number == 0)
        FPS |= 4;  // Z bit
}

void pack(uint32_t* number, uint32_t exponent, uint8_t sign)
{
    int condition = 0;  //   8 - N,  4 - Z,  2 - V,  1 - C
    if (exponent <= 0)
    {
        exponent &= 0377;
        if (FPS & 02000)
        {                    // FIU - Floating interrupt on underflow
            trap(ERRUNDER);  // 10 Floating underflow
            if (!exponent)
                condition |= 4;  // Z bit
        }
        else
        {
            zero(number);
            sign = 0;
            exponent = 0;
            condition |= 4;  // Z bit
        }
    }
    else
    {
        if (exponent >= 0400)
        {
            exponent &= 0377;  // 0200;
            if (FPS & 01000)
            {                   // FIV - Floating interrupt on overflow
                trap(ERROVER);  // 8 Floating overflow
                if (!exponent)
                    condition |= 4;  // Z bit
            }
            else
            {
                zero(number);
                sign = 0;
                exponent = 0;
                condition |= 4;  // Z bit
            }
            condition |= 2;  // V bit
        }
    }
    *number = (*number & 0xFFFF0000) | (sign | (exponent << expshift) | (*number & (fractionmask << 16)));
    if (sign)
    {
        condition |= 8;  // N bit
    }
    FPS = (FPS & 0177760) | condition;
}

void LDEXP(uint32_t* number, uint32_t exponent)
{
    uint8_t sign;
    sign = !!(*number & (signmask << 16));
    *number = (*number & (fractionmask << 16)) | hiddenmask;
    if (exponent & 0100000)
    {
        exponent = exponent - 0200000;
    }
    exponent += expbias;
    pack(number, exponent, sign);
}

void CMPF(uint32_t src1, uint32_t src2)
{
    uint32_t result = 0;
    FPS &= 0177760;
    if ((src1 | src2) & (expmask << 16))  // If both exponents zero then finished!
    {
        if ((src1 ^ src2) & (signmask << 16))  // For different signs + is larger
        {
            result = 1;
        }
        else
        {                                                                            // For same sign and both not zero then need to compare fractions
            result = (src1 & ~(FPPsignMask << 16)) - (src2 & ~(FPPsignMask << 16));  // Difference exponent and initial fraction
            if (!result)
            {  // If zero compare rest
                for (int i = 1; i < precislen; i++)
                {
                    result = src1[i] - src2[i];
                    if (result)
                    {
                        break;
                    }
                }
            }
        }
    }
    if (!result)
    {
        FPS |= 4;  // Zero flag
    }
    else
    {
        if (src1 & (signmask << 16))
        {
            result = -result;
        }
        if (result < 0)
        {
            FPS |= 8;  // Negative flag
        }
    }
}

void CMP() { }

int step(uint32_t instr)
{
    uint32_t tAC, res, v;
    PC = procNS::curPC;
    tAC = (instr >> 6) & 03;
    switch (instr & 07400)
    {
    case 0000000:  // misc ops
        {
            switch (tAC)
            {
            case 0:  // group 0
                {
                    switch (instruction & 077)
                    {
                    case 0:  // 000 CFCC Copy Floating Condition Codes
                        flags();
                        break;
                    case 1:  // 001 SETF Set Floating Mode
                        FPS &= 0177577;
                        precislen = 2;  // Floating is two word precision
                        break;
                    case 2:  // 002 SETI Set Integer Mode
                        FPS &= 0177677;
                        break;
                    case 3:  // 003 LDUP - not valid on all systems
                        trap(2);
                        break;
                    case 9:  // 011 SETD Set Floating Double Mode
                        FPS |= 0200;
                        precislen = 4;  // Double is four word precision
                        break;
                    case 10:  // 012 SETL Set Long Integer Mode
                        FPS |= 0100;
                        break;
                    default:
                        trap(ERRNOP);  // Unknown opcode
                        break;
                    }
                    break;
                }
            case 1:  // load fpp status
                {
                    if ((res = read16(instr)) >= 0)
                    {
                        FPS = res & 0147777;
                        if (!(FPS & 0200))  // FD - Double precision mode
                        {
                            precislen = 2;  // Floating is two word precision
                        }
                        else
                        {
                            precislen = 4;  // Double is four word precision
                        }
                    }
                    break;
                }
            case 2:  // store fpp prog status
                {
                    write16(instr, FPS);
                    break;
                }
            case 3:  // store fec and fea
                {
                    // FEC only for general register
                    if (!(instr & 070))
                    {
                        procNS::R[instr & 07] = FEC;
                    }
                    else
                    {
                        if ((v = aget(instr, MMU_WRITE | 4)) >= 0)
                        {
                            if (write16(v, FEC) >= 0)
                            {
                                v = incv(v);
                                write16(v, FEA);
                            }
                        }
                    }
                    break;
                }
            }
            break;
        }
    case 0000400:  // Single Operand
        {
            switch (tAC)
            {
            case 0:  // CLRF - Clear Float/Double
                {
                    zero(&SCR);
                    if (writeF(instr, SCR) >= 0)
                    {
                        test(&SCR);
                    }
                    break;
                }
            case 1:  // TSTF - Test Float/Double
                {
                    if (readF(instr, SCR) >= 0)
                    {
                        test(&SCR);
                    }
                    break;
                }
            case 2:  // ABSF - Make Absolute Fl/Db
                {
                    if (readF(instr, SCR) != -1)
                    {
                        if (!(SCR & (expmask << 16)))
                        {
                            zero(&SCR);
                        }
                        else
                        {
                            SCR &= ~(signmask << 16);
                        }
                        if (modF(&SCR) >= 0)
                        {
                            test(&SCR);
                        }
                    }
                    break;
                }
            case 3:  // NEGF - Negate Fl/Db
                {
                    break;
                }
            }
            break;
        }
        // Double Operands
    case 0001000:  // MULF - Multiply Fl/Db
    case 0001400:  // MODF - Multiply and case to int
    case 0002000:  // ADDF - Add fl/db
    case 0002400:  // LDF - load fl/db
    case 0003000:  // SUBF - Subtract
    case 0003400:  // CMPF - Compare
    case 0004000:  // STF - Store
    case 0004400:  // DIVF - Divide
    case 0005000:  // STEXP - Store Exponent
    case 0005400:  // STCFI - Convert to int or long
    case 0006000:  // STCFD - Store while convert float to double
    case 0006400:  // LDEXP - Load exponent
    case 0007000:  // LDCIF - Convert int/long to fl/db
    case 0007400:  // LDCDF - Load convert fl/db to double
    default:
        trap(ERRNOP);
    }
}

};  // namespace fp11

#endif