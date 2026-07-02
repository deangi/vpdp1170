#pragma once

#include <stdint.h>

namespace kek_kwp {

static constexpr uint16_t CSR_ADDR  = 0172540;
static constexpr uint16_t CSB_ADDR  = 0172542;
static constexpr uint16_t CNTR_ADDR = 0172544;
static constexpr uint16_t END_ADDR  = 0172550;
static constexpr uint16_t VECTOR    = 0104;
static constexpr uint8_t  BR_LEVEL  = 6;

bool     contains(uint16_t addr);
void     reset();
void     tick();
bool     take_interrupt();
uint16_t read_word(uint16_t addr);
uint8_t  read_byte(uint16_t addr);
void     write_word(uint16_t addr, uint16_t value);
void     write_byte(uint16_t addr, uint8_t value);

}  // namespace kek_kwp
