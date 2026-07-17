// (C) 2026 by Folkert van Heusden
// Released under MIT license
// Some of the code is translated from Neil Webber's PDP11/70 emulator

#include <algorithm>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "error.h"
#include "gen.h"
#include "log.h"
#include "rp06.h"
#include "utils.h"

#if defined(ESP32)
#include "../platform.h"
#endif


static unsigned NSECT       = 22;               // sectors per track
static unsigned NTRAC       = 19;               // tracks per cylinder
constexpr const unsigned SECTOR_SIZE = 512;
constexpr const uint16_t default_DS  = uint16_t(rp06::ds_bits::DPR) /* drive present */ | uint16_t(rp06::ds_bits::MOL) /* medium on-line */ | uint16_t(rp06::ds_bits::VV) /* volume valid */ | uint16_t(rp06::ds_bits::DRY) /* drive ready */;
constexpr const uint16_t CS1_SC  = 0100000;
constexpr const uint16_t CS1_IR  = 0004000;
constexpr const uint16_t CS1_DVA = 0004000;
constexpr const uint16_t CS2_CLR = 0000040;
constexpr const uint16_t CS2_IR  = 0000200;
constexpr const uint16_t DS_LST  = 0002000;
constexpr const uint16_t DS_ERR  = 0040000;
constexpr const uint16_t DS_ATA  = 0100000;
static constexpr int rp06_deferred_completion_instructions = 4;
static constexpr uint16_t ER1_ILF = 0000001;  // illegal function
static constexpr uint16_t ER1_RMR = 0000004;  // register modification refused (SIMH)
static constexpr uint16_t ER1_AOE = 0000100;  // address overflow/error
static constexpr uint16_t ER1_HCRC = 0000200; // header compare / invalid sector

// Unibus RH11/RP register names (must match addresses in rp06.h).
static constexpr const char *regnames[] {
	"CS1", "WC", "BA", "DA",
	"CS2", "DS", "ER1", "AS",
	"LA", "DB", "MR", "DT",
	"SN", "OF", "DC", "CC",
	"ER2", "ER3", "EC1", "EC2",
	"BAE"
};

static int rp06_trace_left = 0;
static uint16_t rp06_last_traced_addr = 0xffff;
static uint16_t rp06_last_traced_val = 0xffff;
static int rp06_repeat_reads = 0;

void rp06_set_trace(const int count)
{
	rp06_trace_left = count < 0 ? 0 : count;
	rp06_last_traced_addr = 0xffff;
	rp06_repeat_reads = 0;
}

int rp06_trace_remaining()
{
	return rp06_trace_left;
}

static const char *rp06_reg_name(const int reg)
{
	const int n = (int)(sizeof(regnames) / sizeof(regnames[0]));
	if (reg < 0 || reg >= n)
		return "?";
	return regnames[reg];
}

static uint16_t rp06_cpu_pc(bus *const b)
{
	if (!b || !b->getCpu())
		return 0;
	return b->getCpu()->getPC();
}

static int rp06_cpu_spl(bus *const b)
{
	if (!b || !b->getCpu())
		return -1;
	return b->getCpu()->getPSW_spl();
}

static bool rp06_irq_pending(bus *const b)
{
	if (!b || !b->getCpu())
		return false;
	return b->getCpu()->has_queued_interrupt(5, 0254);
}

static void rp06_trace(const char *fmt, ...)
{
#if defined(ESP32)
	if (rp06_trace_left <= 0)
		return;
	rp06_trace_left--;

	char buffer[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof buffer, fmt, ap);
	va_end(ap);
	LOG("kek RP06 pc=%06o spl=%d irq=%d %s",
	    (unsigned)rp06_cpu_pc(nullptr), -1, 0, buffer);
#else
	(void)fmt;
#endif
}

static void rp06_trace_dev(bus *const b, const char *fmt, ...)
{
#if defined(ESP32)
	if (rp06_trace_left <= 0)
		return;
	rp06_trace_left--;

	char buffer[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof buffer, fmt, ap);
	va_end(ap);
	LOG("kek RP06 pc=%06o spl=%d irq=%d %s",
	    (unsigned)rp06_cpu_pc(b), rp06_cpu_spl(b),
	    rp06_irq_pending(b) ? 1 : 0, buffer);
#else
	(void)b; (void)fmt;
#endif
}

// RH70 DMA address: BA+BAE is a 22-bit *physical* address (SIMH mba_rdbufW:
// pa = ba when not RH11). Unibus map is NOT used. Do not MMU-translate BA.
static uint32_t rp06_rh70_dma_pa(const uint32_t ba_reg)
{
	return ba_reg & 017777777u;
}


// Suppress identical tight-poll READ spam so the event budget lasts.
static void rp06_trace_read(const uint16_t addr, const uint16_t value,
			    const char *fmt, ...)
{
#if defined(ESP32)
	if (addr == rp06_last_traced_addr && value == rp06_last_traced_val) {
		rp06_repeat_reads++;
		return;
	}
	if (rp06_repeat_reads > 0) {
		if (rp06_trace_left > 0) {
			rp06_trace_left--;
			LOG("kek RP06 READ-POLL repeated %d more times", rp06_repeat_reads);
		}
		rp06_repeat_reads = 0;
	}
	rp06_last_traced_addr = addr;
	rp06_last_traced_val = value;
	if (rp06_trace_left <= 0)
		return;
	rp06_trace_left--;

	char buffer[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof buffer, fmt, ap);
	va_end(ap);
	LOG("kek RP06 pc=%06o spl=%d irq=%d %s",
	    (unsigned)rp06_cpu_pc(nullptr), -1, 0, buffer);
#else
	(void)addr; (void)value; (void)fmt;
#endif
}

static void rp06_trace_read_dev(bus *const b, const uint16_t addr,
				const uint16_t value, const char *fmt, ...)
{
#if defined(ESP32)
	if (addr == rp06_last_traced_addr && value == rp06_last_traced_val) {
		rp06_repeat_reads++;
		return;
	}
	if (rp06_repeat_reads > 0) {
		if (rp06_trace_left > 0) {
			rp06_trace_left--;
			LOG("kek RP06 pc=%06o spl=%d irq=%d READ-POLL repeated %d more times",
			    (unsigned)rp06_cpu_pc(b), rp06_cpu_spl(b),
			    rp06_irq_pending(b) ? 1 : 0, rp06_repeat_reads);
		}
		rp06_repeat_reads = 0;
	}
	rp06_last_traced_addr = addr;
	rp06_last_traced_val = value;
	if (rp06_trace_left <= 0)
		return;
	rp06_trace_left--;

	char buffer[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof buffer, fmt, ap);
	va_end(ap);
	LOG("kek RP06 pc=%06o spl=%d irq=%d %s",
	    (unsigned)rp06_cpu_pc(b), rp06_cpu_spl(b),
	    rp06_irq_pending(b) ? 1 : 0, buffer);
#else
	(void)b; (void)addr; (void)value; (void)fmt;
#endif
}

// Massbus function field: bits <5:1> of CS1 (SIMH GET_FNC).
static const char *rp06_func_name(const uint16_t fnc)
{
	switch (fnc) {
		case 000: return "NOP";
		case 001: return "UNLOAD";
		case 002: return "SEEK";
		case 003: return "RECAL";
		case 004: return "DCLR";
		case 005: return "RELEASE";
		case 006: return "OFFSET";
		case 007: return "RETURN";
		case 010: return "PRESET";
		case 011: return "PACK";
		case 014: return "SEARCH";
		case 030: return "WRITE";
		case 031: return "WRITEH";
		case 034: return "READ";
		case 035: return "READH";
		default:  return "UNKNOWN";
	}
}

static uint16_t rp06_drive_type(const bool is_rp07)
{
	return is_rp07 ? 020042 : 020022;  // RP07 / RP06
}

static uint16_t rp06_selected_drive_bit(const uint16_t cs2)
{
	return uint16_t(1u << (cs2 & 07));
}

static uint16_t rp06_drive_status(const bool operator_stopped, const bool volume_valid)
{
	uint16_t ds = default_DS;
	if (!volume_valid)
		ds &= ~uint16_t(rp06::ds_bits::VV);
	if (operator_stopped)
		ds &= ~(uint16_t(rp06::ds_bits::MOL) | uint16_t(rp06::ds_bits::DRY));
	return ds;
}

static unsigned rp06_cylinders(const bool is_rp07)
{
	return is_rp07 ? 630u : 815u;
}

static bool rp06_valid_address(const bool is_rp07, const uint16_t dc,
			       const uint16_t da)
{
	const uint32_t cyl  = dc & 01777;
	const uint32_t head = (da >> 8) & 0377;
	const uint32_t sec  = da & 0377;
	return cyl < rp06_cylinders(is_rp07) && head < NTRAC && sec < NSECT;
}

static bool rp06_last_block(const bool is_rp07, const uint16_t dc,
			    const uint16_t da)
{
	const uint32_t cyl  = dc & 01777;
	const uint32_t head = (da >> 8) & 0377;
	const uint32_t sec  = da & 0377;
	return cyl == rp06_cylinders(is_rp07) - 1u &&
	       head == NTRAC - 1u && sec == NSECT - 1u;
}

static uint16_t rp06_le_word(const uint8_t *const p)
{
	return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}

rp06::rp06(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity, const bool is_rp07) :
	b(b),
	is_rp07(is_rp07),
	disk_read_activity (disk_read_activity ),
	disk_write_activity(disk_write_activity)
{
	if (is_rp07) {
		NSECT = 50;
		NTRAC = 32;
	}
}

rp06::~rp06()
{
}

void rp06::begin()
{
	reset(true);
}

void rp06::reset(const bool hard)
{
	if (hard) {
		memset(registers, 0x00, sizeof registers);
		registers[reg_num(RP06_CS1)] = uint16_t(rp06::cs1_bits::RDY);
		operator_stopped = false;
		volume_valid = true;
		report_dva_after_start = false;
		last_block_status = false;
		bad_header_valid = false;
		bad_header_dc = 0;
		bad_header_da = 0;
		pack_ack_transient_ticks = 0;
		registers[reg_num(RP06_DS)] = rp06_drive_status(operator_stopped, volume_valid);
		registers[reg_num(RP06_AS)] = 000001;
		registers[reg_num(RP06_DT)] = rp06_drive_type(is_rp07);
		registers[reg_num(RP06_SN)] = 000001;
		int_cnt = 0;
		deferred_active = false;
		deferred_delay = -1;
		deferred_cs1_polls = 0;
		deferred_wc_polls = 0;
		control_active = false;
		control_delay = -1;
		la_sector = 0;
		if (b && b->getCpu())
			b->getCpu()->unqueue_interrupt(5, 0254);
	}
}

FLASHMEM void rp06::show_state(console *const cnsl) const
{
	cnsl->put_string_lf(format("mode: %s", is_rp07 ? "rp07": "rp06"));
	for(int i=0; i<32; i += 4)
		cnsl->put_string_lf(format("reg %2d: %06o %06o %06o %06o", i,
					registers[i + 0], registers[i + 1], registers[i + 2], registers[i + 3]));
	cnsl->put_string_lf(format("offset: %u", compute_offset()));
	cnsl->put_string_lf(format("total interrupts: %u, forwarded: %u", int_cnt_total, int_cnt));
	show_disk_backends(cnsl);
}

#if IS_POSIX
JsonDocument rp06::serialize() const
{
	JsonDocument j;
	j["is-rp07"] = is_rp07;
	return j;
}

rp06 *rp06::deserialize(const JsonVariantConst j, bus *const b)
{
	rp06 *r = new rp06(b, nullptr, nullptr,  j["is-rp07"].as<bool>());
	r->begin();
	return r;
}
#endif

uint8_t rp06::read_byte(const uint16_t addr)
{
	uint16_t v = read_word(addr & ~1);

	if (addr & 1)
		return v >> 8;

	return v;
}

uint16_t rp06::read_word(const uint16_t addr)
{
	const int reg   = reg_num(addr);
	uint16_t  value = registers[reg];

	if (operator_stopped && addr != RP06_DT && addr != RP06_SN) {
		if (addr == RP06_CS2)
			value = (value & 017) | CS2_IR;
		DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
		rp06_trace_read_dev(b, addr, value,
		   "READ-OFFLINE %-4s @ %06o -> %06o", rp06_reg_name(reg), addr, value);
		return value;
	}

	if (addr == RP06_CS1) {
		value = registers[reg_num(RP06_CS1)];
		if (control_active) {
			if (control_delay <= 0) {
				complete_deferred_control_command();
				value = registers[reg_num(RP06_CS1)];
			} else {
				control_delay--;
				value = registers[reg_num(RP06_CS1)] |
					uint16_t(rp06::cs1_bits::RDY) |
					uint16_t(rp06::cs1_bits::GO) | CS1_DVA;
				DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
				return value;
			}
		} else if (deferred_active) {
			deferred_cs1_polls++;
			if (deferred_delay < 0)
				deferred_delay = rp06_deferred_completion_instructions;
			// Boot ROM: tight tstb CS1 loop (complete after a couple polls).
			// RSX secondary: may peek CS1 once before TST WC / BNE success;
			// do not complete on that first peek or WC is cleared too early.
			const bool bootrom_style =
				deferred_wc_polls == 0 && deferred_cs1_polls >= 2;
			const bool after_wc_wait =
				deferred_wc_polls >= 1;
			if (deferred_delay <= 0 && (bootrom_style || after_wc_wait)) {
				complete_deferred_data_command();
				value = registers[reg_num(RP06_CS1)];
			} else {
				value = (registers[reg_num(RP06_CS1)] &
					 ~uint16_t(rp06::cs1_bits::RDY)) |
					uint16_t(rp06::cs1_bits::GO) | CS1_DVA;
				if (deferred_cs1_polls == 1 || deferred_delay <= 0 ||
				    (deferred_cs1_polls % 64) == 0) {
					rp06_trace_dev(b, "DEFER-BUSY fnc=%02o (%s) cs1_poll=%d wc_poll=%d delay=%d CS1=%06o",
						   deferred_fnc, rp06_func_name(deferred_fnc),
						   deferred_cs1_polls, deferred_wc_polls,
						   deferred_delay, value);
				}
				DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
				return value;
			}
		}
		if (registers[reg_num(RP06_ERRREG1)] ||
		    registers[reg_num(RP06_ER2)] ||
		    registers[reg_num(RP06_ER3)] ||
		    (registers[reg_num(RP06_CS1)] & uint16_t(rp06::cs1_bits::TRE))) {
			value |= CS1_SC;
		}
		if (report_dva_after_start)
			value |= CS1_DVA;
		if (pack_ack_transient_ticks > 0)
			value |= CS1_IR;
	} else if (addr == RP06_WC) {
		value = registers[reg_num(RP06_WC)];
		if (deferred_active) {
			// RSX secondary: TST WC / BNE done / SOB timeout.
			// Success is WC != 0, so keep the programmed count and do not
			// complete here — completion follows via CS1 or service_deferred.
			deferred_wc_polls++;
			if (deferred_delay < 0)
				deferred_delay = rp06_deferred_completion_instructions;
			value = deferred_wc ? deferred_wc : registers[reg_num(RP06_WC)];
			if (deferred_wc_polls == 1 || (deferred_wc_polls % 64) == 0) {
				rp06_trace_dev(b, "DEFER-BUSY-WC fnc=%02o (%s) cs1_poll=%d wc_poll=%d delay=%d WC=%06o",
					   deferred_fnc, rp06_func_name(deferred_fnc),
					   deferred_cs1_polls, deferred_wc_polls,
					   deferred_delay, value);
			}
			DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
			return value;
		}
	} else if (addr == RP06_DS) {
		value = rp06_drive_status(operator_stopped, volume_valid);
		if (deferred_active || control_active)
			value &= ~uint16_t(rp06::ds_bits::DRY);
		if (last_block_status ||
		    rp06_last_block(is_rp07, registers[reg_num(RP06_DC)],
				    registers[reg_num(RP06_DA)]))
			value |= DS_LST;
		if (registers[reg_num(RP06_ERRREG1)] ||
		    registers[reg_num(RP06_ER2)] ||
		    registers[reg_num(RP06_ER3)] ||
		    (registers[reg_num(RP06_CS1)] & uint16_t(rp06::cs1_bits::TRE)))
			value |= DS_ERR;
		if (pack_ack_transient_ticks <= 0 &&
		    (registers[reg_num(RP06_AS)] & rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]))) {
			value |= DS_ATA;
		}
		registers[reg_num(RP06_DS)] = value;
	} else if (addr == RP06_DT) {
		value = rp06_drive_type(is_rp07);
		registers[reg_num(RP06_DT)] = value;
	} else if (addr == RP06_SN) {
		value = registers[reg_num(RP06_SN)] ? registers[reg_num(RP06_SN)] : 000001;
	} else if (addr == RP06_RMLA) {
		// SIMH/RH: look-ahead is (sector under head) << 6. Do not echo
		// writes — RSX's pre-READ "MOV #1,LA" poll expects rotation, and
		// echoing 1 made it take a different (hanging) path.
		la_sector = (la_sector + 1) % (int)NSECT;
		value = uint16_t(la_sector << 6);
		registers[reg_num(RP06_RMLA)] = value;
	}

	DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
	rp06_trace_read_dev(b, addr, value,
		   "READ %-4s @ %06o -> %06o CS1=%06o WC=%06o BA=%06o DA=%06o CS2=%06o DS=%06o DT=%06o AS=%06o DC=%06o",
		   rp06_reg_name(reg), addr, value,
		   registers[reg_num(RP06_CS1)],
		   registers[reg_num(RP06_WC)],
		   registers[reg_num(RP06_UBA)],
		   registers[reg_num(RP06_DA)],
		   registers[reg_num(RP06_CS2)],
		   registers[reg_num(RP06_DS)],
		   rp06_drive_type(is_rp07),
		   registers[reg_num(RP06_AS)],
		   registers[reg_num(RP06_DC)]);

	return value;
}

uint16_t rp06::peek_word(const uint16_t addr) const
{
	if (addr < RP06_BASE || addr >= RP06_END || (addr & 1))
		return 0;
	if (operator_stopped)
		return registers[reg_num(addr)];
	if (addr == RP06_CS1) {
		uint16_t cs1 = registers[reg_num(RP06_CS1)];
		if (registers[reg_num(RP06_ERRREG1)] ||
		    registers[reg_num(RP06_ER2)] ||
		    registers[reg_num(RP06_ER3)] ||
		    (cs1 & uint16_t(rp06::cs1_bits::TRE))) {
			cs1 |= CS1_SC;
		}
		if (report_dva_after_start)
			cs1 |= CS1_DVA;
		if (pack_ack_transient_ticks > 0)
			cs1 |= CS1_IR;
		return cs1;
	}
	if (addr == RP06_DS) {
		uint16_t ds = rp06_drive_status(operator_stopped, volume_valid);
		if (deferred_active || control_active)
			ds &= ~uint16_t(rp06::ds_bits::DRY);
		if (last_block_status ||
		    rp06_last_block(is_rp07, registers[reg_num(RP06_DC)],
				    registers[reg_num(RP06_DA)]))
			ds |= DS_LST;
		if (registers[reg_num(RP06_ERRREG1)] ||
		    registers[reg_num(RP06_ER2)] ||
		    registers[reg_num(RP06_ER3)] ||
		    (registers[reg_num(RP06_CS1)] & uint16_t(rp06::cs1_bits::TRE)))
			ds |= DS_ERR;
		if (pack_ack_transient_ticks <= 0 &&
		    (registers[reg_num(RP06_AS)] & rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]))) {
			ds |= DS_ATA;
		}
		return ds;
	}
	return registers[reg_num(addr)];
}

int rp06::reg_num(uint16_t addr) const
{
	return (addr - RP06_BASE) / 2;
}

void rp06::write_byte(const uint16_t addr, const uint8_t v)
{
	const uint16_t word_addr = addr & ~1;
	uint16_t vtemp = registers[reg_num(word_addr)];

	if (addr & 1) {
		vtemp &= 0x00ff;
		vtemp |= v << 8;
	}
	else {
		vtemp &= 0xff00;
		vtemp |= v;
	}

	write_word(word_addr, vtemp);
}

void rp06::set_operator_stop(const bool stopped)
{
	operator_stopped = stopped;
	if (!stopped) {
		volume_valid = false;
		report_dva_after_start = true;
		registers[reg_num(RP06_CS1)] |= uint16_t(rp06::cs1_bits::RDY);
		registers[reg_num(RP06_AS)] |= rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
	} else {
		registers[reg_num(RP06_AS)] &= ~rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
	}
	registers[reg_num(RP06_DS)] = rp06_drive_status(operator_stopped, volume_valid);
	if (b && b->getCpu())
		b->getCpu()->unqueue_interrupt(5, 0254);
	rp06_trace_dev(b, "OPERATOR-%s CS1=%06o CS2=%06o DS=%06o AS=%06o ER1=%06o",
		       stopped ? "STOP" : "START",
		       registers[reg_num(RP06_CS1)],
		       registers[reg_num(RP06_CS2)],
		       registers[reg_num(RP06_DS)],
		       registers[reg_num(RP06_AS)],
		       registers[reg_num(RP06_ERRREG1)]);
}

uint32_t rp06::compute_offset() const
{
	return compute_offset_from(registers[reg_num(RP06_DC)], registers[reg_num(RP06_DA)]);
}

uint32_t rp06::compute_offset_from(uint16_t dc, uint16_t da) const
{
	uint16_t cn = dc;
	uint16_t tn = (da >> 8) & 0377;
	uint16_t sn = da & 0377;
	uint32_t offs = cn * NSECT * NTRAC;
	offs += tn * NSECT;
	offs += sn;
	offs *= SECTOR_SIZE;
	return offs;
}

// SIMH/RH: after a data xfer, DA/DC advance by the number of sectors moved
// so multi-block loaders that only bump BA/WC still stream consecutive disk.
static void rp06_advance_disk_address(uint16_t &dc, uint16_t &da, uint32_t sectors)
{
	uint32_t cyl  = dc & 01777;
	uint32_t head = (da >> 8) & 037;
	uint32_t sec  = da & 077;
	sec += sectors;
	while (sec >= NSECT) {
		sec -= NSECT;
		head++;
		if (head >= NTRAC) {
			head = 0;
			cyl++;
		}
	}
	dc = uint16_t(cyl & 01777);
	da = uint16_t((head << 8) | sec);
}

uint32_t rp06::getphysaddr() const
{
	return getphysaddr_from(registers[reg_num(RP06_CS1)],
				registers[reg_num(RP06_UBA)],
				registers[reg_num(RP06_BAE)]);
}

uint32_t rp06::getphysaddr_from(uint16_t cs1, uint16_t ba, uint16_t bae) const
{
	bool cur_A16 = cs1 & uint16_t(rp06::cs1_bits::A16);
	bool cur_A17 = cs1 & uint16_t(rp06::cs1_bits::A17);
	uint16_t cur_A1621 = 0;
	if (bae) {
		cur_A16 = false;
		cur_A17 = false;
		cur_A1621 = bae & 077;
	}
	return ba | (cur_A16 << 16) | (cur_A17 << 17) | (cur_A1621 << 16);
}

void rp06::defer_data_command(uint16_t fnc, uint16_t cs1_after_go)
{
	deferred_active = true;
	deferred_fnc = fnc;
	deferred_cs1 = cs1_after_go;
	deferred_wc = registers[reg_num(RP06_WC)];
	deferred_ba = registers[reg_num(RP06_UBA)];
	deferred_da = registers[reg_num(RP06_DA)];
	deferred_dc = registers[reg_num(RP06_DC)];
	deferred_bae = registers[reg_num(RP06_BAE)];
	deferred_delay = rp06_deferred_completion_instructions;
	deferred_cs1_polls = 0;
	deferred_wc_polls = 0;
	registers[reg_num(RP06_CS1)] = cs1_after_go & ~uint16_t(rp06::cs1_bits::RDY);
	rp06_trace_dev(b, "DEFER-DATA fnc=%02o (%s) delay=%d WC=%06o BA=%06o DA=%06o DC=%06o",
		   fnc, rp06_func_name(fnc), deferred_delay,
		   deferred_wc, deferred_ba, deferred_da, deferred_dc);
}

void rp06::defer_control_command(uint16_t fnc, uint16_t cs1_after_go)
{
	control_active = true;
	control_fnc = fnc;
	control_cs1 = cs1_after_go | uint16_t(rp06::cs1_bits::RDY) |
		uint16_t(rp06::cs1_bits::GO);
	control_delay = rp06_deferred_completion_instructions;
	registers[reg_num(RP06_CS1)] = control_cs1;
	rp06_trace_dev(b, "DEFER-CTRL fnc=%02o (%s) delay=%d CS1=%06o DA=%06o DC=%06o",
		       fnc, rp06_func_name(fnc), control_delay,
		       registers[reg_num(RP06_CS1)],
		       registers[reg_num(RP06_DA)],
		       registers[reg_num(RP06_DC)]);
}

void rp06::complete_deferred_control_command()
{
	if (!control_active)
		return;
	const uint16_t fnc = control_fnc;
	const uint16_t cs1_start = control_cs1;
	control_active = false;
	control_delay = -1;

	if (fnc == 014)
		registers[reg_num(RP06_CC)] = registers[reg_num(RP06_DC)];

	registers[reg_num(RP06_AS)] |= rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
	uint16_t cs1 = cs1_start &
		(uint16_t(rp06::cs1_bits::FN) | uint16_t(rp06::cs1_bits::IE));
	cs1 |= uint16_t(rp06::cs1_bits::RDY);
	registers[reg_num(RP06_CS1)] = cs1;
	int_cnt_total++;
	if (cs1 & uint16_t(rp06::cs1_bits::IE)) {
		registers[reg_num(RP06_CS1)] =
			(registers[reg_num(RP06_CS1)] & ~uint16_t(rp06::cs1_bits::IE)) |
			uint16_t(rp06::cs1_bits::RDY) | CS1_IR;
		int_cnt++;
		b->getCpu()->queue_interrupt(5, 0254);
	}
}

void rp06::complete_deferred_data_command()
{
	if (!deferred_active)
		return;

	const uint16_t fnc = deferred_fnc;
	const bool is_read = (fnc == 034 || fnc == 035);
	deferred_active = false;
	deferred_delay = -1;

	if (is_read) {
		if (disk_read_activity)
			*disk_read_activity = true;
	} else {
		if (disk_write_activity)
			*disk_write_activity = true;
	}

	uint32_t offs = compute_offset_from(deferred_dc, deferred_da);
	// RH70: BA+BAE increments as the 22-bit physical DMA address (not Unibus map).
	uint32_t ba_reg = getphysaddr_from(deferred_cs1, deferred_ba, deferred_bae);
	uint32_t nw = 65536u - deferred_wc;
	uint32_t nb = nw * 2u;
	const uint32_t dma_pa0 = rp06_rh70_dma_pa(ba_reg);
	const bool header_command = (fnc == 031 || fnc == 035);
	const bool header_mismatch =
		bad_header_valid &&
		bad_header_dc == deferred_dc &&
		bad_header_da == deferred_da &&
		fnc != 031;
	const bool invalid_address =
		!rp06_valid_address(is_rp07, deferred_dc, deferred_da) ||
		header_mismatch;

	rp06_trace_dev(b, "%s-DATA bytes=%u disk_off=%u ba=%08o pa=%08o cyl=%u trk=%u sec=%u",
		   is_read ? "READ" : "WRITE",
		   (unsigned)nb, (unsigned)offs, (unsigned)ba_reg, (unsigned)dma_pa0,
		   (unsigned)deferred_dc,
		   (unsigned)((deferred_da >> 8) & 0377),
		   (unsigned)(deferred_da & 0377));

	uint8_t xfer_buffer[SECTOR_SIZE] { };
	uint32_t end_offset = offs + nb;
	bool xfer_ok = !fhs.empty() && !invalid_address;
	if (invalid_address) {
		registers[reg_num(RP06_ERRREG1)] |= ER1_HCRC;
		registers[reg_num(RP06_CS2)] |= CS2_IR;
		rp06_trace_dev(b, "ADDR-ERR fnc=%02o cyl=%u trk=%u sec=%u header_mismatch=%d",
			       fnc, (unsigned)(deferred_dc & 01777),
			       (unsigned)((deferred_da >> 8) & 0377),
			       (unsigned)(deferred_da & 0377),
			       header_mismatch ? 1 : 0);
	}
	for (uint32_t cur_offset = offs; cur_offset < end_offset; cur_offset += SECTOR_SIZE) {
		if (!xfer_ok)
			break;
		uint32_t cur_n = std::min(end_offset - cur_offset, uint32_t(SECTOR_SIZE));
		const uint32_t pa = rp06_rh70_dma_pa(ba_reg);
		if (is_read) {
			if (fhs.empty() || !fhs.at(0)->read(cur_offset, cur_n, xfer_buffer, SECTOR_SIZE)) {
				rp06_trace_dev(b, "READ-ERR disk_off=%u len=%u", (unsigned)cur_offset, (unsigned)cur_n);
				xfer_ok = false;
				break;
			}
			if (header_command && cur_offset == offs && cur_n >= 16) {
				// RH read-header transfers expose a four-word sector
				// header first, then the flat sector data body.
				uint8_t sector_data[SECTOR_SIZE];
				memcpy(sector_data, xfer_buffer, cur_n);
				const uint16_t header[] = {
					uint16_t(010000 | (deferred_dc & 01777)),
					uint16_t(deferred_da & 017777),
					0,
					0
				};
				for (unsigned i = 0; i < sizeof(header) / sizeof(header[0]); i++) {
					const uint16_t word = header[i];
					xfer_buffer[i * 2] = uint8_t(word & 0377);
					xfer_buffer[i * 2 + 1] = uint8_t(word >> 8);
				}
				const uint32_t header_bytes = sizeof(header);
				if (cur_n > header_bytes)
					memcpy(xfer_buffer + header_bytes, sector_data,
					       cur_n - header_bytes);
			}
			const uint32_t copied = b->write_physical_block(pa, xfer_buffer, cur_n);
			if (copied != cur_n) {
				rp06_trace_dev(b, "READ-DMA-ERR ba=%08o pa=%08o requested=%u copied=%u",
					   (unsigned)ba_reg, (unsigned)pa,
					   (unsigned)cur_n, (unsigned)copied);
				xfer_ok = false;
				ba_reg += copied;
				break;
			}
			ba_reg += cur_n;
		} else {
			const uint32_t copied = b->read_physical_block(pa, xfer_buffer, cur_n);
			if (copied != cur_n) {
				rp06_trace_dev(b, "WRITE-DMA-ERR ba=%08o pa=%08o requested=%u copied=%u",
					   (unsigned)ba_reg, (unsigned)pa,
					   (unsigned)cur_n, (unsigned)copied);
				xfer_ok = false;
				ba_reg += copied;
				break;
			}
			ba_reg += cur_n;
			if (header_command && cur_offset == offs && cur_n >= 8) {
				const uint16_t header[] = {
					rp06_le_word(&xfer_buffer[0]),
					rp06_le_word(&xfer_buffer[2]),
					rp06_le_word(&xfer_buffer[4]),
					rp06_le_word(&xfer_buffer[6])
				};
				const uint16_t expected[] = {
					uint16_t(010000 | (deferred_dc & 01777)),
					uint16_t(deferred_da & 017777),
					0,
					0
				};
				const bool bad_header =
					header[0] != expected[0] || header[1] != expected[1] ||
					header[2] != expected[2] || header[3] != expected[3];
				if (bad_header) {
					bad_header_valid = true;
					bad_header_dc = deferred_dc;
					bad_header_da = deferred_da;
				} else if (bad_header_valid &&
					   bad_header_dc == deferred_dc &&
					   bad_header_da == deferred_da) {
					bad_header_valid = false;
				}
			}
			const uint8_t *write_buffer = xfer_buffer;
			uint32_t write_n = cur_n;
			if (header_command && cur_offset == offs) {
				// The SD image is data-only. WRITE HEADER transfers carry a
				// four-word sector header before the data body; do not persist
				// those header bytes into the flat image.
				const uint32_t header_bytes = 8;
				if (write_n > header_bytes) {
					write_buffer += header_bytes;
					write_n -= header_bytes;
				} else {
					write_n = 0;
				}
			}
			if (write_n && (fhs.empty() || !fhs.at(0)->write(cur_offset, write_n, write_buffer, SECTOR_SIZE))) {
				rp06_trace_dev(b, "WRITE-ERR disk_off=%u len=%u", (unsigned)cur_offset, (unsigned)cur_n);
				xfer_ok = false;
				break;
			}
		}
	}

	const bool partial_header_write = header_mismatch && fnc == 030;
	registers[reg_num(RP06_WC)] =
		xfer_ok ? 0 :
		partial_header_write ? uint16_t(deferred_wc + 1) :
		deferred_wc;
	registers[reg_num(RP06_UBA)] =
		xfer_ok ? uint16_t(ba_reg) :
		partial_header_write ? uint16_t(deferred_ba + 2) :
		deferred_ba;
	registers[reg_num(RP06_BAE)] = xfer_ok ? uint16_t((ba_reg >> 16) & 077) : deferred_bae;
	// Advance desired disk address (RSX secondary load loop relies on this).
	uint16_t da = deferred_da;
	uint16_t dc = deferred_dc;
	const uint32_t sectors_done = (nb + SECTOR_SIZE - 1u) / SECTOR_SIZE;
	last_block_status = xfer_ok && rp06_last_block(is_rp07, deferred_dc, deferred_da);
	if (xfer_ok || partial_header_write) {
		rp06_advance_disk_address(dc, da, sectors_done);
	}
	registers[reg_num(RP06_DA)] = da;
	registers[reg_num(RP06_DC)] = dc;
	registers[reg_num(RP06_CC)] = deferred_dc;
	// Keep IE/function; drop GO; set RDY (and TRE on error).
	// Refresh A16/A17 from the final address (BAE holds bits 16..21).
	uint16_t cs1 = deferred_cs1 &
		(uint16_t(rp06::cs1_bits::FN) | uint16_t(rp06::cs1_bits::IE));
	if (ba_reg & (1u << 16))
		cs1 |= uint16_t(rp06::cs1_bits::A16);
	if (ba_reg & (1u << 17))
		cs1 |= uint16_t(rp06::cs1_bits::A17);
	cs1 |= uint16_t(rp06::cs1_bits::RDY);
	if (!xfer_ok)
		cs1 |= uint16_t(rp06::cs1_bits::TRE);
	registers[reg_num(RP06_CS1)] = cs1;
	registers[reg_num(RP06_AS)] |= rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
	rp06_trace_dev(b, "%s-DONE ok=%d CS1=%06o BA=%06o BAE=%06o DA=%06o DC=%06o",
		   is_read ? "READ" : "WRITE",
		   xfer_ok ? 1 : 0, cs1,
		   registers[reg_num(RP06_UBA)],
		   registers[reg_num(RP06_BAE)],
		   da, dc);
	if (is_read && deferred_ba < 010000 && rp06_trace_left > 0) {
		// Dump a few words of what landed in low memory for boot diagnosis.
		auto word_at = [&](uint16_t addr) -> uint16_t {
			uint16_t lo = b->read_unibus_byte(addr);
			uint16_t hi = b->read_unibus_byte(addr + 1);
			return uint16_t(lo | (hi << 8));
		};
		rp06_trace_dev(b, "READ-DUMP @0=%06o @100=%06o @200=%06o @270=%06o @300=%06o",
			   word_at(0), word_at(0100), word_at(0200),
			   word_at(0270), word_at(0300));
	}

	int_cnt_total++;
	if (cs1 & uint16_t(rp06::cs1_bits::IE)) {
		registers[reg_num(RP06_CS1)] =
			(registers[reg_num(RP06_CS1)] & ~uint16_t(rp06::cs1_bits::IE)) |
			uint16_t(rp06::cs1_bits::RDY) | CS1_IR;
		int_cnt++;
		b->getCpu()->queue_interrupt(5, 0254);
		rp06_trace_dev(b, "IRQ-QUEUE vec=254 BR5 reason=data-done CS1=%06o AS=%06o DS=%06o ER1=%06o",
			       registers[reg_num(RP06_CS1)],
			       registers[reg_num(RP06_AS)],
			       registers[reg_num(RP06_DS)],
			       registers[reg_num(RP06_ERRREG1)]);
	}
}

void rp06::service_deferred()
{
	if (pack_ack_transient_ticks > 0)
		pack_ack_transient_ticks--;
	if (control_active) {
		if (control_delay < 0)
			control_delay = rp06_deferred_completion_instructions;
		if (control_delay > 0) {
			control_delay--;
			return;
		}
		complete_deferred_control_command();
	}
	if (!deferred_active)
		return;
	if (deferred_delay < 0)
		deferred_delay = rp06_deferred_completion_instructions;
	if (deferred_delay > 0) {
		deferred_delay--;
		return;
	}
	// DMA time elapsed. Complete even if the guest never polled CS1/WC —
	// IE-driven I/O waits for RDY/IRQ only. (Earlier poll gating hung that
	// path once we stopped canceling busy GOs.)
	complete_deferred_data_command();
}

void rp06::write_word(const uint16_t addr, uint16_t v)
{
	const int reg = reg_num(addr);

	DOLOG(log_ss::LS_DISK, "RP06: write \"%s\"/%06o: %06o", rp06_reg_name(reg), addr, v);
	rp06_trace_dev(b, "WRITE %-4s @ %06o <- %06o CS1=%06o CS2=%06o DS=%06o AS=%06o ER1=%06o",
		       rp06_reg_name(reg), addr, v,
		       registers[reg_num(RP06_CS1)],
		       registers[reg_num(RP06_CS2)],
		       registers[reg_num(RP06_DS)],
		       registers[reg_num(RP06_AS)],
		       registers[reg_num(RP06_ERRREG1)]);

	// SIMH: while a data transfer is active, register writes (except AS)
	// are refused with ER1 RMR — they must not abort/replace the xfer.
	if (deferred_active && addr != RP06_AS) {
		registers[reg_num(RP06_ERRREG1)] |= ER1_RMR;
#if defined(ESP32)
		static int rmr_logs = 0;
		if (rmr_logs++ < 16) {
			LOG("kek RP06 RMR busy-reject reg=%s <- %06o (defer fnc=%02o delay=%d)",
			    rp06_reg_name(reg), v, deferred_fnc, deferred_delay);
		}
#endif
		rp06_trace_dev(b, "RMR busy-reject %-4s <- %06o", rp06_reg_name(reg), v);
		return;
	}

	if (operator_stopped) {
		// Diskless diagnostics take the drive offline and then verify raw
		// register read/write paths. Keep STOP asserted and let CS2 CLR test
		// as data here; a hard reset would put MOL/DRY back online mid-test.
		if (addr != RP06_DT && addr != RP06_SN)
			registers[reg] = v;
		return;
	}

	if (addr == RP06_AS) {
		// Write-1-to-clear attention bits.
		registers[reg_num(RP06_AS)] &= uint16_t(~v);
		return;
	}

	if (addr == RP06_CS2) {
		if (v & CS2_CLR) {
			const bool keep_dva_after_start =
				report_dva_after_start ||
				(registers[reg_num(RP06_AS)] & rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]));
			const uint16_t keep_cc = registers[reg_num(RP06_CC)];
			reset(true);
			report_dva_after_start = keep_dva_after_start;
			registers[reg_num(RP06_CC)] = keep_cc;
			rp06_trace_dev(b, "CS2 CLR -> controller reset");
			return;
		}
		registers[reg_num(RP06_CS2)] = v & 017;
		return;
	}

	if (addr == RP06_DT || addr == RP06_SN) {
		// Drive type / serial are read-only from the guest's perspective.
		return;
	}

	if (addr != RP06_CS1) {
		registers[reg] = v;
		if (addr == RP06_DA || addr == RP06_DC)
			last_block_status = false;
		if (addr == RP06_DC)
			registers[reg_num(RP06_CC)] = v;
		if (addr == RP06_ERRREG1 && v != 0) {
			registers[reg_num(RP06_AS)] |= rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
			registers[reg_num(RP06_CS1)] |= uint16_t(rp06::cs1_bits::TRE);
		}
		return;
	}

	// Capture prior CS1 before applying the write so clrb/clr cannot
	// accidentally clear sticky RDY/TRE by reading the new value as "prev".
	const uint16_t prev = registers[reg_num(RP06_CS1)];
	registers[reg_num(RP06_CS1)] =
		(prev & (uint16_t(rp06::cs1_bits::RDY) | uint16_t(rp06::cs1_bits::TRE))) |
		(v & ~(uint16_t(rp06::cs1_bits::RDY) | uint16_t(rp06::cs1_bits::TRE)));

	if (!(v & uint16_t(rp06::cs1_bits::GO))) {
		// Enabling IE with attention already pending should raise IRQ
		// (SIMH: interrupt on IE transition while ATA/SC pending).
		if ((v & uint16_t(rp06::cs1_bits::IE)) &&
		    (registers[reg_num(RP06_AS)] & 000001) &&
		    !(prev & uint16_t(rp06::cs1_bits::IE))) {
			int_cnt_total++;
			int_cnt++;
			registers[reg_num(RP06_CS1)] =
				(registers[reg_num(RP06_CS1)] & ~uint16_t(rp06::cs1_bits::IE)) |
				uint16_t(rp06::cs1_bits::RDY) | CS1_IR;
			b->getCpu()->queue_interrupt(5, 0254);
			rp06_trace_dev(b, "IRQ-QUEUE vec=254 BR5 reason=IE+ATA CS1=%06o AS=%06o DS=%06o ER1=%06o",
				       registers[reg_num(RP06_CS1)],
				       registers[reg_num(RP06_AS)],
				       registers[reg_num(RP06_DS)],
				       registers[reg_num(RP06_ERRREG1)]);
		}
		else if (v & uint16_t(rp06::cs1_bits::IE)) {
			rp06_trace_dev(b, "IRQ-NOQUEUE reason=IE-no-ATA prev=%06o write=%06o CS1=%06o AS=%06o DS=%06o ER1=%06o",
				       prev, v,
				       registers[reg_num(RP06_CS1)],
				       registers[reg_num(RP06_AS)],
				       registers[reg_num(RP06_DS)],
				       registers[reg_num(RP06_ERRREG1)]);
		}
		return;
	}

	// New GO while idle. Drop any stale done-interrupt from the prior xfer.
	if (b && b->getCpu())
		b->getCpu()->unqueue_interrupt(5, 0254);
	rp06_trace_dev(b, "IRQ-UNQUEUE vec=254 BR5 reason=new-go");

	bool generate_interrupt = false;
	const uint16_t fnc = (v >> 1) & 037;  // Massbus function field

	registers[reg_num(RP06_CS1)] &=
		~(uint16_t(rp06::cs1_bits::GO) | uint16_t(rp06::cs1_bits::RDY) |
		  uint16_t(rp06::cs1_bits::TRE));

	// SIMH rp_clr_as: starting most commands clears this drive's attention.
	// The functional diagnostic expects PACK ACK to acknowledge VV while
	// leaving the post-spinup attention visible in AS/RHDS.ATA.
	if (fnc != 011)
		registers[reg_num(RP06_AS)] &= ~rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);

	rp06_trace_dev(b, "CMD fnc=%02o (%s) CS1=%06o WC=%06o BA=%06o DA=%06o DC=%06o AS=%06o slots=%u",
		   fnc, rp06_func_name(fnc),
		   registers[reg_num(RP06_CS1)],
		   registers[reg_num(RP06_WC)],
		   registers[reg_num(RP06_UBA)],
		   registers[reg_num(RP06_DA)],
		   registers[reg_num(RP06_DC)],
		   registers[reg_num(RP06_AS)],
		   (unsigned)fhs.size());

	auto finish_ok = [&](bool set_attention) {
		registers[reg_num(RP06_CS1)] |= uint16_t(rp06::cs1_bits::RDY);
		if (set_attention)
			registers[reg_num(RP06_AS)] |= rp06_selected_drive_bit(registers[reg_num(RP06_CS2)]);
		generate_interrupt = true;
	};

	// Attention rules match SIMH pdp11_rp.c:
	//   DCLR/RELEASE/NOP/PRESET/PACK complete without ATA.
	//   SEEK/RECAL/OFFSET/RETURN/UNLOAD/SEARCH set ATA on completion.
	if (fnc == 000) {
		// NOP
		finish_ok(false);
	} else if (fnc == 004 || fnc == 005) {
		// DCLR / RELEASE — clear error regs; do not raise ATA.
		registers[reg_num(RP06_ERRREG1)] = 0;
		registers[reg_num(RP06_ER2)] = 0;
		registers[reg_num(RP06_ER3)] = 0;
		registers[reg_num(RP06_EC1)] = 0;
		registers[reg_num(RP06_EC2)] = 0;
		finish_ok(false);
	} else if (fnc == 001 || fnc == 002 || fnc == 003 ||
		   fnc == 006 || fnc == 007) {
		// UNLOAD / SEEK / RECAL / OFFSET / RETURN
		finish_ok(true);
	} else if (fnc == 010 || fnc == 011) {
		// PRESET / PACK ACK — volume valid; no ATA (SIMH returns early).
		volume_valid = true;
		registers[reg_num(RP06_DS)] = rp06_drive_status(operator_stopped, volume_valid);
		if (fnc == 011)
			pack_ack_transient_ticks = 6;
		registers[reg_num(RP06_DA)] = 0;
		registers[reg_num(RP06_DC)] = 0;
		registers[reg_num(RP06_CC)] = 0;
		finish_ok(false);
	} else if (fnc == 014) {
		// SEARCH
		defer_control_command(fnc, registers[reg_num(RP06_CS1)]);
		return;
	} else if (fnc == 030 || fnc == 031 || fnc == 034 || fnc == 035) {
		// Defer DMA so BA=0 bootstraps can poll CS1 before overwrite.
		defer_data_command(fnc, registers[reg_num(RP06_CS1)]);
		return;
	} else {
		DOLOG(log_ss::LS_DISK, "RP06: command fnc=%02o not implemented", fnc);
		rp06_trace_dev(b, "CMD-UNIMP fnc=%02o (%s)", fnc, rp06_func_name(fnc));
		registers[reg_num(RP06_CS1)] |= uint16_t(rp06::cs1_bits::RDY) | uint16_t(rp06::cs1_bits::TRE);
		generate_interrupt = true;
	}

	if (generate_interrupt) {
		int_cnt_total++;
		if (registers[reg_num(RP06_CS1)] & uint16_t(rp06::cs1_bits::IE)) {
			int_cnt++;
			registers[reg_num(RP06_CS1)] =
				(registers[reg_num(RP06_CS1)] & ~uint16_t(rp06::cs1_bits::IE)) |
				uint16_t(rp06::cs1_bits::RDY) | CS1_IR;
			b->getCpu()->queue_interrupt(5, 0254);
			rp06_trace_dev(b, "IRQ-QUEUE vec=254 BR5 reason=cmd-done CS1=%06o AS=%06o DS=%06o ER1=%06o",
				       registers[reg_num(RP06_CS1)],
				       registers[reg_num(RP06_AS)],
				       registers[reg_num(RP06_DS)],
				       registers[reg_num(RP06_ERRREG1)]);
		}
		else {
			rp06_trace_dev(b, "IRQ-NOQUEUE reason=cmd-done-ie-clear CS1=%06o AS=%06o DS=%06o ER1=%06o",
				       registers[reg_num(RP06_CS1)],
				       registers[reg_num(RP06_AS)],
				       registers[reg_num(RP06_DS)],
				       registers[reg_num(RP06_ERRREG1)]);
		}
	}
}
