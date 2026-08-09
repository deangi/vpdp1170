// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#pragma once

#include "gen.h"
#if IS_POSIX
#include <ArduinoJson.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "disk_device.h"
#include "disk_backend.h"


#define RL02_CSR 0174400  // control status register
#define RL02_BAR 0174402  // bus address register
#define RL02_DAR 0174404  // disk address register
#define RL02_MPR 0174406  // multi purpose register
#define RL02_BAE 0174410  // bus address extension (RLV12/Q22 only)
#define RL02_BASE  RL02_CSR
// Unibus RL11 (PDP-11/70): registers stop at MPR. SIMH returns NXM for
// RLBAE on Unibus/RLV11; answering BAE made RSTS INIT treat us as RLV12
// and then fatal on the vector-160 interrupt probe.
#define RL02_END  (RL02_MPR + 2)

constexpr const int rl02_sectors_per_track = 40;
constexpr const int rl02_track_count       = 512;
constexpr const int rl02_bytes_per_sector  = 256;
constexpr const int rl02_xfer_buffer_bytes = 4096;

void rl02_set_trace(int count);
int rl02_trace_remaining();
#if defined(ESP32)
void rl02_trace_vector_write(uint32_t pa, uint16_t value, uint16_t va);
#endif

class bus;

class rl02: public disk_device
{
private:
	bus      *const b;
	uint16_t        registers[5];
	uint8_t         xfer_buffer[rl02_xfer_buffer_bytes];
	int16_t         track  { 0 };
	uint8_t         head   { 0 };
	uint8_t         sector { 0 };
	uint16_t        mpr[3];
	bool            bae_active { false };
	bool            deferred_data_active { false };
	bool            deferred_execute     { false };
	uint16_t        deferred_csr         { 0 };
	uint16_t        deferred_bar         { 0 };
	uint16_t        deferred_dar         { 0 };
	uint16_t        deferred_mpr         { 0 };
	uint16_t        deferred_bae         { 0 };
	bool            deferred_bae_active  { false };
	uint8_t         deferred_command     { 0 };
	int             deferred_device      { 0 };
	int             deferred_poll_count  { 0 };
	int             deferred_service_delay { -1 };
#if defined(ESP32)
	int             irq_pending_ticks    { 0 };
	int             irq_spl_ok_ticks     { 0 };
	// RSTS INIT: GETSTAT+IE completes under high SPL; later SPL drops and
	// the poll loop at ~067xxx expects BR5. Deliver only when both are true
	// (SPL < 5 and PC in the high poll range), after a short arm delay so
	// the "expecting vector 160" flag is set. Too early → unexpected 160;
	// never delivered → "does not interrupt" then unexpected 4.
	static constexpr int IRQ_DELAY_TICKS = 96;
	static constexpr int IRQ_SPL_ARM_TICKS = 3;
#endif

	abool *const disk_read_activity  { nullptr };
	abool *const disk_write_activity { nullptr };

	uint32_t get_bus_address() const;
	void     update_bus_address(const uint32_t a);
	void     update_dar();
	void     advance_dar_raw(uint32_t bytes);
	void     advance_disk_position(uint32_t bytes);
	uint32_t calc_offset() const;
	bool     data_command_pending(const uint16_t csr) const;
	void     defer_data_command(const uint16_t csr, const uint8_t command, const int device);
	void     complete_deferred_data_command();
	// Sync heads to a data DAR when they disagree. Real RL11 returns HNF
	// instead, but 2.11BSD's standalone driver assumes every SEEK worked and
	// never re-reads the header after an error — a soft/hard position desync
	// then HNF-storms (rlcs=112275). SIMH avoids some of this with per-unit
	// TRK; we also tolerate a mismatch by implying the seek.
	bool     ensure_position_for_data(int device, uint8_t req_sector,
					  uint8_t req_head, int req_track,
					  const char *op);

public:
	rl02(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity);
	virtual ~rl02();

	void begin() override;
	void reset(const bool hard) override;

	void show_state(console *const cnsl) const override;

#if IS_POSIX
	JsonDocument serialize() const;
	static rl02 *deserialize(const JsonVariantConst j, bus *const b);
#endif

	uint8_t  read_byte(const uint16_t addr) override;
	uint16_t read_word(const uint16_t addr) override;
	// Side-effect-free examine for monitors / telnet (no MPR shift, no poll).
	uint16_t peek_word(const uint16_t addr) const;
	bool     is_deferred_active() const { return deferred_data_active; }
	int      deferred_delay_remaining() const { return deferred_service_delay; }
	int      deferred_polls() const { return deferred_poll_count; }
	uint8_t  deferred_cmd() const { return deferred_command; }
	int      deferred_unit() const { return deferred_device; }
#if defined(ESP32)
	int      irq_ticks_remaining() const { return irq_pending_ticks; }
#else
	int      irq_ticks_remaining() const { return 0; }
#endif
	int16_t  current_track() const { return track; }
	uint8_t  current_head() const { return head; }
	uint8_t  current_sector() const { return sector; }

	void write_byte(const uint16_t addr, const uint8_t  v) override;
	void write_word(const uint16_t addr, const uint16_t v) override;
	void service_deferred();
	bool needs_deferred_service() const {
		return deferred_data_active || irq_ticks_remaining() > 0;
	}
};
