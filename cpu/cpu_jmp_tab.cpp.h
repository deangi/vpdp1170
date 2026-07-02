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

// Switch case table used inside step() function of kb11 and kd11

#if !H_CPU_JMP_TAB
#define H_CPU_JMP_TAB 1

// Double Operands
switch (instr & 0770000)
{
case 0000000:
    {
        switch (instr & 0777000)
        {
        case 0004000:  // JSR 004RDD
            JSR(instr);
            return;
        default:
            break;
        }
    }
    break;
case 0010000:  // MOV 01SSDD
    MOV(instr);
    return;
case 0020000:  // CMP 02SSDD
    CMP(instr);
    return;
case 0030000:  // BIT 03SSDD
    BIT(instr);
    return;
case 0040000:  // BIC 04SSDD
    BIC(instr);
    return;
case 0050000:  // BIS 05SSDD
    BIS(instr);
    return;
case 0060000:  // ADD 06SSDD
    ADD(instr);
    return;
case 0070000:  // EIS boards
    {
        switch (instr & 0777000)
        {
        case 0070000:  // MUL 070RSS
            MUL(instr);
            return;
        case 0071000:  // DIV 071RSS
            DIV(instr);
            return;
        case 0072000:  // ASH 073RSS
            ASH(instr);
            return;
        case 0073000:  // ASHC 073RSS
            ASHC(instr);
            return;
        case 0074000:  // XOR 074RDD
            XOR(instr);
            return;
        case 0075000:  // FIS boards -- Not implemented
            {
#if USE_FIS
                switch (instr & 0777770)
                {
                case 0075000:  // FADD 07500R
                    break;
                case 0075010:  // FSUB 07501R
                    break;
                case 0075020:  // FMUL 07502R
                    break;
                case 0075030:  // FDIV 07503R
                    break;
                default:
                    break;
                }
#endif
            }
            break;
        case 0077000:  // SOB 077RNN
            SOB(instr);
            return;
        }
    }
    break;
case 0110000:  // MOVB 11SSDD
    MOV(instr);
    return;
case 0120000:  // CMPB 12SSDD
    CMP(instr);
    return;
case 0130000:  // BITB 13SSDD
    BIT(instr);
    return;
case 0140000:  // BICB 14SSDD
    BIC(instr);
    return;
case 0150000:  // BISB 15SSDD
    BIS(instr);
    return;
case 0160000:  // SUB 16SSDD
    SUB(instr);
    return;
case 0170000:  // FP11 Instructions
    {
#if SUPRESS_UNIX_FP_NOP  // this is actually FPP/FP11... but hey...
        switch (instr)
        {
        case 0170001:  // SETF; Set floating mode
            return;
        case 0170002:  // SETI; Set integer mode
            return;
        case 0170011:  // SETD; Set double mode; not needed by UNIX, but used; therefore ignored
            return;
        case 0170012:  // SETL; Set long mode
            return;
        default:
            break;
        }
#endif
    }
    break;
default:
    break;
}

// Single operands
switch (instr & 0777700)
{
case 0000100:  // JMP 0001DD
    JMP(instr);
    return;
case 0000200:  // RTS and SPL; 00020R and 00023N
    {
        switch (instr & 0000270)
        {
        case 0000200:  // RTS 00020R
            RTS(instr);
            return;
        case 0000230:  // SPL 00023N -- Not implemented
            SPL(instr);
            return;
        default:
            {
                // Condition Codes
                if ((instr & 0177740) == 0240)
                {  // CL?, SE?
                    if (instr & 020)
                    {
                        PS |= instr & 017;
                    }
                    else
                    {
                        // Precedence bug in sam11's stock code: the
                        // original `PS &= ~instr & 017` is parsed as
                        // `PS &= ((~instr) & 017)` and clears whichever
                        // of the low four bits are NOT specified in
                        // instr.  CCC (0o257) would compute PS &= 0,
                        // wiping the priority bits along with N/Z/V/C.
                        // We want PS &= ~(instr & 017) so only the
                        // bits selected by the low 4 of instr are
                        // cleared.
                        PS &= ~(instr & 017);
                    }
                    return;
                }
            }
            break;
        }
    }
    break;
case 0000300:  // SWAB 0003DD
    SWAB(instr);
    return;
case 0000400:  // BR 0004XXX
case 0000500:
case 0000600:
case 0000700:
    BR(instr);
    return;
case 0001000:  // BNE 0010XXX
case 0001100:
case 0001200:
case 0001300:
    BNE(instr);
    return;
case 0001400:  // BEQ 0014XXX
case 0001500:
case 0001600:
case 0001700:
    BEQ(instr);
    return;
case 0002000:  // BGE 0020XXX
case 0002100:
case 0002200:
case 0002300:
    BGE(instr);
    return;
case 0002400:  // BLT 0024XXX
case 0002500:
case 0002600:
case 0002700:
    BLT(instr);
    return;
case 0003000:  // BGT 0030XXX
case 0003100:
case 0003200:
case 0003300:
    BGT(instr);
    return;
case 0003400:  // BLE 0034XXX
case 0003500:
case 0003600:
case 0003700:
    BLE(instr);
    return;
case 0005000:  // CLR 0050DD
    CLR(instr);
    return;
case 0005100:  // COM 0051DD
    COM(instr);
    return;
case 0005200:  // INC 0052DD
    INC(instr);
    return;
case 0005300:  // DEC 0053DD
    _DEC(instr);
    return;
case 0005400:  // NEG 0054DD
    NEG(instr);
    return;
case 0005500:  // ADC 0055DD
    _ADC(instr);
    return;
case 0005600:  // SBC 0056DD
    SBC(instr);
    return;
case 0005700:  // TST 0057DD
    TST(instr);
    return;
case 0006000:  // ROR 0060DD
    ROR(instr);
    return;
case 0006100:  // ROL 0061DD
    ROL(instr);
    return;
case 0006200:  // ASR 0062DD
    ASR(instr);
    return;
case 0006300:  // ASL 0063DD
    ASL(instr);
    return;
case 0006400:  // MARK 0064DD
    MARK(instr);
    return;       // sam11 stock had `break` here, which dropped through
                  //  to the trailing longjmp(INTINVAL) at end of step(),
                  //  causing every MARK to fault and land in the trap
                  //  handler instead of returning via R5.
case 0006500:  // MFPI 0065DD
    MFPI(instr);
    return;
case 0006600:  // MTPI 0066DD
    MTPI(instr);
    return;
case 0006700:  // SXT 0067DD
    SXT(instr);
    return;
case 0100000:  // BPL 1000XXX
case 0100100:
case 0100200:
case 0100300:
    BPL(instr);
    return;
case 0100400:  // BMI 1004XXX
case 0100500:
case 0100600:
case 0100700:
    BMI(instr);
    return;
case 0101000:  // BHI 1010XXX
case 0101100:
case 0101200:
case 0101300:
    BHI(instr);
    return;
case 0101400:  // BLOS 1014XXX
case 0101500:
case 0101600:
case 0101700:
    BLOS(instr);
    return;
case 0102000:  // BVC 1020XXX
case 0102100:
case 0102200:
case 0102300:
    BVC(instr);
    return;
case 0102400:  // BVS 1024XXX
case 0102500:
case 0102600:
case 0102700:
    BVS(instr);
    return;
case 0103000:  // BCC/BHIS 1030XXX
case 0103100:
case 0103200:
case 0103300:
    BCC(instr);
    return;
case 0103400:  // BCS/BLO 1034XXX
case 0103500:
case 0103600:
case 0103700:
    BCS(instr);
    return;
case 0104000:  // EMT 104000 - 104377
case 0104100:
case 0104200:
case 0104300:
    EMTX(instr);
    return;
case 0104400:  // TRAP 104400 - 104777
case 0104500:
case 0104600:
case 0104700:
    EMTX(instr);
    return;
case 0105000:  // CLRB 1050DD
    CLR(instr);
    return;
case 0105100:  // COMB 1051DD
    COM(instr);
    return;
case 0105200:  // INCB 1052DD
    INC(instr);
    return;
case 0105300:  // DECB 1053DD
    _DEC(instr);
    return;
case 0105400:  // NEGB 1054DD
    NEG(instr);
    return;
case 0105500:  // ADCB 1055DD
    _ADC(instr);
    return;
case 0105600:  // SBCB 1056DD
    SBC(instr);
    return;
case 0105700:  // TSTB 1057DD
    TST(instr);
    return;
case 0106000:  // RORB 1060DD
    ROR(instr);
    return;
case 0106100:  // ROLB 1061DD
    ROL(instr);
    return;
case 0106200:  // ASRB 1062DD
    ASR(instr);
    return;
case 0106300:  // ASLB 1063DD
    ASL(instr);
    return;
case 0106400:  // MTPS 1064DD (PDP-11/34+ Move To Processor Status)
    MTPS(instr);
    return;
case 0106500:  // MFPD 1065SS
    MFPD(instr);
    return;
case 0106600:  // MTPD 1066DD
    MTPD(instr);
    return;
case 0106700:  // MFPS 1067DD (PDP-11/34+ Move From Processor Status)
    MFPS(instr);
    return;
default:
    break;
}

// No operands
switch (instr & 0777777)
{
case 0000000:  // HALT 000000
    _HALT(instr);
    return;
case 0000001:  // WAIT 000001
    _WAIT(instr);
    return;
case 0000002:  // RTI 000002
    RTT(instr);
    return;
case 0000003:  // BPT 000003
    EMTX(instr);
    return;
case 0000004:  // IOT 000004
    EMTX(instr);
    return;
case 0000005:  // RESET 000005
    RESET(instr);
    return;
case 0000006:  // RTT 000006
    RTT(instr);
    return;
case 0000007:  // MFPT 000007 / Reserved
    //     MFPT(instr);  // 11/44 only
    UNOP(instr);
    return;
case 0000240:  // NOP 000240
    _NOP(instr);
    return;
}

#endif
