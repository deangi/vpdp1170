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
constexpr const uint16_t CS2_CLR = 0000040;
constexpr const uint16_t DS_ATA  = 0100000;
static constexpr int rp06_deferred_completion_instructions = 4;
static constexpr uint16_t ER1_RMR = 0000004;  // register modification refused (SIMH)

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
	LOG("kek RP06 %s", buffer);
#else
	(void)fmt;
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
	LOG("kek RP06 %s", buffer);
#else
	(void)addr; (void)value; (void)fmt;
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
		registers[reg_num(RP06_DS)] = default_DS;
		registers[reg_num(RP06_DT)] = rp06_drive_type(is_rp07);
		registers[reg_num(RP06_SN)] = 000001;
		int_cnt = 0;
		deferred_active = false;
		deferred_delay = -1;
		deferred_cs1_polls = 0;
		deferred_wc_polls = 0;
		la_sector = 0;
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

	if (addr == RP06_CS1) {
		value = registers[reg_num(RP06_CS1)];
		if (deferred_active) {
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
				value = registers[reg_num(RP06_CS1)] & ~uint16_t(rp06::cs1_bits::RDY);
				if (deferred_cs1_polls == 1 || deferred_delay <= 0 ||
				    (deferred_cs1_polls % 64) == 0) {
					rp06_trace("DEFER-BUSY fnc=%02o (%s) cs1_poll=%d wc_poll=%d delay=%d CS1=%06o",
						   deferred_fnc, rp06_func_name(deferred_fnc),
						   deferred_cs1_polls, deferred_wc_polls,
						   deferred_delay, value);
				}
				DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
				return value;
			}
		}
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
				rp06_trace("DEFER-BUSY-WC fnc=%02o (%s) cs1_poll=%d wc_poll=%d delay=%d WC=%06o",
					   deferred_fnc, rp06_func_name(deferred_fnc),
					   deferred_cs1_polls, deferred_wc_polls,
					   deferred_delay, value);
			}
			DOLOG(log_ss::LS_DISK, "RP06: read \"%s\"/%o: %06o", rp06_reg_name(reg), addr, value);
			return value;
		}
	} else if (addr == RP06_DS) {
		value = default_DS;
		if (registers[reg_num(RP06_AS)] & 000001)
			value |= DS_ATA;
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
	rp06_trace_read(addr, value,
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
	return registers[reg_num(addr)];
}

int rp06::reg_num(uint16_t addr) const
{
	return (addr - RP06_BASE) / 2;
}

void rp06::write_byte(const uint16_t addr, const uint8_t v)
{
	uint16_t vtemp = registers[reg_num(addr)];

	if (addr & 1) {
		vtemp &= 0x00ff;
		vtemp |= v << 8;
	}
	else {
		vtemp &= 0xff00;
		vtemp |= v;
	}

	write_word(addr & ~1, vtemp);
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
	rp06_trace("DEFER-DATA fnc=%02o (%s) delay=%d WC=%06o BA=%06o DA=%06o DC=%06o",
		   fnc, rp06_func_name(fnc), deferred_delay,
		   deferred_wc, deferred_ba, deferred_da, deferred_dc);
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

	rp06_trace("%s-DATA bytes=%u disk_off=%u ba=%08o pa=%08o cyl=%u trk=%u sec=%u",
		   is_read ? "READ" : "WRITE",
		   (unsigned)nb, (unsigned)offs, (unsigned)ba_reg, (unsigned)dma_pa0,
		   (unsigned)deferred_dc,
		   (unsigned)((deferred_da >> 8) & 0377),
		   (unsigned)(deferred_da & 0377));

	uint8_t xfer_buffer[SECTOR_SIZE] { };
	uint32_t end_offset = offs + nb;
	bool xfer_ok = !fhs.empty();
	for (uint32_t cur_offset = offs; cur_offset < end_offset; cur_offset += SECTOR_SIZE) {
		uint32_t cur_n = std::min(end_offset - cur_offset, uint32_t(SECTOR_SIZE));
		const uint32_t pa = rp06_rh70_dma_pa(ba_reg);
		if (is_read) {
			if (fhs.empty() || !fhs.at(0)->read(cur_offset, cur_n, xfer_buffer, SECTOR_SIZE)) {
				rp06_trace("READ-ERR disk_off=%u len=%u", (unsigned)cur_offset, (unsigned)cur_n);
				xfer_ok = false;
				break;
			}
			const uint32_t copied = b->write_physical_block(pa, xfer_buffer, cur_n);
			if (copied != cur_n) {
				rp06_trace("READ-DMA-ERR ba=%08o pa=%08o requested=%u copied=%u",
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
				rp06_trace("WRITE-DMA-ERR ba=%08o pa=%08o requested=%u copied=%u",
					   (unsigned)ba_reg, (unsigned)pa,
					   (unsigned)cur_n, (unsigned)copied);
				xfer_ok = false;
				ba_reg += copied;
				break;
			}
			ba_reg += cur_n;
			if (fhs.empty() || !fhs.at(0)->write(cur_offset, cur_n, xfer_buffer, SECTOR_SIZE)) {
				rp06_trace("WRITE-ERR disk_off=%u len=%u", (unsigned)cur_offset, (unsigned)cur_n);
				xfer_ok = false;
				break;
			}
		}
	}

	registers[reg_num(RP06_WC)] = 0;
	registers[reg_num(RP06_UBA)] = uint16_t(ba_reg);
	registers[reg_num(RP06_BAE)] = uint16_t((ba_reg >> 16) & 077);
	// Advance desired disk address (RSX secondary load loop relies on this).
	uint16_t da = deferred_da;
	uint16_t dc = deferred_dc;
	const uint32_t sectors_done = (nb + SECTOR_SIZE - 1u) / SECTOR_SIZE;
	rp06_advance_disk_address(dc, da, sectors_done);
	registers[reg_num(RP06_DA)] = da;
	registers[reg_num(RP06_DC)] = dc;
	registers[reg_num(RP06_CC)] = dc;
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
	rp06_trace("%s-DONE ok=%d CS1=%06o BA=%06o BAE=%06o DA=%06o DC=%06o",
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
		rp06_trace("READ-DUMP @0=%06o @100=%06o @200=%06o @270=%06o @300=%06o",
			   word_at(0), word_at(0100), word_at(0200),
			   word_at(0270), word_at(0300));
	}

	int_cnt_total++;
	if (cs1 & uint16_t(rp06::cs1_bits::IE)) {
		int_cnt++;
		b->getCpu()->queue_interrupt(5, 0254);
		rp06_trace("IRQ-QUEUE vec=254 BR5");
	}
}

void rp06::service_deferred()
{
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
	rp06_trace("WRITE %-4s @ %06o <- %06o", rp06_reg_name(reg), addr, v);

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
		rp06_trace("RMR busy-reject %-4s <- %06o", rp06_reg_name(reg), v);
		return;
	}

	if (addr == RP06_AS) {
		// Write-1-to-clear attention bits.
		registers[reg_num(RP06_AS)] &= uint16_t(~v);
		return;
	}

	if (addr == RP06_CS2) {
		if (v & CS2_CLR) {
			reset(true);
			rp06_trace("CS2 CLR -> controller reset");
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
			b->getCpu()->queue_interrupt(5, 0254);
			rp06_trace("IRQ-QUEUE vec=254 BR5 (IE+ATA)");
		}
		return;
	}

	// New GO while idle. Drop any stale done-interrupt from the prior xfer.
	if (b && b->getCpu())
		b->getCpu()->unqueue_interrupt(5, 0254);

	bool generate_interrupt = false;
	const uint16_t fnc = (v >> 1) & 037;  // Massbus function field

	registers[reg_num(RP06_CS1)] &=
		~(uint16_t(rp06::cs1_bits::GO) | uint16_t(rp06::cs1_bits::RDY) |
		  uint16_t(rp06::cs1_bits::TRE));

	// SIMH rp_clr_as: starting a command clears this drive's attention.
	registers[reg_num(RP06_AS)] &= ~000001;

	rp06_trace("CMD fnc=%02o (%s) CS1=%06o WC=%06o BA=%06o DA=%06o DC=%06o AS=%06o slots=%u",
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
			registers[reg_num(RP06_AS)] |= 000001;  // drive 0 attention
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
		registers[reg_num(RP06_DS)] = default_DS;
		registers[reg_num(RP06_DA)] = 0;
		registers[reg_num(RP06_DC)] = 0;
		registers[reg_num(RP06_CC)] = 0;
		finish_ok(false);
	} else if (fnc == 014) {
		// SEARCH
		registers[reg_num(RP06_CC)] = registers[reg_num(RP06_DC)];
		finish_ok(true);
	} else if (fnc == 030 || fnc == 031 || fnc == 034 || fnc == 035) {
		// Defer DMA so BA=0 bootstraps can poll CS1 before overwrite.
		defer_data_command(fnc, registers[reg_num(RP06_CS1)]);
		return;
	} else {
		DOLOG(log_ss::LS_DISK, "RP06: command fnc=%02o not implemented", fnc);
		rp06_trace("CMD-UNIMP fnc=%02o (%s)", fnc, rp06_func_name(fnc));
		registers[reg_num(RP06_CS1)] |= uint16_t(rp06::cs1_bits::RDY) | uint16_t(rp06::cs1_bits::TRE);
		generate_interrupt = true;
	}

	if (generate_interrupt) {
		int_cnt_total++;
		if (registers[reg_num(RP06_CS1)] & uint16_t(rp06::cs1_bits::IE)) {
			int_cnt++;
			b->getCpu()->queue_interrupt(5, 0254);
			rp06_trace("IRQ-QUEUE vec=254 BR5");
		}
	}
}
