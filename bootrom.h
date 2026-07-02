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

#define BOOT_START 002000
#define BOOT_LEN   (sizeof(bootrom_rk0) / sizeof(uint16_t))

#define BOOT_RK_UNIT_IDX (4)  // if we want to change to boot from RK1+ then change the value in this index to the unit number
static const uint16_t bootrom_rk0[] = {
  0042113,                   /* "KD" */
  0012706, BOOT_START,       /* MOV #boot_start, SP */
  0012700, 0000000,          /* MOV #unit, R0        ; unit number */
  0010003,                   /* MOV R0, R3 */
  0000303,                   /* SWAB R3 */
  0006303,                   /* ASL R3 */
  0006303,                   /* ASL R3 */
  0006303,                   /* ASL R3 */
  0006303,                   /* ASL R3 */
  0006303,                   /* ASL R3 */
  0012701, 0177412,          /* MOV #RKDA, R1        ; csr */
  0010311,                   /* MOV R3, (R1)         ; load da */
  0005041,                   /* CLR -(R1)            ; clear ba */
  0012741, 0177000,          /* MOV #-256.*2, -(R1)  ; load wc */
  0012741, 0000005,          /* MOV #READ+GO, -(R1)  ; read & go */
  0005002,                   /* CLR R2 */
  0005003,                   /* CLR R3 */
  0012704, BOOT_START + 020, /* MOV #START+20, R4 */
  0005005,                   /* CLR R5 */
  0105711,                   /* TSTB (R1) */
  0100376,                   /* BPL .-2 */
  0105011,                   /* CLRB (R1) */
  0005007,                   /* CLR PC */
};

#define BOOT_RL_UNIT_IDX (4)
static const uint16_t bootrom_rl0[] = {
  0042114,                   /* "LD" */
  0012706, BOOT_START,       /* MOV #boot_start, SP */
  0012700, 0000000,          /* MOV #unit, R0 */
  0010003,                   /* MOV R0, R3 */
  0000303,                   /* SWAB R3 */
  0012701, 0174400,          /* MOV #RLCS, R1        ; csr */
  0012761, 0000013, 0000004, /* MOV #13, 4(R1)       ; clr err */
  0052703, 0000004,          /* BIS #4, R3           ; unit+gstat */
  0010311,                   /* MOV R3, (R1)         ; issue cmd */
  0105711,                   /* TSTB (R1)            ; wait */
  0100376,                   /* BPL .-2 */
  0105003,                   /* CLRB R3 */
  0052703, 0000010,          /* BIS #10, R3          ; unit+rdhdr */
  0010311,                   /* MOV R3, (R1)         ; issue cmd */
  0105711,                   /* TSTB (R1)            ; wait */
  0100376,                   /* BPL .-2 */
  0016102, 0000006,          /* MOV 6(R1), R2        ; get hdr */
  0042702, 0000077,          /* BIC #77, R2          ; clr sector */
  0005202,                   /* INC R2               ; magic bit */
  0010261, 0000004,          /* MOV R2, 4(R1)        ; seek to 0 */
  0105003,                   /* CLRB R3 */
  0052703, 0000006,          /* BIS #6, R3           ; unit+seek */
  0010311,                   /* MOV R3, (R1)         ; issue cmd */
  0105711,                   /* TSTB (R1)            ; wait */
  0100376,                   /* BPL .-2 */
  0005061, 0000002,          /* CLR 2(R1)            ; clr ba */
  0005061, 0000004,          /* CLR 4(R1)            ; clr da */
  0012761, 0177000, 0000006, /* MOV #-512., 6(R1)    ; set wc */
  0105003,                   /* CLRB R3 */
  0052703, 0000014,          /* BIS #14, R3          ; unit+read */
  0010311,                   /* MOV R3, (R1)         ; issue cmd */
  0105711,                   /* TSTB (R1)            ; wait */
  0100376,                   /* BPL .-2 */
  0042711, 0000377,          /* BIC #377, (R1) */
  0005002,                   /* CLR R2 */
  0005003,                   /* CLR R3 */
  0012704, BOOT_START + 020, /* MOV #START+20, R4 */
  0005005,                   /* CLR R5 */
  0005007                    /* CLR PC */
};

#define BOOT_RP_UNIT_IDX (4)
static const uint16_t bootrom_rp0[] = {
  0042102,                   /* "BD" */
  0012706, BOOT_START,       /* mov #boot_start, sp */
  0012700, 0000000,          /* mov #unit, r0 */
  0012701, 0176700,          /* mov #RPCS1, r1 */
  0012761, 0000040, 0000010, /* mov #CS2_CLR, 10(r1) ; reset */
  0010061, 0000010,          /* mov r0, 10(r1)       ; set unit */
  0012711, 0000021,          /* mov #RIP+GO, (r1)    ; pack ack */
  0012761, 0010000, 0000032, /* mov #FMT16B, 32(r1)  ; 16b mode */
  0012761, 0177000, 0000002, /* mov #-512., 2(r1)    ; set wc */
  0005061, 0000004,          /* clr 4(r1)            ; clr ba */
  0005061, 0000006,          /* clr 6(r1)            ; clr da */
  0005061, 0000034,          /* clr 34(r1)           ; clr cyl */
  0012711, 0000071,          /* mov #READ+GO, (r1)   ; read  */
  0105711,                   /* tstb (r1)            ; wait */
  0100376,                   /* bpl .-2 */
  0005002,                   /* clr R2 */
  0005003,                   /* clr R3 */
  0012704, BOOT_START + 020, /* mov #start+020, r4 */
  0005005,                   /* clr R5 */
  0105011,                   /* clrb (r1) */
  0005007                    /* clr PC */
};

static const uint16_t bootrom_tm0[] = {
  0012700, 0172526, /* mov #172526,r0 */
  0010040,          /* mov r0,-(r0) */
  0012740, 0060003, /* mov #60003,-(r0) */
  0012700, 0172522, /* mov #172522,r0 */
  0105710,          /* tstb (r0) */
  0100376,          /* bpl -1 */
  0005000,          /* clr r0 */
  0000110           /* jmp (r0) */
};

// #define BOOT_START 001000
// #define BOOT_LEN   9

// uint16_t bootrom[] = {
//   0012700,
//   0177406,
//   0012710,
//   0177400,
//   0012740,
//   0000005,
//   0105710,
//   0100376,
//   0005007,
// };
