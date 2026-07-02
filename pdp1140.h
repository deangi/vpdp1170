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

#include <stdint.h>

/*
 * The PDP-11 series of processors use hardware and software drivers with part
 * numbers with the pattern Letter.Letter.11 E.g. DD11 or RK11
 *
 * This code follows the original numbering scheme, with the different files
 * running the emulators for different parts of the emulated hardware.
 *
 * "sam11.*" is the exception to this as it's the main glue of the emulator and
 * its code doesn't directly emulate a part of the hardware.
 *
 * The structure of the data flow, is more-or-less identical to the real PDP-11
 *
 * CPU -> Backplane -> Device -> Backplane -> CPU
 *
 * The real PDP-11 had no clock speed as it was all async, the main limit
 * For processor speed is memory and bus speed. That is also true here.
 *
 * Table of devices/controllers:
 * =============================
 *
 * Items marked 'Y' are for most purposes fully implemented
 * Items marked 'P' are partially implemented
 * Items marked '*' are implemented as part of another module
 * Items marked '+' are work in progress
 *
 * This list is not exhaustive, and there will be controllers not listed.
 *
 * Num:     I:  Description:
 * ----------------------------------------------------------------------------
 *
 * Processor and Extensions:
 *
 * KB11-A   P+  Main CPU 11/45 \_ Main processor is mostly 11/40
 * KD11-A   Y   Main CPU 11/40 /  but has a few 11/45 things
 * KE11-E   *   Extended instructions (EIS)
 * KG11     *   XOR/CRC "cagey" calculations controller
 * KJ11     *+  Stack Limit Register
 * KL11     Y   Main Console TTY Interface
 * KM11         Maintenance Device
 * KT11     Y+  Memory Management Unit (11/40 compliant ONLY)
 * KW11     *   KW11-L line clock and optional KW11-P programmable clock
 * KY11-D   +   Developer/Diagnostics Console (front panel)
 *
 * KE11-F   P*+ Floating Point Instructions Extension
 * FP11     P*+ Floating Point Coprocessor
 *
 * Coms/Bus:
 *
 * AA11         (alias for DL11 type AA?)
 * BB11         (alias for DL11 type BB?)
 * DC11         Serial (async) Line Controller
 * DD11     Y   UNIBUS Backplane
 * DH11         Serial (async) Line Controller
 * DJ11         Serial (async) Line Controller
 * DL11     +   Serial (async) Line Controller <- this is the one you add to expand the no. TTYs
 * DM11         Serial (async) Line Controller
 * DQ11         Serial (NPR sync) Line Controller
 * DR11         Parallel Controller
 * DS11         Serial (sync) Line Controller
 * DT11         Bus Switch
 * DU11         Serial (sync) Line Controller
 * DZ11         Serial (async) Line Controller
 *
 * Memory (18-bit address = 248KiB Max):
 *
 * KF11-A       Processor Core Memory (ignored if external memory exists)
 * MM11         Ferrite Core Memory
 * MS11     Y   Silicon Memory
 *
 * Storage:
 *                                                     UNIX:
 * RK11     Y   RK Hard Disk Controller (RK05)          RK
 * RK611        RK Hard Disk Controller (RK06, RK07)    HK
 * RF11         RS Disk Controller (RS11)
 * RL11     +   RL Disk Controller (RL02)               RL
 * RP11     *+  RP Disk Pack Controller (RP03, RP02)    RP
 * RH11     +   MASSBUS Disk Pack Controller (RP04/5/6) HP
 * RC11         RS Disk Controller  (RS64)
 * PC11     +   PC Punch Tape Controller (PC05)         PC
 * TC11         TU DECtape Controller (TU56)
 * TM11     +   TU/TE Magnetic Tape Controller (TU10)   MT
 * CR11         CR/CM Card Controller (aka CM11)
 * TA11         TA Cassette Tape Controller (TU60)
 *
 * Printers:
 *
 * LP11     Y   Line Printer                            LP
 *
 * Networking/Ethernet:
 *
 * DEUNA        DEC's Ethernet Interface
 * DELUA        DEC's Second Ethernet Interface
 * NI1010A      Ethernet Interface by Interlan
 */

#ifndef _H_PDP1140_
#define _H_PDP1140_

namespace pdp11 {

// List of emulated hardware to include
#define USE_11_45    false  // change this line to true to compile with an 11/45 processor (WIP)
#define STRICT_11_40 true   // When operating in 11/40 mode, restrict features to be 11/40 only and not hybrid 11/40 and 11/45 (needed for BSD), this overrides the USE_11_45 option

// If we're in strict 11/40 mode, disable 11/45 mode
#if STRICT_11_40
#undef USE_11_45
#define USE_11_45 false
#endif

#define USE_RK     true  // }
#define KL_CONSOLE true  //  }- These should always be included, and are just here for record, they don't change the code
#define KW_LKS     true  // }

#define KY_PANEL false  // The ky11 front panel will still kinda work without this, but with it changes it to run all bus functions into it, which slows down bus r/w access
#define DL_TTYS  false  // DL11 TTY Console connectors

#define USE_FP              false  // WIP - enable the FP11 Floating point  }_ These are different ways of adding floating point, they have different formats and instructions
#define USE_FIS             false  // WIP - enable the FIS Floating point   }  FIS is not actually implemented, it just disables NOP traps
#define SUPRESS_UNIX_FP_NOP true   // disable NOPs on fpp calls used by unix v6

#define USE_LP true   // enable the line printer
#define USE_PC true   // WIP - enable the punch card/tape read/write
#define USE_RP true   // RH11/RP secondary disk support
#define USE_RL true   // RL11 is integrated into DD11 and reset with the CPU
#define USE_TM false  // WIP - enable TM11 mag tape drives (e.g. TU10)

struct intr {
    uint8_t vec;
    uint8_t pri;
};
};  // namespace pdp11

// Vectors & Addresses from DEC PDP-11/40 Processor Handbook Appendix B (1972)

// Interrupt Vectors:
enum
{
    INTRFU = 0000,     // Reserved
    INTBUS = 0004,     // Bus timeout and generally other CPU/System faults
    INTFPP = 0010,     // FPP Error
    INTINVAL = 0010,   // Reserved Intruction
    INTDEBUG = 0014,   // Debugging trap
    INTIOT = 0020,     // IOT Trap
    INTPF = 0024,      // Power Fail
    INTEMT = 0030,     // EMT
    INTTRP = 0034,     // "TRAP"
    INTSS0 = 0040,     // System Software 0
    INTSS1 = 0044,     // System Software 1
    INTSS2 = 0050,     // System Software 2
    INTSS3 = 0054,     // System Software 3
    INTTTYIN = 0060,   // TTY In ("Console Reader/Keyboard")
    INTTTYOUT = 0064,  // TTY Out ("Console Puncher/Printer")
    INTPCRD = 0070,    // PC11 Reader
    INTPCPU = 0074,    // PC11 Puncher
    INTCLOCK = 0100,   // Line CLock
    INTRTC = 0104,     // Programmer RTC
    INTXYP = 0120,     // XY Plotter
    INTDR = 0124,      // DR11B parallel interface
    INTADO = 0130,     // a/d subsystem
    INTAFC = 0134,     // Analogue control subsystem
    INTAAS = 0140,     // AA11 Scope
    INTAAL = 0144,     // AA11 Light
    INTRL = 0160,      // RL11 Disk Control
    INTUR0 = 0170,     // User Reserved
    INTUR1 = 0174,     // User Reserved
    INTLP = 0200,      // Line Printer
    INTRF = 0204,      // RF11 Disk Control
    INTRC = 0210,      // RC11 Disk Control
    INTTC = 0214,      // TC11 Disk Control
    INTRK = 0220,      // RK11 Disk Control
    INTTM = 0224,      // TM11 Disk Control
    INTCR = 0230,      // CR11 Disk Control
    INTUDC = 0234,     // Digital control subsystem
    INTPIRQ = 0240,    // 11/45 PIRQ
    INTFPUERR = 0244,  // FPU Error
    INTMMUERR = 0250,  // MMU Error
    INTRP = 0254,      // RP Disk Control
    INTTA = 0260,      // TA11 Cassette Control
    // 264 unused
    INTUR2 = 0270,     // User Reserved
    INTUR3 = 0274,     // User Reserved
    INTFLOAT = 0300,   // Start of floating vectors
};

// Switch Settings:
enum
{
    INST_UNIX_SINGLEUSER = 0173030,  // this boots Unix into single user mode and keeps it there
};

// Device/Register Addresses (taken from my PDP-11 04/05/10/35/40/45 Processor Handbook, published 1975):
enum
{
    DEV_CPU_STAT = 0777776,       // CPU Status
    DEV_STACK_LIM = 0777774,      // Stack Limit Register
    DEV_PIRQ = 0777772,           // Program Interrupt Request
    DEV_MICROPROG_BRK = 0777770,  // Microprogram break

    DEV_CPU_ERROR = 0777766,    // CPU Errors
    DEV_SYS_I_D = 0777764,      // System I/D
    DEV_SYS_SIZE_UP = 0777762,  // System Size Upper
    DEV_SYS_SIZE_LO = 0777760,  // System Size Lower

    DEV_HIT_MISS = 0777752,  // Hit/Miss Register
    DEV_MAINTAIN = 0777750,  // System Maintenance Register

    DEV_CONTROL = 0777746,        // Control Register
    DEV_MEM_SYS_ERROR = 0777744,  // Memory System Error
    DEV_ERROR_ADR_HI = 0777742,   // Error Address High
    DEV_ERROR_ADR_LO = 0777740,   // Error Address Low

    DEV_CPU_USR_SP = 0777717,   // CPU User Stack Pointer (R6)
    DEV_CPU_SUP_SP = 0777716,   // CPU Super Stack Pointer (R6)
    DEV_CPU_GEN1_R5 = 0777715,  // CPU General Register (set 1) R5
    DEV_CPU_GEN1_R4 = 0777714,  // CPU General Register (set 1) R4
    DEV_CPU_GEN1_R3 = 0777713,  // CPU General Register (set 1) R3
    DEV_CPU_GEN1_R2 = 0777712,  // CPU General Register (set 1) R2
    DEV_CPU_GEN1_R1 = 0777711,  // CPU General Register (set 1) R1
    DEV_CPU_GEN1_R0 = 0777710,  // CPU General Register (set 1) R0

    DEV_CPU_KER_PC = 0777707,   // CPU (Kernel) Program Counter (R7)
    DEV_CPU_KER_SP = 0777706,   // CPU Kernel Stack Pointer (R6)
    DEV_CPU_GEN0_R5 = 0777705,  // CPU General Register (set 0) R5
    DEV_CPU_GEN0_R4 = 0777704,  // CPU General Register (set 0) R4
    DEV_CPU_GEN0_R3 = 0777703,  // CPU General Register (set 0) R3
    DEV_CPU_GEN0_R2 = 0777702,  // CPU General Register (set 0) R2
    DEV_CPU_GEN0_R1 = 0777701,  // CPU General Register (set 0) R1
    DEV_CPU_GEN0_R0 = 0777700,  // CPU General Register (set 0) R0

    DEV_USR_DAT_PAR_R7 = 0777676,  // MMU User Data PAR Register 7
    DEV_USR_DAT_PAR_R6 = 0777674,  // MMU User Data PAR Register 6
    DEV_USR_DAT_PAR_R5 = 0777672,  // MMU User Data PAR Register 5
    DEV_USR_DAT_PAR_R4 = 0777670,  // MMU User Data PAR Register 4
    DEV_USR_DAT_PAR_R3 = 0777666,  // MMU User Data PAR Register 3
    DEV_USR_DAT_PAR_R2 = 0777664,  // MMU User Data PAR Register 2
    DEV_USR_DAT_PAR_R1 = 0777662,  // MMU User Data PAR Register 1
    DEV_USR_DAT_PAR_R0 = 0777660,  // MMU User Data PAR Register 0

    DEV_USR_INS_PAR_R7 = 0777656,  // MMU User Instruction PAR Register 7
    DEV_USR_INS_PAR_R6 = 0777654,  // MMU User Instruction PAR Register 6
    DEV_USR_INS_PAR_R5 = 0777652,  // MMU User Instruction PAR Register 5
    DEV_USR_INS_PAR_R4 = 0777650,  // MMU User Instruction PAR Register 4
    DEV_USR_INS_PAR_R3 = 0777646,  // MMU User Instruction PAR Register 3
    DEV_USR_INS_PAR_R2 = 0777644,  // MMU User Instruction PAR Register 2
    DEV_USR_INS_PAR_R1 = 0777642,  // MMU User Instruction PAR Register 1
    DEV_USR_INS_PAR_R0 = 0777640,  // MMU User Instruction PAR Register 0

    DEV_USR_DAT_PDR_R7 = 0777636,  // MMU User Data PDR Register 7
    DEV_USR_DAT_PDR_R6 = 0777634,  // MMU User Data PDR Register 6
    DEV_USR_DAT_PDR_R5 = 0777632,  // MMU User Data PDR Register 5
    DEV_USR_DAT_PDR_R4 = 0777630,  // MMU User Data PDR Register 4
    DEV_USR_DAT_PDR_R3 = 0777626,  // MMU User Data PDR Register 3
    DEV_USR_DAT_PDR_R2 = 0777624,  // MMU User Data PDR Register 2
    DEV_USR_DAT_PDR_R1 = 0777622,  // MMU User Data PDR Register 1
    DEV_USR_DAT_PDR_R0 = 0777620,  // MMU User Data PDR Register 0

    DEV_USR_INS_PDR_R7 = 0777616,  // MMU User Instruction PDR Register 7
    DEV_USR_INS_PDR_R6 = 0777614,  // MMU User Instruction PDR Register 6
    DEV_USR_INS_PDR_R5 = 0777612,  // MMU User Instruction PDR Register 5
    DEV_USR_INS_PDR_R4 = 0777610,  // MMU User Instruction PDR Register 4
    DEV_USR_INS_PDR_R3 = 0777606,  // MMU User Instruction PDR Register 3
    DEV_USR_INS_PDR_R2 = 0777604,  // MMU User Instruction PDR Register 2
    DEV_USR_INS_PDR_R1 = 0777602,  // MMU User Instruction PDR Register 1
    DEV_USR_INS_PDR_R0 = 0777600,  // MMU User Instruction PDR Register 0

    DEV_MMU_SR2 = 0777576,  // MMU System Register 2
    DEV_MMU_SR1 = 0777574,  // MMU System Register 1
    DEV_MMU_SR0 = 0777572,  // MMU System Register 0

    DEV_CONSOLE_SR = 0777570,  // Console switch/display register
    DEV_CONSOLE_DR = 0777570,  // Console display register

    DEV_CONSOLE_TTY_OUT_DATA = 0777566,    // First KL/DL is Console TTY - data out
    DEV_CONSOLE_TTY_OUT_STATUS = 0777564,  // First KL/DL - status out
    DEV_CONSOLE_TTY_IN_DATA = 0777562,     // First KL/DL - data in
    DEV_CONSOLE_TTY_IN_STATUS = 0777560,   // First KL/DL - status in

    DEV_PC_PB = 0777556,  // PC11 Punch Buffer Register
    DEV_PC_PS = 0777554,  // PC11 Punch Status Register
    DEV_PC_RB = 0777552,  // PC11 Reader Buffer Reg
    DEV_PC_RS = 0777550,  // PC11 Reader Status Register

    DEV_KW_LKS = 0777546,  // Line clock status

    DEV_LP_DATA = 0777516,    // LP11 Data out
    DEV_LP_STATUS = 0777514,  // LP11 Status
    DEV_LP_NU0 = 0777512,     // LP11 (not used)
    DEV_LP_NU1 = 0777510,     // LP11 (not used)

    DEV_TA_NU0 = 0777506,  // TA11 (not used)
    DEV_TA_NU1 = 0777504,  // TA11 (not used)
    DEV_TA_DB = 0777502,   // TA11 Data Buffer
    DEV_TA_CS = 0777500,   // TA11 Control Status

    DEV_RF_ADS = 0777476,  // RF11 Address of Disk Segment
    DEV_RF_MA = 0777474,   // RF11  Maintenance Reg
    DEV_RF_DBR = 0777472,  // RF11 Data Buffer
    DEV_RF_DAE = 0777470,  // RF11 Disk Address Extension Error
    DEV_RF_DAR = 0777466,  // RF11 Disk Address Reg
    DEV_RF_CMA = 0777464,  // RF11 Current Memory Address
    DEV_RF_WC = 0777462,   // RF11 Word Count
    DEV_RF_DCS = 0777460,  // RF11 Disk Control Status

    DEV_RC_DB = 0777456,  // RC11 Data Buffer
    DEV_RC_MN = 0777454,  // RC11 Maintenance Reg
    DEV_RC_CA = 0777452,  // RC11 Current Address
    DEV_RC_WC = 0777450,  // RC11 Word Count
    DEV_RC_CS = 0777446,  // RC11 Control and Status
    DEV_RC_ER = 0777444,  // RC11 Error Status
    DEV_RC_DA = 0777442,  // RC11 Disk Address
    DEV_RC_LA = 0777440,  // RC11 Look Ahead

    // RK611 Overlaps with RC11 and RF11
    DEV_RK6_MR3 = 0777476,  // RK611 Maint Reg 3
    DEV_RK6_CS1 = 0777440,  // RK611 Control Status 1

    DEV_DT_8 = 0777436,  // DT11
    DEV_DT_7 = 0777434,
    DEV_DT_6 = 0777432,
    DEV_DT_5 = 0777430,
    DEV_DT_4 = 0777426,
    DEV_DT_3 = 0777424,
    DEV_DT_2 = 0777422,
    DEV_DT_1 = 0777420,

    DEV_RK_DB = 0777416,  // RK11 Data Buffer
    DEV_RK_MR = 0777414,  // RK11 Maintenance Register
    DEV_RK_DA = 0777412,  // RK11 Disk Address
    DEV_RK_BA = 0777410,  // RK11 Bus Address (current memory address)
    DEV_RK_WC = 0777406,  // RK11 Word Count
    DEV_RK_CS = 0777404,  // RK11 Control Status
    DEV_RK_ER = 0777402,  // RK11 Error
    DEV_RK_DS = 0777400,  // RK11 Drive Status

    DEV_TC_NU0 = 0777356,
    DEV_TC_NU1 = 0777354,
    DEV_TC_NU2 = 0777352,
    DEV_TC_DT = 0777350,  // TC11 Data
    DEV_TC_BA = 0777346,  // TC11 Bus Address
    DEV_TC_WC = 0777344,  // TC11 Word Count
    DEV_TC_CM = 0777342,  // TC11 Command
    DEV_TC_ST = 0777340,  // TC11 Control and Status

    DEV_KE_2_ASH = 0777336,  // KE11-A #2 Arithmetic Shift
    DEV_KE_2_LSH = 0777334,  // KE11-A #2 Logical Shift
    DEV_KE_2_NOR = 0777332,  // KE11-A #2 Normalise
    DEV_KE_2_SR = 0777331,   // KE11-A #2 Status Register
    DEV_KE_2_SC = 0777330,   // KE11-A #2 Step Counter
    DEV_KE_2_MUL = 0777326,  // KE11-A #2 Multiply
    DEV_KE_2_MQ = 0777324,   // KE11-A #2 Multiplier Quotient
    DEV_KE_2_AC = 0777322,   // KE11-A #2 Accumilator
    DEV_KE_2_DIV = 0777320,  // KE11-A #2 Divide

    DEV_KE_1_ASH = 0777316,  // KE11-A #1 Arithmetic Shift
    DEV_KE_1_LSH = 0777314,  // KE11-A #1 Logical Shift
    DEV_KE_1_NOR = 0777312,  // KE11-A #1 Normalise
    DEV_KE_1_SR = 0777311,   // KE11-A #1 Status Register
    DEV_KE_1_SC = 0777310,   // KE11-A #1 Step Counter
    DEV_KE_1_MUL = 0777306,  // KE11-A #1 Multiply
    DEV_KE_1_MQ = 0777304,   // KE11-A #1 Multiplier Quotient
    DEV_KE_1_AC = 0777302,   // KE11-A #1 Accumilator
    DEV_KE_1_DIV = 0777300,  // KE11-A #1 Divide

    DEV_CR_NU0 = 0777166,  // CR11/CM11 Not used
    DEV_CR_B2 = 0777164,   // CR11/CM11 Encoded Buffer
    DEV_CR_B1 = 0777162,   // CR11/CM11 Data Buffer
    DEV_CR_S = 0777160,    // CR11/CM11 Status Register

    // AD01 A/D AT 0776776 TO 0776770

    // AA11 AT 776766 TO 0776754

    DEV_RH_CS3 = 0776752,  // RH11 Control Status 3
    DEV_RH_BAE = 0776750,  // RH11 Bus Address Extensions
    DEV_RH_EC2 = 0776746,  // RH11 Error Correct 2
    DEV_RH_EC1 = 0776744,  // RH11 Error Correct 1
    DEV_RH_ER3 = 0776742,  // RH11 Error 3
    DEV_RH_ER2 = 0776740,  // RH11 Error 2
    DEV_RH_CC = 0776736,   // RH11
    DEV_RH_DC = 0776734,   // RH11 Drive Control
    DEV_RH_OF = 0776732,   // RH11 Offset
    DEV_RH_SN = 0776730,   // RH11 Serial Number
    DEV_RH_DT = 0776726,   // RH11 Drive Type
    DEV_RH_MR = 0776724,   // RH11
    DEV_RH_DB = 0776722,   // RH11
    DEV_RH_LA = 0776720,   // RH11 Look Ahead
    DEV_RH_AS = 0776716,   // RH11
    DEV_RH_ER1 = 0776714,  // RH11 Error 1
    DEV_RH_DS = 0776712,   // RH11 Disk Status
    DEV_RH_CS2 = 0776710,  // RH11 Control Status 2
    DEV_RH_DA = 0776706,   // RH11 Disk Address
    DEV_RH_BA = 0776704,   // RH11 Bus Address
    DEV_RH_WC = 0776702,   // RH11 Word Count
    DEV_RH_CS1 = 0776700,  // RH11 Control Status 1

    // NOTE: the RP11 overlaps with the RH11, only one can be used at once... except we fudge it baby ;)
    DEV_RP_SILO = 0776736,  // RP11 Silo Memory
    DEV_RP_SUCA = 0776734,  // RP11 Select Unit Cylinder Address
    DEV_RP_M3 = 0776732,    // RP11 Maintenance Register 3
    DEV_RP_M2 = 0776730,    // RP11 Maintenance Reg 2
    DEV_RP_M1 = 0776726,    // RP11 Maintenance Reg 1
    DEV_RP_DA = 0776724,    // RP11 Disk Address
    DEV_RP_CA = 0776722,    // RP11 Cylinder Address
    DEV_RP_BA = 0776720,    // RP11 Bus Address
    DEV_RP_WC = 0776716,    // RP11 Word Count
    DEV_RP_CS = 0776714,    // RP11 Control Status
    DEV_RP_ER = 0776712,    // RP11 Error Reg
    DEV_RP_DS = 0776710,    // RP11 Device Status
    DEV_RP_NU0 = 0776706,   // RP11 Not used (but responds)
    DEV_RP_NU1 = 0776704,   // RP11 Not used (but responds)
    DEV_RP_NU2 = 0776702,   // RP11 Not used (but responds)
    DEV_RP_NU3 = 0776700,   // RP11 Not used (but responds)

    DEV_DL_16_TTY_OUT_DATA = 0776676,    // KL/DL TTY Interface #16 data out
    DEV_DL_16_TTY_OUT_STATUS = 0776674,  // KL/DL TTY Interface #16 status out
    DEV_DL_16_TTY_IN_DATA = 0776672,     // KL/DL TTY Interface #16 data in
    DEV_DL_16_TTY_IN_STATUS = 0776670,   // KL/DL TTY Interface #16 status in
    DEV_DL_15_TTY_OUT_DATA = 0776666,    // KL/DL TTY Interface #15 data out
    DEV_DL_15_TTY_OUT_STATUS = 0776664,  // KL/DL TTY Interface #15 status out
    DEV_DL_15_TTY_IN_DATA = 0776662,     // KL/DL TTY Interface #15 data in
    DEV_DL_15_TTY_IN_STATUS = 0776660,   // KL/DL TTY Interface #15 status in
    // ... UNTIL
    DEV_DL_4_TTY_OUT_DATA = 0776536,    // KL/DL TTY Interface #1 data out
    DEV_DL_4_TTY_OUT_STATUS = 0776534,  // KL/DL TTY Interface #1 status out
    DEV_DL_4_TTY_IN_DATA = 0776532,     // KL/DL TTY Interface #1 data in
    DEV_DL_4_TTY_IN_STATUS = 0776530,   // KL/DL TTY Interface #1 status in
    DEV_DL_3_TTY_OUT_DATA = 0776526,    // KL/DL TTY Interface #1 data out
    DEV_DL_3_TTY_OUT_STATUS = 0776524,  // KL/DL TTY Interface #1 status out
    DEV_DL_3_TTY_IN_DATA = 0776522,     // KL/DL TTY Interface #1 data in
    DEV_DL_3_TTY_IN_STATUS = 0776520,   // KL/DL TTY Interface #1 status in
    DEV_DL_2_TTY_OUT_DATA = 0776516,    // KL/DL TTY Interface #1 data out
    DEV_DL_2_TTY_OUT_STATUS = 0776514,  // KL/DL TTY Interface #1 status out
    DEV_DL_2_TTY_IN_DATA = 0776512,     // KL/DL TTY Interface #1 data in
    DEV_DL_2_TTY_IN_STATUS = 0776510,   // KL/DL TTY Interface #1 status in
    DEV_DL_1_TTY_OUT_DATA = 0776506,    // KL/DL TTY Interface #1 data out
    DEV_DL_1_TTY_OUT_STATUS = 0776504,  // KL/DL TTY Interface #1 status out
    DEV_DL_1_TTY_IN_DATA = 0776502,     // KL/DL TTY Interface #1 data in
    DEV_DL_1_TTY_IN_STATUS = 0776500,   // KL/DL TTY Interface #1 status in

    // LOAD OF D-something-11 comm devices

    DEV_RL_BAE = 0774420,  // RL11 Bus Address Extension Register
    DEV_RL_MP = 0774406,   // RL11 Multipurpose Register
    DEV_RL_DA = 0774404,   // RL11 Disk Address
    DEV_RL_BS = 0774402,   // RL11 Bus Address
    DEV_RL_CS = 0774400,   // RL11 Control Status

    DEV_KWP = 0772546,       // KW11-P (XX)
    DEV_KWP_CNTR = 0772544,  // KW11-P Counter
    DEV_KWP_CSB = 0772542,   // KW11-P Count Set Register
    DEV_KWP_CSR = 0772540,   // KW11-P CSR

    DEV_TM_RD = 0772532,  // TM11 read lines
    DEV_TM_DB = 0772530,  // TM11 data buffer
    DEV_TM_BA = 0772526,  // TM11 memory/buffer address
    DEV_TM_BC = 0772524,  // TM11 byte record counter
    DEV_TM_CS = 0772522,  // TM11 command
    DEV_TM_ER = 0772520,  // TM11 status/error

    DEV_MMU_SR3 = 0772516,  // MMU System Status Register 3

    DEV_KER_DAT_PAR_R7 = 0772376,  // MMU Kernel Data PAR Register 7
    DEV_KER_DAT_PAR_R6 = 0772374,  // MMU Kernel Data PAR Register 6
    DEV_KER_DAT_PAR_R5 = 0772372,  // MMU Kernel Data PAR Register 5
    DEV_KER_DAT_PAR_R4 = 0772370,  // MMU Kernel Data PAR Register 4
    DEV_KER_DAT_PAR_R3 = 0772366,  // MMU Kernel Data PAR Register 3
    DEV_KER_DAT_PAR_R2 = 0772364,  // MMU Kernel Data PAR Register 2
    DEV_KER_DAT_PAR_R1 = 0772362,  // MMU Kernel Data PAR Register 1
    DEV_KER_DAT_PAR_R0 = 0772360,  // MMU Kernel Data PAR Register 0

    DEV_KER_INS_PAR_R7 = 0772356,  // MMU Kernel Instruction PAR Register 7
    DEV_KER_INS_PAR_R6 = 0772354,  // MMU Kernel Instruction PAR Register 6
    DEV_KER_INS_PAR_R5 = 0772352,  // MMU Kernel Instruction PAR Register 5
    DEV_KER_INS_PAR_R4 = 0772350,  // MMU Kernel Instruction PAR Register 4
    DEV_KER_INS_PAR_R3 = 0772346,  // MMU Kernel Instruction PAR Register 3
    DEV_KER_INS_PAR_R2 = 0772344,  // MMU Kernel Instruction PAR Register 2
    DEV_KER_INS_PAR_R1 = 0772342,  // MMU Kernel Instruction PAR Register 1
    DEV_KER_INS_PAR_R0 = 0772340,  // MMU Kernel Instruction PAR Register 0

    DEV_KER_DAT_PDR_R7 = 0772336,  // MMU Kernel Data PDR Register 7
    DEV_KER_DAT_PDR_R6 = 0772334,  // MMU Kernel Data PDR Register 6
    DEV_KER_DAT_PDR_R5 = 0772332,  // MMU Kernel Data PDR Register 5
    DEV_KER_DAT_PDR_R4 = 0772330,  // MMU Kernel Data PDR Register 4
    DEV_KER_DAT_PDR_R3 = 0772326,  // MMU Kernel Data PDR Register 3
    DEV_KER_DAT_PDR_R2 = 0772324,  // MMU Kernel Data PDR Register 2
    DEV_KER_DAT_PDR_R1 = 0772322,  // MMU Kernel Data PDR Register 1
    DEV_KER_DAT_PDR_R0 = 0772320,  // MMU Kernel Data PDR Register 0

    DEV_KER_INS_PDR_R7 = 0772316,  // MMU Kernel Instruction PDR Register 7
    DEV_KER_INS_PDR_R6 = 0772314,  // MMU Kernel Instruction PDR Register 6
    DEV_KER_INS_PDR_R5 = 0772312,  // MMU Kernel Instruction PDR Register 5
    DEV_KER_INS_PDR_R4 = 0772310,  // MMU Kernel Instruction PDR Register 4
    DEV_KER_INS_PDR_R3 = 0772306,  // MMU Kernel Instruction PDR Register 3
    DEV_KER_INS_PDR_R2 = 0772304,  // MMU Kernel Instruction PDR Register 2
    DEV_KER_INS_PDR_R1 = 0772302,  // MMU Kernel Instruction PDR Register 1
    DEV_KER_INS_PDR_R0 = 0772300,  // MMU Kernel Instruction PDR Register 0

    DEV_SUP_DAT_PAR_R7 = 0772276,  // MMU Supervisor Data PAR Register 7
    DEV_SUP_DAT_PAR_R6 = 0772274,  // MMU Supervisor Data PAR Register 6
    DEV_SUP_DAT_PAR_R5 = 0772272,  // MMU Supervisor Data PAR Register 5
    DEV_SUP_DAT_PAR_R4 = 0772270,  // MMU Supervisor Data PAR Register 4
    DEV_SUP_DAT_PAR_R3 = 0772266,  // MMU Supervisor Data PAR Register 3
    DEV_SUP_DAT_PAR_R2 = 0772264,  // MMU Supervisor Data PAR Register 2
    DEV_SUP_DAT_PAR_R1 = 0772262,  // MMU Supervisor Data PAR Register 1
    DEV_SUP_DAT_PAR_R0 = 0772260,  // MMU Supervisor Data PAR Register 0

    DEV_SUP_INS_PAR_R7 = 0772256,  // MMU Supervisor Instruction PAR Register 7
    DEV_SUP_INS_PAR_R6 = 0772254,  // MMU Supervisor Instruction PAR Register 6
    DEV_SUP_INS_PAR_R5 = 0772252,  // MMU Supervisor Instruction PAR Register 5
    DEV_SUP_INS_PAR_R4 = 0772250,  // MMU Supervisor Instruction PAR Register 4
    DEV_SUP_INS_PAR_R3 = 0772246,  // MMU Supervisor Instruction PAR Register 3
    DEV_SUP_INS_PAR_R2 = 0772244,  // MMU Supervisor Instruction PAR Register 2
    DEV_SUP_INS_PAR_R1 = 0772242,  // MMU Supervisor Instruction PAR Register 1
    DEV_SUP_INS_PAR_R0 = 0772240,  // MMU Supervisor Instruction PAR Register 0

    DEV_SUP_DAT_PDR_R7 = 0772236,  // MMU Supervisor Data PDR Register 7
    DEV_SUP_DAT_PDR_R6 = 0772234,  // MMU Supervisor Data PDR Register 6
    DEV_SUP_DAT_PDR_R5 = 0772232,  // MMU Supervisor Data PDR Register 5
    DEV_SUP_DAT_PDR_R4 = 0772230,  // MMU Supervisor Data PDR Register 4
    DEV_SUP_DAT_PDR_R3 = 0772226,  // MMU Supervisor Data PDR Register 3
    DEV_SUP_DAT_PDR_R2 = 0772224,  // MMU Supervisor Data PDR Register 2
    DEV_SUP_DAT_PDR_R1 = 0772222,  // MMU Supervisor Data PDR Register 1
    DEV_SUP_DAT_PDR_R0 = 0772220,  // MMU Supervisor Data PDR Register 0

    DEV_SUP_INS_PDR_R7 = 0772216,  // MMU Supervisor Instruction PDR Register 7
    DEV_SUP_INS_PDR_R6 = 0772214,  // MMU Supervisor Instruction PDR Register 6
    DEV_SUP_INS_PDR_R5 = 0772212,  // MMU Supervisor Instruction PDR Register 5
    DEV_SUP_INS_PDR_R4 = 0772210,  // MMU Supervisor Instruction PDR Register 4
    DEV_SUP_INS_PDR_R3 = 0772206,  // MMU Supervisor Instruction PDR Register 3
    DEV_SUP_INS_PDR_R2 = 0772204,  // MMU Supervisor Instruction PDR Register 2
    DEV_SUP_INS_PDR_R1 = 0772202,  // MMU Supervisor Instruction PDR Register 1
    DEV_SUP_INS_PDR_R0 = 0772200,  // MMU Supervisor Instruction PDR Register 0

    DEV_MEMORY = 0760000,  // Main Memory (0->0760000 (excl))
};
#endif
