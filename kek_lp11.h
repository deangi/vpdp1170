#pragma once

#include <stdint.h>

namespace kek_lp11 {

static constexpr uint16_t CSR_ADDR  = 0177514;
static constexpr uint16_t DB_ADDR   = 0177516;
static constexpr uint16_t VECTOR    = 0200;
static constexpr uint8_t  BR_LEVEL  = 4;

using InstructionCounterFn = uint64_t (*)();

bool     contains(uint16_t addr);
void     set_instruction_counter(InstructionCounterFn fn);
void     reset();
void     tick();
bool     take_interrupt();
uint16_t read_word(uint16_t addr);
uint8_t  read_byte(uint16_t addr);
void     write_word(uint16_t addr, uint16_t value);
void     write_byte(uint16_t addr, uint8_t value);

}  // namespace kek_lp11
