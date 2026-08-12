// (C) 2026 by Folkert van Heusden
// Released under MIT license
//
// vpdp1170 adaptations:
// - Always 8 Unibus lines
// - Deferred service (no dedicated FreeRTOS thread)
// - 64-byte RX silo only on the active Telnet-backed line
// - LPR receive-enable (RCVE) gating per SIMH / EK-DZ11

#pragma once
#include <cstdint>

#include "comm.h"
#include "device.h"
#include "gen.h"
#include "bus.h"
#include "log.h"
#include "my_lock.h"

class bus;

constexpr const int dz11_n_lines = 8;
constexpr const int n_dz11_registers = 4;
constexpr const size_t dz11_line_fifo_bytes = 64;
// Only this line owns RX/TX host FIFOs; others are disconnected stubs.
constexpr const int dz11_active_line = 0;

#define DZ11_INTERRUPT_VECTOR_RX 0310
#define DZ11_INTERRUPT_VECTOR_TX 0314
#define DZ11_INTERRUPT_LEVEL 5
#define DZ11_BASE 0160100
#define DZ11_CSR   DZ11_BASE
#define DZ11_RBUF (DZ11_BASE + 1 * 2)
#define DZ11_LPR  (DZ11_BASE + 1 * 2)
#define DZ11_TCR  (DZ11_BASE + 2 * 2)
#define DZ11_MSR  (DZ11_BASE + 3 * 2)
#define DZ11_TDR  (DZ11_BASE + 3 * 2)
#define DZ11_END  (DZ11_BASE + 4 * 2)

// SIMH / hardware CSR bits (octal-friendly hex)
#define DZ11_CSR_CLR   0000020
#define DZ11_CSR_MSE   0000040
#define DZ11_CSR_RIE   0000100
#define DZ11_CSR_RDONE 0000200
#define DZ11_CSR_SAE   0010000
#define DZ11_CSR_SA    0020000
#define DZ11_CSR_TIE   0040000
#define DZ11_CSR_TRDY  0100000
#define DZ11_CSR_RW    (DZ11_CSR_MSE | DZ11_CSR_RIE | DZ11_CSR_SAE | DZ11_CSR_TIE)

#define DZ11_RBUF_VALID 0100000
#define DZ11_RBUF_OVRE  0040000
#define DZ11_LPR_RCVE   0010000

FLASHMEM class dz11: public device
{
private:
	bus              *const b      { nullptr };
	uint16_t          registers[n_dz11_registers] { 0 };
	size_t            scanner_line_nr { 0 };

	enum cstate { NOT_CONNECTED = 0, PENDING, CONNECTED };
	comm_io          *const io_channels { nullptr };
	cstate            connected[dz11_n_lines] { };
	bool              rx_enable[dz11_n_lines] { };
	enum psetting { NO_PARITY = 0, ODD_PARITY, EVEN_PARITY };
	psetting          parity_setting[dz11_n_lines] { };

	// 64-byte RX silo for the active Telnet line only.
	uint8_t           rx_silo[dz11_line_fifo_bytes] { };
	uint8_t           rx_count { 0 };
	uint8_t           rx_head { 0 };
	uint8_t           rx_tail { 0 };
	bool              rx_overrun { false };

	mutable my_lock   input_lock;

	void trigger_interrupt(const bool is_tx);
	bool is_rx_interrupt_enabled() const;
	bool is_tx_interrupt_enabled() const;
	void update_tx_state();
	void update_rx_int();
	void update_tx_int();
	void poll_connections_and_rx();
	bool silo_push(uint8_t c);
	bool silo_pop(uint8_t* c);
	bool silo_empty() const { return rx_count == 0; }
	bool line_tx_ready(int line) const;

public:
	dz11(bus *const b, comm_io *const io_channels);
	virtual ~dz11();

	bool begin();

	comm_io * get_comm_interfaces() { return io_channels; }

	void reset(const bool hard) override;

	void show_state(console *const cnsl) const override;

	void test_port(const size_t port_nr) const;
	void test_ports(const int cnt) const;

	uint8_t  read_byte(const uint16_t addr) override;
	uint16_t read_word(const uint16_t addr) override;

	void write_byte(const uint16_t addr, const uint8_t  v) override;
	void write_word(const uint16_t addr, const uint16_t v) override;

	// vpdp: polled from the kek instruction deferred path (no DZ thread).
	void service_deferred();
	bool needs_deferred_service();

	void operator()();
};
