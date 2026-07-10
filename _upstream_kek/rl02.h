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
#define RL02_BAE 0174410  // bus address extension register (RLV12-style)
#define RL02_BASE  RL02_CSR
#define RL02_END  (RL02_BAE + 2)

constexpr const int rl02_sectors_per_track = 40;
constexpr const int rl02_track_count       = 512;
constexpr const int rl02_bytes_per_sector  = 256;
constexpr const int rl02_xfer_buffer_bytes = 4096;

void rl02_set_trace(int count);
int rl02_trace_remaining();

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
	uint8_t         deferred_command     { 0 };
	int             deferred_device      { 0 };
	int             deferred_poll_count  { 0 };
	int             deferred_service_delay { -1 };
#if defined(ESP32)
	int             irq_pending_ticks    { 0 };
	static constexpr int IRQ_DELAY_TICKS = 2;
#endif

	abool *const disk_read_activity  { nullptr };
	abool *const disk_write_activity { nullptr };

	uint32_t get_bus_address() const;
	void     update_bus_address(const uint32_t a);
	void     update_dar();
	void     advance_disk_position(uint32_t bytes);
	uint32_t calc_offset() const;
	bool     data_command_pending(const uint16_t csr) const;
	void     defer_data_command(const uint16_t csr, const uint8_t command, const int device);
	void     complete_deferred_data_command();

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

	void write_byte(const uint16_t addr, const uint8_t  v) override;
	void write_word(const uint16_t addr, const uint16_t v) override;
	void service_deferred();
};
