// (C) 2026 by Folkert van Heusden
// Released under MIT license
//
// vpdp1170: deferred-service DZ11 with 64-byte silo on the active line only.
// Register/IRQ behavior aligned with SIMH pdp11_dz.c for CSR/RBUF/LPR/TCR/MSR/TDR.

#include "gen.h"
#if defined(ESP32)
#include <Arduino.h>
#endif
#include <cstring>
#include <string>

#include "bus.h"
#include "cpu.h"
#include "dz11.h"
#include "log.h"
#include "utils.h"

constexpr const char *const dz11_register_names[] {
	"R0_CSR", "R2_RBUF_LPR", "R4_TCR", "R6_MSR_TDR"
};

dz11::dz11(bus *const b, comm_io *const io_channels):
	b(b),
	io_channels(io_channels)
{
	reset(true);
}

dz11::~dz11()
{
	DOLOG(log_ss::LS_COMM, "DZ11 closing");
	delete io_channels;
}

FLASHMEM void dz11::show_state(console *const cnsl) const
{
	my_unique_lock lck(&input_lock);
	cnsl->put_string_lf(format(
		" active line %d silo %u/%u overrun=%d",
		dz11_active_line, (unsigned)rx_count,
		(unsigned)dz11_line_fifo_bytes, rx_overrun ? 1 : 0));
	for (int i = 0; i < dz11_n_lines; i++) {
		std::string out = format(" line %d: ", i);
		if (connected[i] == NOT_CONNECTED) out += "not connected";
		else if (connected[i] == PENDING) out += "pending";
		else out += "connected";
		if (rx_enable[i]) out += ", rx_en";
		cnsl->put_string_lf(out);
	}
	cnsl->put_string_lf(format(" RX IE: %s  TX IE: %s",
		is_rx_interrupt_enabled() ? "true" : "false",
		is_tx_interrupt_enabled() ? "true" : "false"));
	for (int i = 0; i < n_dz11_registers; i++)
		cnsl->put_string_lf(format(" register %d: %06o", i, registers[i]));
}

bool dz11::begin()
{
	DOLOG(log_ss::LS_COMM, "DZ11 begin (deferred service, %d lines, silo %u on line %d)",
	      dz11_n_lines, (unsigned)dz11_line_fifo_bytes, dz11_active_line);
	return true;
}

void dz11::test_port(const size_t nr) const
{
	auto str = format("DZ11 test line %" PRIzu "", nr);
	DOLOG(log_ss::LS_COMM, str.c_str());
	io_channels->send_data(nr, reinterpret_cast<const uint8_t *>(str.c_str()), str.size());
}

void dz11::test_ports(const int cnt) const
{
	for (int k = 0; k < cnt; k++) {
		for (int i = 0; i < dz11_n_lines; i++)
			test_port(i);
	}
}

void dz11::trigger_interrupt(const bool is_tx)
{
	DOLOG(log_ss::LS_COMM, "DZ11: %s interrupt", is_tx ? "TX" : "RX");
	b->getCpu()->queue_interrupt(DZ11_INTERRUPT_LEVEL,
		is_tx ? DZ11_INTERRUPT_VECTOR_TX : DZ11_INTERRUPT_VECTOR_RX);
}

bool dz11::silo_push(uint8_t c)
{
	if (rx_count >= dz11_line_fifo_bytes) {
		rx_overrun = true;
		return false;
	}
	rx_silo[rx_head] = c;
	rx_head = (uint8_t)((rx_head + 1) % dz11_line_fifo_bytes);
	rx_count++;
	return true;
}

bool dz11::silo_pop(uint8_t* c)
{
	if (rx_count == 0) return false;
	*c = rx_silo[rx_tail];
	rx_tail = (uint8_t)((rx_tail + 1) % dz11_line_fifo_bytes);
	rx_count--;
	return true;
}

void dz11::reset(const bool hard)
{
	if (!hard) return;
	// SIMH dz_clear: CSR starts at 0 (no TRDY). Asserting TRDY out of
	// reset made RSTS enable TIE and take an unexpected vector-314 trap.
	for (int i = 0; i < n_dz11_registers; i++)
		registers[i] = 0;
	for (int i = 0; i < dz11_n_lines; i++) {
		rx_enable[i] = false;
		parity_setting[i] = NO_PARITY;
	}
	rx_count = 0;
	rx_head = 0;
	rx_tail = 0;
	rx_overrun = false;
	scanner_line_nr = 0;
}

bool dz11::is_rx_interrupt_enabled() const
{
	return (registers[0] & DZ11_CSR_RIE) && (registers[0] & DZ11_CSR_MSE);
}

bool dz11::is_tx_interrupt_enabled() const
{
	return (registers[0] & DZ11_CSR_TIE) && (registers[0] & DZ11_CSR_MSE);
}

// Telnet-backed line is ready to accept TX only when a client is present.
// Stub lines never go ready — matches "no carrier" for unused lines.
bool dz11::line_tx_ready(int line) const
{
	if (line < 0 || line >= dz11_n_lines) return false;
	if ((registers[2] & (1 << line)) == 0) return false;
	return connected[line] == CONNECTED;
}

void dz11::update_rx_int()
{
	if (is_rx_interrupt_enabled() && !silo_empty())
		trigger_interrupt(false);
	else
		b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
			DZ11_INTERRUPT_VECTOR_RX);
}

void dz11::update_tx_int()
{
	if (is_tx_interrupt_enabled() && (registers[0] & DZ11_CSR_TRDY))
		trigger_interrupt(true);
	else
		b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
			DZ11_INTERRUPT_VECTOR_TX);
}

// SIMH dz_update_xmti: TRDY only while MSE and an enabled line can take TX.
void dz11::update_tx_state()
{
	if ((registers[0] & DZ11_CSR_MSE) == 0) {
		registers[0] &= (uint16_t)~(DZ11_CSR_TRDY | DZ11_CSR_RDONE | DZ11_CSR_SA);
		update_tx_int();
		return;
	}

	if (registers[0] & DZ11_CSR_TRDY) {
		const int cur = (int)((registers[0] >> 8) & 7);
		if (!line_tx_ready(cur))
			registers[0] &= (uint16_t)~DZ11_CSR_TRDY;
		update_tx_int();
		if (registers[0] & DZ11_CSR_TRDY)
			return;
	}

	const int start = (int)scanner_line_nr;
	for (int i = 0; i < dz11_n_lines; i++) {
		const int line = (start + 1 + i) % dz11_n_lines;
		if (!line_tx_ready(line))
			continue;
		registers[0] &= (uint16_t)~0x700;
		registers[0] |= (uint16_t)(line << 8);
		scanner_line_nr = (size_t)line;
		registers[0] |= DZ11_CSR_TRDY;
		break;
	}
	update_tx_int();
}

void dz11::poll_connections_and_rx()
{
	if (!io_channels) return;

	for (int line_nr = 0; line_nr < dz11_n_lines; line_nr++) {
		bool is_connected  = io_channels->is_connected(line_nr);
		bool was_connected = connected[line_nr] != NOT_CONNECTED;

		if (is_connected != was_connected) {
			DOLOG(log_ss::LS_COMM, "DZ11 line %d state changed to %d",
			      line_nr, is_connected);
			connected[line_nr] = is_connected ? PENDING : NOT_CONNECTED;

			uint16_t mask1 = (uint16_t)(1 << line_nr);
			uint16_t mask2 = (uint16_t)(1 << (line_nr + 8));
			if (is_connected)
				registers[3] |= mask1 | mask2;
			else
				registers[3] &= (uint16_t)~(mask1 | mask2);
		}

		if (line_nr != dz11_active_line)
			continue;
		if (!rx_enable[line_nr])
			continue;

		while (io_channels->has_data(line_nr)) {
			uint8_t buffer = io_channels->get_byte(line_nr);
			if (!silo_push(buffer))
				break;
		}
	}
	update_rx_int();
}

void dz11::service_deferred()
{
	my_unique_lock lck(&input_lock);
	poll_connections_and_rx();
	update_tx_state();
}

bool dz11::needs_deferred_service()
{
	if (!io_channels) return false;
	if (io_channels->is_connected(dz11_active_line)) return true;
	if (io_channels->has_data(dz11_active_line)) return true;
	if ((registers[0] & DZ11_CSR_MSE) && (registers[2] & 0xff)) return true;
	if (!silo_empty()) return true;
	return false;
}

void dz11::operator()()
{
}

uint8_t dz11::read_byte(const uint16_t addr)
{
	uint16_t v = read_word(addr & ~1);
	if (addr & 1)
		return (uint8_t)(v >> 8);
	return (uint8_t)v;
}

uint16_t dz11::read_word(const uint16_t addr)
{
	my_unique_lock lck(&input_lock);
	int      reg   = (addr - DZ11_BASE) / 2;
	uint16_t vtemp = registers[reg];

	if (addr == DZ11_CSR) {
		if (registers[reg] & DZ11_CSR_CLR) {
			reset(true);
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_RX);
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_TX);
		}

		// Report stored TRDY/TLINE; only RDONE is dynamic from the silo.
		// Do NOT synthesize TRDY from TCR enables (that primed vector 314).
		vtemp = registers[0];
		vtemp &= (uint16_t)~DZ11_CSR_RDONE;
		if (!silo_empty())
			vtemp |= DZ11_CSR_RDONE;
		vtemp &= (uint16_t)~7;
		registers[0] = (uint16_t)((registers[0] & (uint16_t)~DZ11_CSR_RDONE) |
					  (vtemp & DZ11_CSR_RDONE));
	}
	else if (addr == DZ11_RBUF) {
		vtemp = 0;
		uint8_t c = 0;
		if (silo_pop(&c)) {
			bool p = false;
			if (parity_setting[dz11_active_line] == EVEN_PARITY)
				p = !parity(c);
			else if (parity_setting[dz11_active_line] == ODD_PARITY)
				p = parity(c);
			vtemp = (uint16_t)(DZ11_RBUF_VALID |
				(dz11_active_line << 8) | c | (p << 7));
			if (rx_overrun) {
				vtemp |= DZ11_RBUF_OVRE;
				rx_overrun = false;
			}
		}
		registers[0] &= (uint16_t)~DZ11_CSR_SA;
		update_rx_int();
	}
	else if (addr == DZ11_TCR) {
		/* as stored */
	}
	else if (addr == DZ11_MSR) {
		vtemp = registers[reg] & 0x00ff;
		for (int i = 0; i < dz11_n_lines; i++) {
			if (connected[i] != NOT_CONNECTED)
				vtemp |= (uint16_t)(1 << (8 + i));
		}
		registers[reg] &= 0xff00;
	}

	DOLOG(log_ss::LS_COMM, "DZ11: read %06o from register %06o (\"%s\", %d)",
	      vtemp, addr, dz11_register_names[reg], reg);
	return vtemp;
}

void dz11::write_byte(const uint16_t addr, const uint8_t v)
{
	uint16_t vtemp = registers[(addr - DZ11_BASE) / 2];
	if (addr & 1) {
		vtemp &= 0x00ff;
		vtemp |= (uint16_t)(v << 8);
	} else {
		vtemp &= 0xff00;
		vtemp |= v;
	}
	write_word(addr & ~1, vtemp);
}

void dz11::write_word(const uint16_t addr, const uint16_t v)
{
	int      reg   = (addr - DZ11_BASE) / 2;
	uint16_t v_set = v;
	DOLOG(log_ss::LS_COMM, "DZ11: write %06o to register %06o (\"%s\", %d)",
	      v, addr, dz11_register_names[reg], reg);

	my_unique_lock lck(&input_lock);
	if (addr == DZ11_CSR) {
		if (v & DZ11_CSR_CLR) {
			rx_count = 0;
			rx_head = 0;
			rx_tail = 0;
			rx_overrun = false;
			for (int i = 0; i < dz11_n_lines; i++)
				rx_enable[i] = false;
			registers[0] = 0;
			registers[1] = 0;
			registers[2] &= 0xff00;  // SIMH: keep DTR, clear XMTE
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_RX);
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_TX);
			return;
		}

		const uint16_t old = registers[0];
		v_set = (uint16_t)((registers[0] & (uint16_t)~DZ11_CSR_RW) |
				   (v & DZ11_CSR_RW));
		registers[0] = v_set;

		if ((v_set & DZ11_CSR_RIE) == 0)
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_RX);
		else if (!(old & DZ11_CSR_RIE) && (v_set & DZ11_CSR_MSE) &&
			 !silo_empty())
			trigger_interrupt(false);

		if ((v_set & DZ11_CSR_TIE) == 0)
			b->getCpu()->unqueue_interrupt(DZ11_INTERRUPT_LEVEL,
				DZ11_INTERRUPT_VECTOR_TX);
		else if (!(old & DZ11_CSR_TIE) && (old & DZ11_CSR_TRDY) &&
			 (v_set & DZ11_CSR_MSE))
			trigger_interrupt(true);

		update_tx_state();
		return;
	}
	else if (addr == DZ11_LPR) {
		int line_nr = v & 7;
		if (line_nr < dz11_n_lines) {
			rx_enable[line_nr] = (v & DZ11_LPR_RCVE) != 0;
			if (v & 64)
				parity_setting[line_nr] = (v & 128) ? ODD_PARITY : EVEN_PARITY;
			else
				parity_setting[line_nr] = NO_PARITY;
		}
		registers[reg] = v;
		update_rx_int();
		return;
	}
	else if (addr == DZ11_TDR) {
		int line_nr = (int)((registers[0] >> 8) & 7);
		if (line_nr < dz11_n_lines) {
			uint8_t c = (parity_setting[line_nr] != NO_PARITY)
				? (uint8_t)(v & 127) : (uint8_t)v;
			io_channels->send_data(line_nr, &c, 1);
			DOLOG(log_ss::LS_COMM, "DZ11 TRANSMIT %c (%d) on line %d",
			      c, v, line_nr);
		}
		registers[0] &= (uint16_t)~DZ11_CSR_TRDY;
		update_tx_state();
		return;
	}
	else if (addr == DZ11_TCR) {
		for (int i = 0; i < dz11_n_lines; i++) {
			uint16_t mask = (uint16_t)(1 << i);
			if ((v & mask) && connected[i] == PENDING)
				connected[i] = CONNECTED;
		}
		registers[reg] = v;
		if (registers[0] & DZ11_CSR_TRDY) {
			const int cur = (int)((registers[0] >> 8) & 7);
			if ((v & (1 << cur)) == 0)
				registers[0] &= (uint16_t)~DZ11_CSR_TRDY;
		}
		update_tx_state();
		return;
	}

	registers[reg] = v_set;
}
