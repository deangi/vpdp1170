// (C) 2026 by Folkert van Heusden
// Released under MIT license
// Some of the code is translated from Neil Webber's PDP11/70 emulator

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


#define RP06_CS1 	0176700  // control register
#define RP06_WC	 	0176702  // word count
#define RP06_UBA	0176704  // UNIBUS address
#define RP06_DA	 	0176706  // desired address
#define RP06_CS2	0176710  // control/status register 2
#define RP06_DS	 	0176712  // drive status
#define RP06_ERRREG1    0176714  // error register 1
#define RP06_AS	 	0176716  // unified attention status
#define RP06_RMLA	0176720  // lookahead (sector under head)
#define RP06_DB		0176722  // data buffer
#define RP06_MR		0176724  // maintenance
#define RP06_DT		0176726  // drive type
#define RP06_SN		0176730  // serial number
#define RP06_OFR	0176732  // heads offset
#define RP06_DC	 	0176734  // desired cylinder
#define RP06_CC	 	0176736  // current cylinder
#define RP06_ER2	0176740  // error register 2
#define RP06_ER3	0176742  // error register 3
#define RP06_EC1	0176744  // ECC position
#define RP06_EC2	0176746  // ECC pattern
#define RP06_BAE	0176750  // address extension (pdp11/70 extra phys bits)
#define RP06_BASE  RP06_CS1
#define RP06_END  (RP06_BAE + 2)

void rp06_set_trace(int count);
int  rp06_trace_remaining();

class bus;

class rp06: public disk_device
{
private:
	bus     *const b       { nullptr };
	bool     is_rp07       { false   };
	uint16_t registers[32] {         };
	unsigned int_cnt       { 0       };
	unsigned int_cnt_total { 0       };

	abool *const disk_read_activity  { nullptr };
	abool *const disk_write_activity { nullptr };

	// Deferred DMA: keep RDY clear for a few instruction steps so a BA=0
	// bootstrap can poll CS1 before its own code is overwritten.
	bool     deferred_active { false };
	uint16_t deferred_fnc    { 0 };
	uint16_t deferred_cs1    { 0 };
	uint16_t deferred_wc     { 0 };
	uint16_t deferred_ba     { 0 };
	uint16_t deferred_da     { 0 };
	uint16_t deferred_dc     { 0 };
	uint16_t deferred_bae    { 0 };
	int      deferred_delay  { -1 };
	int      deferred_cs1_polls { 0 };
	int      deferred_wc_polls  { 0 };
	int      la_sector { 0 };

	int      reg_num(uint16_t addr) const;
	uint32_t getphysaddr() const;
	uint32_t compute_offset() const;
	uint32_t getphysaddr_from(uint16_t cs1, uint16_t ba, uint16_t bae) const;
	uint32_t compute_offset_from(uint16_t dc, uint16_t da) const;
	void     defer_data_command(uint16_t fnc, uint16_t cs1_after_go);
	void     complete_deferred_data_command();

public:
	rp06(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity, const bool is_rp07);
	virtual ~rp06();

	void begin() override;
	void reset(const bool hard) override;
	void service_deferred();

	void show_state(console *const cnsl) const override;

#if IS_POSIX
	JsonDocument serialize() const;
	static rp06 *deserialize(const JsonVariantConst j, bus *const b);
#endif

	uint8_t  read_byte(const uint16_t addr) override;
	uint16_t read_word(const uint16_t addr) override;

	// Monitor/debug examine: return register image without CS1/WC poll side effects.
	uint16_t peek_word(const uint16_t addr) const;
	bool     is_deferred_active() const { return deferred_active; }
	int      deferred_delay_remaining() const { return deferred_delay; }
	int      deferred_cs1_poll_count() const { return deferred_cs1_polls; }
	int      deferred_wc_poll_count() const { return deferred_wc_polls; }

	void write_byte(const uint16_t addr, const uint8_t  v) override;
	void write_word(const uint16_t addr, const uint16_t v) override;

	enum class ds_bits {
		OFM = 0000001,  // offset mode
		VV  = 0000100,  // volume valid
		DRY = 0000200,  // drive ready
		DPR = 0000400,  // drive present
		MOL = 0010000   // medium online
	};

	enum class cs1_bits {
		GO  = 0000001,  // GO bit
		FN  = 0000076,  // 5 bit function code - this is the mask
		IE  = 0000100,  // Interrupt enable
		RDY = 0000200,  // Drive ready
		A16 = 0000400,
		A17 = 0001000,
		TRE = 0040000,
	};
};
