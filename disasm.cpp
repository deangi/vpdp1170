// sam11 debug dissassembler

#include "dd11.h"
#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "kt11.h"
#include "sam11_platform.h"
#include "sam11.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if USE_11_45
#define procNS kb11
#else
#define procNS kd11
#endif

const char* rs[] = {
  "R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC"};

typedef struct {
    uint16_t inst;
    uint16_t arg;
    const char* msg;
    uint8_t flag;
    bool b;
} D;

enum
{
    DD = 1 << 1,
    S = 1 << 2,
    RR = 1 << 3,
    O = 1 << 4,
    N = 1 << 5
};

D disamtable[] = {
  {0077700, 0005000, "CLR", DD, true},
  {0077700, 0005100, "COM", DD, true},
  {0077700, 0005200, "INC", DD, true},
  {0077700, 0005300, "DEC", DD, true},
  {0077700, 0005400, "NEG", DD, true},
  {0077700, 0005700, "TST", DD, true},
  {0077700, 0006200, "ASR", DD, true},
  {0077700, 0006300, "ASL", DD, true},
  {0077700, 0006000, "ROR", DD, true},
  {0077700, 0006100, "ROL", DD, true},
  {0177700, 0000300, "SWAB", DD, false},
  {0077700, 0005500, "ADC", DD, true},
  {0077700, 0005600, "SBC", DD, true},
  {0177700, 0006700, "SXT", DD, false},
  {0070000, 0010000, "MOV", S | DD, true},
  {0070000, 0020000, "CMP", S | DD, true},
  {0170000, 0060000, "ADD", S | DD, false},
  {0170000, 0160000, "SUB", S | DD, false},
  {0070000, 0030000, "BIT", S | DD, true},
  {0070000, 0040000, "BIC", S | DD, true},
  {0070000, 0050000, "BIS", S | DD, true},
  {0177000, 0070000, "MUL", RR | DD, false},
  {0177000, 0071000, "DIV", RR | DD, false},
  {0177000, 0072000, "ASH", RR | DD, false},
  {0177000, 0073000, "ASHC", RR | DD, false},
  {0177400, 0000400, "BR", O, false},
  {0177400, 0001000, "BNE", O, false},
  {0177400, 0001400, "BEQ", O, false},
  {0177400, 0100000, "BPL", O, false},
  {0177400, 0100400, "BMI", O, false},
  {0177400, 0101000, "BHI", O, false},
  {0177400, 0101400, "BLOS", O, false},
  {0177400, 0102000, "BVC", O, false},
  {0177400, 0102400, "BVS", O, false},
  {0177400, 0103000, "BCC", O, false},
  {0177400, 0103400, "BCS", O, false},
  {0177400, 0002000, "BGE", O, false},
  {0177400, 0002400, "BLT", O, false},
  {0177400, 0003000, "BGT", O, false},
  {0177400, 0003400, "BLE", O, false},
  {0177700, 0000100, "JMP", DD, false},
  {0177000, 0004000, "JSR", RR | DD, false},
  {0177770, 0000200, "RTS", RR, false},
  {0177777, 0006400, "MARK", 0, false},
  {0177000, 0077000, "SOB", RR | O, false},
  {0177777, 0000005, "RESET", 0, false},
  {0177700, 0106500, "MFPD (11/45)", DD, false},
  {0177700, 0106600, "MTPD (11/45)", DD, false},
  {0177700, 0006500, "MFPI", DD, false},
  {0177700, 0006600, "MTPI", DD, false},
  {0177777, 0000001, "WAIT", 0, false},
  {0177777, 0000007, "MFPT (11/44)", 0, false},
  {0177777, 0000000, "HALT", 0, false},
  {0177777, 0000002, "RTI", 0, false},
  {0177777, 0000006, "RTT", 0, false},
  {0177400, 0104000, "EMT", N, false},
  {0177400, 0104400, "TRAP", N, false},
  {0177777, 0000003, "BPT", 0, false},
  {0177777, 0000004, "IOT", 0, false},
  {0177777, 0000000, "??? Unknown", 0, false},
  {0, 0, "", 0, false},
};

void disasmaddr(uint16_t m, uint32_t a)
{
    if (m & 7)
    {
        switch (m)
        {
        case 027:
            a += 2;
            _printf("$%06o", dd11::read16(a));
            return;
        case 037:
            a += 2;
            _printf("*%06o", dd11::read16(a));
            return;
        case 067:
            a += 2;
            _printf("*%06o", (a + 2 + (dd11::read16(a))) & 0xFFFF);
            return;
        case 077:
            _printf("**%06o", (a + 2 + (dd11::read16(a))) & 0xFFFF);
            return;
        }
    }

    switch (m & 070)
    {
    case 000:
        Serial.print(rs[m & 7]);
        break;
    case 010:
        _printf("(%s)", rs[m & 7]);
        break;
    case 020:
        _printf("(%s)+", rs[m & 7]);
        break;
    case 030:
        _printf("*(%s)+", rs[m & 7]);
        break;
    case 040:
        _printf("-(%s)", rs[m & 7]);
        break;
    case 050:
        _printf("*-(%s)", rs[m & 7]);
        break;
    case 060:
        a += 2;
        _printf("%06o (%s)", dd11::read16(a), rs[m & 7]);
        break;
    case 070:
        a += 2;
        _printf("*%06o (%s)", dd11::read16(a), rs[m & 7]);
        break;
    }
}

void disasm(uint32_t a)
{
    uint16_t ins = dd11::read16(a);

    if ((ins & 0170000) == 0170000)
    {
        Serial.print(F("FP11/UNIMPL "));
        disasmaddr((ins >> 6) & 077, a);
        Serial.print(',');
        disasmaddr(ins & 077, a);
        return;
    }

    D l;
    uint8_t i;
    for (i = 0; disamtable[i].inst; i++)
    {
        l = disamtable[i];
        if ((ins & l.inst) == l.arg)
        {
            break;
        }
    }
    if (l.inst == 0)
    {
        Serial.print(F("???"));
        return;
    }
    _printf(l.msg);
    if (l.b && (ins & 0100000))
    {
        Serial.print('B');
    }
    uint16_t s = (ins & 07700) >> 6;
    uint16_t d = ins & 077;
    uint8_t o = ins & 0377;
    switch (l.flag)
    {
    case S | DD:
        Serial.print(' ');
        disasmaddr(s, a);
        Serial.print(',');
    case DD:
        Serial.print(' ');
        disasmaddr(d, a);
        break;
    case RR | O:
        Serial.print(' ');
        Serial.print(rs[(ins & 0700) >> 6]);
        Serial.print(',');
        o &= 077;
    case O:
        if (o & 0x80)
        {
            _printf(" -%03o", (2 * ((0xFF ^ o) + 1)));
        }
        else
        {
            _printf(" +%03o", (2 * o));
        };
        break;
    case RR | DD:
        Serial.print(' ');
        Serial.print(rs[(ins & 0700) >> 6]);
        Serial.print(F(", "));
        disasmaddr(d, a);
    case RR:
        Serial.print(' ');
        Serial.print(rs[ins & 7]);
    }
}

static void disasm_append(char* buffer, size_t size, const char* format, ...)
{
    size_t used = strlen(buffer);
    if (used >= size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + used, size - used, format, args);
    va_end(args);
}

static void disasm_format_operand(uint16_t operand, uint32_t& physical_cursor,
                                  uint16_t& virtual_cursor,
                                  char* buffer, size_t size)
{
    uint8_t reg = operand & 7;
    uint8_t mode = (operand >> 3) & 7;
    uint16_t extension;

    switch (mode)
    {
    case 0:
        disasm_append(buffer, size, "%s", rs[reg]);
        break;
    case 1:
        disasm_append(buffer, size, "(%s)", rs[reg]);
        break;
    case 2:
        if (reg == 7) {
            extension = dd11::read16(physical_cursor);
            physical_cursor += 2;
            virtual_cursor += 2;
            disasm_append(buffer, size, "#%06o", extension);
        } else {
            disasm_append(buffer, size, "(%s)+", rs[reg]);
        }
        break;
    case 3:
        if (reg == 7) {
            extension = dd11::read16(physical_cursor);
            physical_cursor += 2;
            virtual_cursor += 2;
            disasm_append(buffer, size, "@#%06o", extension);
        } else {
            disasm_append(buffer, size, "@(%s)+", rs[reg]);
        }
        break;
    case 4:
        disasm_append(buffer, size, "-(%s)", rs[reg]);
        break;
    case 5:
        disasm_append(buffer, size, "@-(%s)", rs[reg]);
        break;
    case 6:
        extension = dd11::read16(physical_cursor);
        physical_cursor += 2;
        virtual_cursor += 2;
        if (reg == 7) {
            uint16_t target = (uint16_t)(virtual_cursor +
                                         (int16_t)extension);
            disasm_append(buffer, size, "%06o", target);
        } else {
            disasm_append(buffer, size, "%06o(%s)", extension, rs[reg]);
        }
        break;
    case 7:
        extension = dd11::read16(physical_cursor);
        physical_cursor += 2;
        virtual_cursor += 2;
        if (reg == 7) {
            uint16_t target = (uint16_t)(virtual_cursor +
                                         (int16_t)extension);
            disasm_append(buffer, size, "@%06o", target);
        } else {
            disasm_append(buffer, size, "@%06o(%s)", extension, rs[reg]);
        }
        break;
    }
}

bool disasm_format(uint32_t address, uint16_t virtual_address,
                   char* buffer, size_t size)
{
    if (!buffer || size == 0) return false;
    buffer[0] = 0;

    uint16_t instruction = dd11::read16(address);
    if ((instruction & 0170000) == 0170000) {
        uint16_t source = (instruction >> 6) & 077;
        uint16_t destination = instruction & 077;
        uint32_t physical_cursor = address + 2;
        uint16_t virtual_cursor = virtual_address + 2;

        disasm_append(buffer, size, "FP11/UNIMPL ");
        disasm_format_operand(source, physical_cursor, virtual_cursor,
                              buffer, size);
        disasm_append(buffer, size, ",");
        disasm_format_operand(destination, physical_cursor, virtual_cursor,
                              buffer, size);
        return true;
    }

    D decoded = {};
    for (uint8_t i = 0; disamtable[i].inst; i++) {
        if ((instruction & disamtable[i].inst) == disamtable[i].arg) {
            decoded = disamtable[i];
            break;
        }
    }
    if (!decoded.inst) {
        disasm_append(buffer, size, "???");
        return false;
    }

    disasm_append(buffer, size, "%s", decoded.msg);
    if (decoded.b && (instruction & 0100000))
        disasm_append(buffer, size, "B");

    uint16_t source = (instruction >> 6) & 077;
    uint16_t destination = instruction & 077;
    uint8_t offset = instruction & 0377;
    uint32_t physical_cursor = address + 2;
    uint16_t virtual_cursor = virtual_address + 2;

    switch (decoded.flag)
    {
    case S | DD:
        disasm_append(buffer, size, " ");
        disasm_format_operand(source, physical_cursor, virtual_cursor,
                              buffer, size);
        disasm_append(buffer, size, ",");
        disasm_format_operand(destination, physical_cursor, virtual_cursor,
                              buffer, size);
        break;
    case DD:
        disasm_append(buffer, size, " ");
        disasm_format_operand(destination, physical_cursor, virtual_cursor,
                              buffer, size);
        break;
    case RR | O: {
        uint16_t target = (uint16_t)(virtual_address + 2 -
                                     2 * (instruction & 077));
        disasm_append(buffer, size, " %s,%06o",
                      rs[(instruction >> 6) & 7], target);
        break;
    }
    case O: {
        int16_t displacement = (int8_t)offset;
        uint16_t target = (uint16_t)(virtual_address + 2 +
                                     displacement * 2);
        disasm_append(buffer, size, " %06o", target);
        break;
    }
    case RR | DD:
        disasm_append(buffer, size, " %s,",
                      rs[(instruction >> 6) & 7]);
        disasm_format_operand(destination, physical_cursor, virtual_cursor,
                              buffer, size);
        break;
    case RR:
        disasm_append(buffer, size, " %s", rs[instruction & 7]);
        break;
    case N:
        disasm_append(buffer, size, " %03o", instruction & 0377);
        break;
    }
    return true;
}

void printstate()
{
    _printf("%%%% R0 0%06o R1 0%06o R2 0%06o R3 0%06o\r\n",
      uint16_t(procNS::R[0]), uint16_t(procNS::R[1]), uint16_t(procNS::R[2]), uint16_t(procNS::R[3]));
    _printf("%%%% R4 0%06o R5 0%06o R6 0%06o R7 0%06o\r\n",
      uint16_t(procNS::R[4]), uint16_t(procNS::R[5]), uint16_t(procNS::R[6]), procNS::curPC);  // uint16_t(procNS::R[7]));
    _printf("%%%% [%c%c%s%s%s%s]\r\n",
      tolower(users_char[procNS::prevuser]),
      users_char[procNS::curuser],
      procNS::N() ? "N" : " ",
      procNS::Z() ? "Z" : " ",
      procNS::V() ? "V" : " ",
      procNS::C() ? "C" : " ");
    _printf("%%%% mmu inst decode     %s\r\n", !(kt11::SR0 & 1) ? "disabled" : "enabled");
    _printf("%%%% mmu data decode [U] %s\r\n", !(kt11::SR3 & 1) ? "disabled" : "enabled");
    _printf("%%%% mmu data decode [K] %s\r\n", !(kt11::SR3 & 4) ? "disabled" : "enabled");
    _printf("%%%% mmu data decode [S] %s\r\n", !(kt11::SR3 & 2) ? "disabled" : "enabled");
    _printf("%%%% instr 0%06o: 0%06o\t", procNS::curPC, dd11::read16(kt11::decode_instr(procNS::curPC, false, procNS::curuser)));
#if ALLOW_DISASM
    disasm(kt11::decode_instr(procNS::curPC, false, procNS::curuser));
#endif
    Serial.println("\r\n");
    Serial.flush();
}
