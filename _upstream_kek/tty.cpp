// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#include <errno.h>
#include <string.h>
#include <unistd.h>
#if defined(ESP32)
#include <Arduino.h>
#endif

#include "tty.h"
#include "cpu.h"
#include "gen.h"
#include "log.h"
#include "memory.h"
#include "utils.h"


const char * const regnames[] = { 
	"reader status ",
	"reader buffer ",
	"puncher status",
	"puncher buffer"
	};

static constexpr uint16_t TTY_DONE = 0200;
static constexpr uint16_t TTY_IE   = 0100;
static constexpr uint32_t TTY_TX_DELAY_US = 100;

static uint32_t tty_now_us()
{
#if defined(ESP32)
	return micros();
#else
	return 0;
#endif
}

tty::tty(console *const c, bus *const b) :
	c(c),
	b(b)
{
	reset(true);
	c->set_data_cb_notifier(this);
}

tty::~tty()
{
}

void tty::reset(const bool hard)
{
	if (hard) {
		memset(registers, 0x00, sizeof registers);
		registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] = TTY_DONE;
	}
	rx_irq_asserted = false;
	tx_irq_asserted = false;
	tx_ready_reported = true;
	tx_busy = false;
	tx_ready_at_us = 0;
}

void tty::update_rx_interrupt()
{
	const uint16_t tks = registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2];
	const bool should_assert = (tks & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE);
	if (should_assert && !rx_irq_asserted) {
		rx_irq_asserted = true;
		b->getCpu()->queue_interrupt(4, 060);
	}
	else if (!should_assert && rx_irq_asserted) {
		rx_irq_asserted = false;
		b->getCpu()->unqueue_interrupt(4, 060);
	}
}

void tty::update_tx_interrupt()
{
	const uint16_t tps = registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2];
	const bool should_assert = (tps & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE);
	bool cpu_has_irq = false;

	if (b && b->getCpu()) {
		cpu_has_irq = b->getCpu()->has_queued_interrupt(4, 064);
	}

	if (should_assert && !cpu_has_irq && b && b->getCpu()) {
		tx_irq_asserted = true;
		tx_ready_reported = true;
		b->getCpu()->queue_interrupt(4, 064);
	}
	else if (!should_assert && b && b->getCpu()) {
		tx_irq_asserted = false;
		b->getCpu()->unqueue_interrupt(4, 064);
	}
	else if (!should_assert) {
		tx_irq_asserted = false;
	}
}

void tty::service_deferred()
{
	if (!tx_busy) {
		registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] |= TTY_DONE;
		update_tx_interrupt();
		return;
	}

#if defined(ESP32)
	if ((int32_t)(tty_now_us() - tx_ready_at_us) < 0)
		return;
#endif

	tx_busy = false;
	registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] |= TTY_DONE;
	update_tx_interrupt();
}

uint8_t tty::read_byte(const uint16_t addr)
{
	uint16_t v = read_word(addr & ~1);
	if (addr & 1)
		return v >> 8;
	return v;
}

void tty::notify_rx()
{
	registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] |= TTY_DONE;
	update_rx_interrupt();
}

uint16_t tty::read_word(const uint16_t addr)
{
	const int reg    = (addr - PDP11TTY_BASE) / 2;
	service_deferred();
	uint16_t  vtemp  = registers[reg];
	bool      notify = false;

	if (addr == PDP11TTY_TKS) {
		bool have_char = c->poll_char();

		vtemp &= ~TTY_DONE;
		vtemp |= have_char ? TTY_DONE : 0;
	}
	else if (addr == PDP11TTY_TKB) {
		auto ch = c->wait_char(1);
		if (ch.has_value() == false) {
			vtemp = 0;
			registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] &= ~TTY_DONE;
		}
		else {
			vtemp = ch.value() | (parity(ch.value()) << 7);
			if (c->poll_char()) {
				registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] |= TTY_DONE;
				notify = true;
			}
			else
				registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] &= ~TTY_DONE;
		}
	}
	else if (addr == PDP11TTY_TPS) {
		vtemp = registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2];
	}

	DOLOG(log_ss::LS_COMM, "PDP11TTY read addr %o (%s): %d, 7bit: %d", addr, regnames[reg], vtemp, vtemp & 127);

	registers[reg] = vtemp;

	if (notify)
		notify_rx();
	else if (addr == PDP11TTY_TKS || addr == PDP11TTY_TKB)
		update_rx_interrupt();

	return vtemp;
}

void tty::write_byte(const uint16_t addr, const uint8_t v)
{
	uint16_t vtemp = registers[(addr - PDP11TTY_BASE) / 2];
	
	if (addr & 1) {
		vtemp &= ~0xff00;
		vtemp |= v << 8;
	}
	else {
		vtemp &= ~0x00ff;
		vtemp |= v;
	}

	write_word(addr, vtemp);
}

void tty::write_word(const uint16_t addr, uint16_t v)
{
	const int reg = (addr - PDP11TTY_BASE) / 2;

	DOLOG(log_ss::LS_COMM, "PDP11TTY write %o (%s): %o", addr, regnames[reg], v);

	if (addr == PDP11TTY_TKS) {
		registers[reg] = v & TTY_IE;
		if (c->poll_char())
			registers[reg] |= TTY_DONE;
		update_rx_interrupt();
		return;
	}

	if (addr == PDP11TTY_TPS) {
		const bool old_ie = (registers[reg] & TTY_IE) != 0;
		registers[reg] = (registers[reg] & TTY_DONE) | (v & TTY_IE);
		if (!old_ie && (registers[reg] & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE))
			tx_ready_reported = false;
		update_tx_interrupt();
		return;
	}

	if (addr == PDP11TTY_TPB) {
		char ch = v & 127;
		tx_busy = true;
		tx_ready_reported = false;
		tx_ready_at_us = tty_now_us() + TTY_TX_DELAY_US;
		registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] &= ~TTY_DONE;
		update_tx_interrupt();

		DOLOG(log_ss::LS_COMM, "PDP11TTY print '%c'", ch);
		c->put_char(ch);
	}

	DOLOG(log_ss::LS_COMM, "set register %o to %o", addr, v);
	registers[(addr - PDP11TTY_BASE) / 2] = v;
}

#if IS_POSIX
JsonDocument tty::serialize()
{
	JsonDocument j;

	JsonDocument ja_reg;
	JsonArray    ja_reg_work = ja_reg.to<JsonArray>();
        for(size_t i=0; i<4; i++)
                ja_reg_work.add(registers[i]);
	j["registers"] = ja_reg;

	return j;
}

tty *tty::deserialize(const JsonVariantConst j, bus *const b, console *const cnsl)
{
	tty       *out   = new tty(cnsl, b);

	JsonArrayConst ja_reg = j["registers"];
	int       i_reg  = 0;
	for(auto v: ja_reg)
		out->registers[i_reg++] = v;

	return out;
}
#endif
