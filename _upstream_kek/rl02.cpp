// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#include <errno.h>
#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "error.h"
#include "gen.h"
#include "log.h"
#include "mmu.h"
#include "rl02.h"
#include "utils.h"

#if defined(ESP32)
#include "../disk.h"
#include "../platform.h"
#endif


constexpr const char * const regnames[] = {
	"control status",
	"bus address",
	"disk address",
	"multipurpose",
	"bus address extension"
	};

constexpr const char * const commands[] = {
	"no-op",
	"write check",
	"get status",
	"seek",
	"read header",
	"write data",
	"read data",
	"read data w/o header check"
	};

static int rl02_trace_left = 0;

static constexpr uint32_t rl01_image_bytes = 5242880u;
static constexpr uint32_t rl02_image_bytes = 10485760u;
static constexpr int rl01_track_count = 256;
// Keep this short: long busy windows let RSX issue GETSTAT and rewrite DAR
// while a deferred READ is in flight (seen as disk_off=2816 + NOIRQ).
// vpdp1140 completes data in the CSR write and only delays the IRQ 2 ticks.
static constexpr int rl02_deferred_completion_instructions = 2;
static constexpr int rl02_register_count = 5;

void rl02_set_trace(const int count)
{
	rl02_trace_left = count < 0 ? 0 : count;
}

int rl02_trace_remaining()
{
	return rl02_trace_left;
}

static bool rl02_unit_is_rl02(const int unit)
{
#if defined(ESP32)
	return disk_size_is_rl02(disk_size_bytes(unit));
#else
	(void)unit;
	return true;
#endif
}

static bool rl02_unit_attached(const int unit)
{
#if defined(ESP32)
	return unit >= 0 && unit < 4 && disk_is_mounted(unit);
#else
	(void)unit;
	return true;
#endif
}

static int rl02_unit_track_count(const int unit)
{
#if defined(ESP32)
	const uint32_t bytes = disk_size_bytes(unit);
	if (disk_size_is_rl01(bytes))
		return rl01_track_count;
	if (disk_size_is_rl02(bytes))
		return rl02_track_count;
#else
	(void)unit;
#endif
	return rl02_track_count;
}

static void rl02_clamp_position_to_media(const int unit, int16_t& track, uint8_t& head, uint8_t& sector)
{
	const int tracks = rl02_unit_track_count(unit);
	if (track >= tracks) {
		track = tracks - 1;
		head = 1;
		sector = rl02_sectors_per_track - 1;
		return;
	}
	if (sector >= rl02_sectors_per_track) {
		sector = rl02_sectors_per_track - 1;
	}
}

static void rl02_trace(const char *fmt, ...)
{
#if defined(ESP32)
	if (rl02_trace_left <= 0)
		return;
	rl02_trace_left--;

	char buffer[192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof buffer, fmt, ap);
	va_end(ap);
	LOG("kek RL02 %s", buffer);
#else
	(void)fmt;
#endif
}

static uint32_t rl02_resolve_dma_pa(bus *const b, const uint32_t bus_address);

static uint32_t rl02_unibus_map_entry(const uint32_t bus_address)
{
	return (bus_address & 0777777) / UNIBUS_MAP_PAGE_SIZE;
}

static uint32_t rl02_unibus_map_base(bus *const b, const uint32_t bus_address)
{
	const uint32_t entry = rl02_unibus_map_entry(bus_address);
	return b ? b->get_unibus_map_entry((int)entry) : 0;
}

// DMA destination PA for RL. Prefer Unibus map. Addresses in 0760000-0777777
// are treated as 22-bit physical RAM: with BAE they are legitimate memory
// targets, but the Unibus map's last page is the I/O hole (n=0 DMA).
static uint32_t rl02_resolve_dma_pa(bus *const b, const uint32_t bus_address)
{
	const uint32_t addr18 = bus_address & 0777777;
	if (addr18 >= 0760000)
		return addr18;

	const uint32_t ub_pa = b->translate_unibus_for_monitor(bus_address);
	mmu *const mu = b->getMMU();
	if (!mu || !mu->is_enabled())
		return ub_pa;
	if ((bus_address & ~0177777u) != 0)
		return ub_pa;
	const uint32_t ub18 = bus_address & 0777777;
	if (ub_pa != ub18)
		return ub_pa;  // non-identity Unibus map — trust it

	const uint16_t va = uint16_t(ub18);
	const uint8_t apf = uint8_t(va >> 13);
	const int d_idx = mu->calc_par_pdr_index(0, d_space, apf);
	const uint16_t pard = mu->getPAR(d_idx);
	const uint16_t identity_par = uint16_t(apf) << 6;
	if (pard == 0 || pard == identity_par)
		return ub_pa;

	const uint32_t mask = (mu->getMMR3() & 16) ? 017777777u : 0x3ffffu;
	return (mu->get_physical_memory_offset(d_idx) + (va & 8191)) & mask;
}

static uint32_t rl02_dma_write(bus *const b, const uint32_t bus_address,
			       const uint8_t *source, const uint32_t n)
{
	const uint32_t addr18 = bus_address & 0777777;
	if (addr18 >= 0760000)
		return b->write_physical_block(addr18, source, n);

	const uint32_t ub_pa = b->translate_unibus_for_monitor(bus_address);
	const uint32_t pa = rl02_resolve_dma_pa(b, bus_address);
	if (pa == ub_pa)
		return b->write_unibus_block(bus_address, source, n);
	return b->write_physical_block(pa, source, n);
}

static uint32_t rl02_dma_read(bus *const b, const uint32_t bus_address,
			      uint8_t *target, const uint32_t n)
{
	const uint32_t addr18 = bus_address & 0777777;
	if (addr18 >= 0760000)
		return b->read_physical_block(addr18, target, n);

	const uint32_t ub_pa = b->translate_unibus_for_monitor(bus_address);
	const uint32_t pa = rl02_resolve_dma_pa(b, bus_address);
	if (pa == ub_pa)
		return b->read_unibus_block(bus_address, target, n);
	return b->read_physical_block(pa, target, n);
}

static uint32_t rl02_transfer_byte_count(const uint16_t mpr_value)
{
	const uint16_t words = uint16_t(0 - mpr_value);
	return uint32_t(words) * 2u;
}

rl02::rl02(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity) :
	b(b),
	disk_read_activity (disk_read_activity ),
	disk_write_activity(disk_write_activity)
{
}

rl02::~rl02()
{
	for(auto fh : fhs)
		delete fh;
}

void rl02::begin()
{
	reset(true);
}

void rl02::reset(const bool hard)
{
	if (hard) {
		memset(registers,   0x00, sizeof registers  );
		memset(xfer_buffer, 0x00, sizeof xfer_buffer);
		memset(mpr,         0x00, sizeof mpr        );
		bae_active = false;
	}

	deferred_data_active = false;
	deferred_execute = false;
	deferred_csr = 0;
	deferred_bar = 0;
	deferred_dar = 0;
	deferred_mpr = 0;
	deferred_bae = 0;
	deferred_bae_active = false;
	deferred_command = 0;
	deferred_device = 0;
	deferred_poll_count = 0;
	deferred_service_delay = -1;

	track  = 0;
	head   = 0;
	sector = 0;
}

FLASHMEM void rl02::show_state(console *const cnsl) const
{
	cnsl->put_string_lf(format("CSR: %06o", registers[0]));
	cnsl->put_string_lf(format("BAR: %06o", registers[1]));
	cnsl->put_string_lf(format("DAR: %06o", registers[2]));
	cnsl->put_string_lf(format("BAE: %06o", registers[4]));
	cnsl->put_string_lf(format("MPR: %06o / %06o / %06o", mpr[0], mpr[1], mpr[2]));

	cnsl->put_string_lf(format("track : %d", track ));
	cnsl->put_string_lf(format("head  : %d", head  ));
	cnsl->put_string_lf(format("sector: %d", sector));
	show_disk_backends(cnsl);
}

#if IS_POSIX
JsonDocument rl02::serialize() const
{
	JsonDocument j;

	JsonDocument j_backends;
	JsonArray    j_backends_work = j_backends.to<JsonArray>();
	for(auto & dbe: fhs)
		j_backends_work.add(dbe->serialize());
	j["backends"] = j_backends;

	for(int regnr=0; regnr<rl02_register_count; regnr++)
		j[format("register-%d", regnr)] = registers[regnr];

	for(int mprnr=0; mprnr<3; mprnr++)
		j[format("mpr-%d", mprnr)] = mpr[mprnr];

	j["bae-active"] = bae_active;
	j["track"]  = track;
	j["head"]   = head;
	j["sector"] = sector;

	return j;
}

rl02 *rl02::deserialize(const JsonVariantConst j, bus *const b)
{
	rl02 *r = new rl02(b, nullptr, nullptr);
	r->begin();

	JsonArrayConst j_backends = j["backends"];
	for(auto v: j_backends)
		r->access_disk_backends()->push_back(disk_backend::deserialize(v));

	for(int regnr=0; regnr<rl02_register_count; regnr++)
		r->registers[regnr] = j[format("register-%d", regnr)];

	for(int mprnr=0; mprnr<3; mprnr++)
		r->mpr[mprnr] = j[format("mpr-%d", mprnr)];

	r->bae_active = j["bae-active"] | false;
	r->track  = j["track"];
	r->head   = j["head"];
	r->sector = j["sector"];

	return r;
}
#endif

uint8_t rl02::read_byte(const uint16_t addr)
{
	uint16_t v = read_word(addr & ~1);

	if (addr & 1)
		return v >> 8;

	return v;
}

uint16_t rl02::read_word(const uint16_t addr)
{
	const int reg = (addr - RL02_BASE) / 2;

	if (addr == RL02_CSR) {  // control status
		const int device = (registers[reg] >> 8) & 3;
		if (deferred_data_active) {
			deferred_poll_count++;
			if (deferred_service_delay < 0)
				deferred_service_delay = rl02_deferred_completion_instructions;

			setBit(registers[reg], 0, rl02_unit_attached(device));  // drive ready (DRDY)
			setBit(registers[reg], 7, false);  // controller busy (CRDY clear)

			const uint16_t value = registers[reg];
			DOLOG(log_ss::LS_DISK, "RL02: deferred data command busy on CSR read: %06o", value);
			if (deferred_poll_count == 1 || deferred_service_delay <= 0 || (deferred_poll_count % 64) == 0) {
				rl02_trace("DEFER-BUSY unit=%d cmd=%u(%s) poll=%d delay=%d CSR=%06o BAR=%06o DAR=%06o MPR=%06o",
						deferred_device, deferred_command, commands[deferred_command],
						deferred_poll_count, deferred_service_delay, value,
						deferred_bar, deferred_dar, deferred_mpr);
			}

			return value;
		}

		setBit(registers[reg], 0, rl02_unit_attached(device));  // drive ready (DRDY)
		setBit(registers[reg], 7, true);                       // controller ready (CRDY)
	}

	uint16_t value = 0;

	if (addr == RL02_MPR) {  // multi purpose register
		value = mpr[0];
		mpr[0] = mpr[1];
		mpr[1] = mpr[2];
		mpr[2] = 0;
	}
	else {
		value = registers[reg];
	}

	DOLOG(log_ss::LS_DISK, "RL02: read \"%s\"/%o: %06o", regnames[reg], addr, value);
	rl02_trace("READ %-12s @ %06o -> %06o CSR=%06o BAR=%06o DAR=%06o MPR0=%06o trk=%d head=%u sec=%u",
			regnames[reg], addr, value,
			registers[0], registers[1], registers[2], mpr[0],
			track, head, sector);

	return value;
}

void rl02::write_byte(const uint16_t addr, const uint8_t v)
{
	uint16_t vtemp = registers[(addr - RL02_BASE) / 2];

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

uint32_t rl02::get_bus_address() const
{
	// Unibus RL11 (vpdp1140 / SIMH Unibus): 18-bit address from BAR + CSR
	// A16/A17 only. BAE is RLV12/Q22; RSX leaves a stale BAE after the
	// system load, which was folding HB DMA into 0760000-0777777 (I/O page).
	const uint32_t bar = registers[(RL02_BAR - RL02_BASE) / 2];
	return (bar | (uint32_t((registers[(RL02_CSR - RL02_BASE) / 2] >> 4) & 3) << 16)) & ~1u;
}

void rl02::update_bus_address(const uint32_t a)
{
	registers[(RL02_BAR - RL02_BASE) / 2] = a;
	registers[(RL02_BAE - RL02_BASE) / 2] = (a >> 16) & 077;
	bae_active = true;

	registers[(RL02_CSR - RL02_BASE) / 2] &= ~(3 << 4);
	registers[(RL02_CSR - RL02_BASE) / 2] |= ((a >> 16) & 3) << 4;
}

uint32_t rl02::calc_offset() const
{
	return (rl02_sectors_per_track * track * 2 + head * rl02_sectors_per_track + sector) * rl02_bytes_per_sector;
}

void rl02::update_dar()
{
	registers[(RL02_DAR - RL02_BASE) / 2] = (sector & 63) | (head << 6) | (track << 7);
}

void rl02::advance_disk_position(uint32_t bytes)
{
	uint32_t sectors = (bytes + rl02_bytes_per_sector - 1) / rl02_bytes_per_sector;

	while(sectors-- > 0) {
		sector++;
		if (sector >= rl02_sectors_per_track) {
			sector = 0;

			head++;
			if (head >= 2) {
				head = 0;
				track++;
			}
		}
	}
}

bool rl02::data_command_pending(const uint16_t csr) const
{
	const uint8_t command = (csr >> 1) & 7;
	return command == 5 || command == 6 || command == 7;
}

void rl02::defer_data_command(const uint16_t csr, const uint8_t command, const int device)
{
	deferred_data_active = true;
	deferred_csr = csr;
	deferred_bar = registers[(RL02_BAR - RL02_BASE) / 2];
	deferred_dar = registers[(RL02_DAR - RL02_BASE) / 2];
	deferred_mpr = registers[(RL02_MPR - RL02_BASE) / 2];
	deferred_bae = registers[(RL02_BAE - RL02_BASE) / 2];
	deferred_bae_active = bae_active;
	deferred_command = command;
	deferred_device = device;
	deferred_poll_count = 0;
	// Start the busy timer immediately so IE+WAIT (no CSR poll) still completes.
	deferred_service_delay = rl02_deferred_completion_instructions;

	setBit(registers[(RL02_CSR - RL02_BASE) / 2], 0, true);   // drive ready
	setBit(registers[(RL02_CSR - RL02_BASE) / 2], 7, false);  // controller busy

	rl02_trace("DEFER-DATA unit=%d cmd=%u(%s) CSR=%06o BAR=%06o DAR=%06o MPR=%06o",
			device, command, commands[command], registers[0], deferred_bar,
			deferred_dar, deferred_mpr);
}

void rl02::complete_deferred_data_command()
{
	if (!deferred_data_active)
		return;

	const uint16_t csr = deferred_csr;
	const uint8_t command = deferred_command;
	const int device = deferred_device;

	// Restore transfer parameters in case software poked DAR/BAR during busy
	// (RSX GETSTAT uses DAR=13 and was observed corrupting in-flight READs).
	registers[(RL02_BAR - RL02_BASE) / 2] = deferred_bar;
	registers[(RL02_DAR - RL02_BASE) / 2] = deferred_dar;
	registers[(RL02_MPR - RL02_BASE) / 2] = deferred_mpr;
	registers[(RL02_BAE - RL02_BASE) / 2] = deferred_bae;
	bae_active = deferred_bae_active;
	mpr[0] = deferred_mpr;

	deferred_data_active = false;
	deferred_execute = true;
	rl02_trace("DEFER-COMPLETE unit=%d cmd=%u(%s) CSR=%06o BAR=%06o DAR=%06o MPR=%06o",
			device, command, commands[command], csr, deferred_bar,
			deferred_dar, deferred_mpr);
	write_word(RL02_CSR, csr);
	deferred_execute = false;
}

void rl02::service_deferred()
{
	if (deferred_data_active) {
		if (deferred_service_delay < 0)
			deferred_service_delay = rl02_deferred_completion_instructions;
		if (deferred_service_delay > 0)
			deferred_service_delay--;
		else
			complete_deferred_data_command();
	}

#if defined(ESP32)
	if (irq_pending_ticks > 0 && --irq_pending_ticks == 0) {
		if (registers[(RL02_CSR - RL02_BASE) / 2] & 64) {
			rl02_trace("IRQ-DELIVER vec=160 BR5 CSR=%06o",
					registers[(RL02_CSR - RL02_BASE) / 2]);
			b->getCpu()->queue_interrupt(5, 0160);
		}
		else {
			rl02_trace("IRQ-CANCEL IE cleared CSR=%06o",
					registers[(RL02_CSR - RL02_BASE) / 2]);
		}
	}
#endif
}

void rl02::write_word(const uint16_t addr, uint16_t v)
{
	const int reg = (addr - RL02_BASE) / 2;

	DOLOG(log_ss::LS_DISK, "RL02: write \"%s\"/%06o: %06o", regnames[reg], addr, v);

	// While a data xfer is in progress, keep BAR/DAR/MPR/CSR stable. RSX was
	// observed issuing GETSTAT (DAR=13) during the deferred window, which
	// made the eventual READ land at disk_off=2816 and drop IE/IRQ.
	if (deferred_data_active && !deferred_execute) {
		rl02_trace("WRITE-BUSY-IGNORE @ %06o val=%06o pending CSR=%06o BAR=%06o DAR=%06o MPR=%06o",
				addr, v, deferred_csr, deferred_bar, deferred_dar, deferred_mpr);
		setBit(registers[(RL02_CSR - RL02_BASE) / 2], 0, true);
		setBit(registers[(RL02_CSR - RL02_BASE) / 2], 7, false);
		return;
	}

	if (addr == RL02_BAE) {
		registers[reg] = v & 077;
		bae_active = true;
		rl02_trace("WRITE BAE @ %06o val=%06o -> %06o CSR=%06o BAR=%06o DAR=%06o MPR=%06o",
				addr, v, registers[reg],
				registers[0], registers[1], registers[2],
				registers[(RL02_MPR - RL02_BASE) / 2]);
		return;
	}

	registers[reg] = v;

	if (addr == RL02_CSR) {  // control status
#if defined(ESP32)
		irq_pending_ticks = 0;
		if (b && b->getCpu())
			b->getCpu()->unqueue_interrupt(5, 0160);
#endif
		const uint8_t command = (v >> 1) & 7;

		const bool    do_exec = !(v & 128);

		int           device  = (v >> 8) & 3;

		DOLOG(log_ss::LS_DISK, "RL02: device %d, set command %d, exec: %d (%s)", device, command, do_exec, commands[command]);
		rl02_trace("WRITE CSR @ %06o val=%06o unit=%d cmd=%u(%s) exec=%d IE=%d CSR=%06o BAR=%06o DAR=%06o MPR=%06o trk=%d head=%u sec=%u slots=%u",
				addr, v, device, command, commands[command], do_exec ? 1 : 0,
				(v & 64) ? 1 : 0,
				registers[0], registers[1], registers[2],
				registers[(RL02_MPR - RL02_BASE) / 2],
				track, head, sector, (unsigned)fhs.size());

		bool          do_int  = false;

		if (size_t(device) >= fhs.size() || !rl02_unit_attached(device)) {
			DOLOG(log_ss::LS_DISK, "RL02: PDP11/70 is accessing virtual disk %d which is not attached", device);

			registers[(RL02_CSR - RL02_BASE) / 2] |= (1 << 10) | (1 << 15);
			rl02_trace("NOT-ATTACHED unit=%d CSR=%06o", device, registers[(RL02_CSR - RL02_BASE) / 2]);

			do_int = true;
		}
		else if (data_command_pending(v) && !deferred_execute) {
			defer_data_command(v, command, device);
			return;
		}
		else if (command == 2) {  // get status
			mpr[0] = 5 /* lock on */ | (1 << 3) /* brush home */ | (1 << 4) /* heads over disk */ | (head << 6);
			if (rl02_unit_is_rl02(device))
				mpr[0] |= (1 << 7) /* RL02 */;
			mpr[1] = mpr[0];
			rl02_trace("GETSTAT unit=%d media=%s -> MPR=%06o", device, rl02_unit_is_rl02(device) ? "RL02" : "RL01", mpr[0]);
		}
		else if (command == 3) {  // seek
			uint16_t temp = registers[(RL02_DAR - RL02_BASE) / 2];
			const int media_tracks = rl02_unit_track_count(device);

			int cylinder_count = (temp >> 7) * (temp & 4 ? 1 : -1);

			int16_t new_track = track + cylinder_count;

			if (new_track < 0)
				new_track = 0;
			else if (new_track >= media_tracks)
				new_track = media_tracks - 1;

			DOLOG(log_ss::LS_DISK, "RL02: device %d, seek from cylinder %d to %d (distance: %d, DAR: %06o)", device, track, new_track, cylinder_count, temp);
			rl02_trace("SEEK unit=%d media=%s DAR=%06o old_track=%d new_track=%d distance=%d",
					device, rl02_unit_is_rl02(device) ? "RL02" : "RL01", temp, track, new_track, cylinder_count);
			track  = new_track;

//			update_dar();

			do_int = true;
		}
		else if (command == 4) {  // read header
			mpr[0] = (sector & 63) | (head << 6) | (track << 7);
			mpr[1] = 0;  // zero
			mpr[2] = 0;  // TODO: CRC

			DOLOG(log_ss::LS_DISK, "RL02: device %d, read header [cylinder: %d, head: %d, sector: %d] %06o", device, track, head, sector, mpr[0]);
			rl02_trace("RDHDR unit=%d -> hdr=%06o trk=%d head=%u sec=%u",
					device, mpr[0], track, head, sector);

			do_int = true;
		}
		else if (command == 5) {  // write data
			if (disk_write_activity)
				*disk_write_activity = true;

			uint32_t memory_address   = get_bus_address();

			uint32_t count            = rl02_transfer_byte_count(registers[(RL02_MPR - RL02_BASE) / 2]);

			uint16_t temp             = registers[(RL02_DAR - RL02_BASE) / 2];

			sector = temp & 63;
			head   = (temp >> 6) & 1;
			track  = temp >> 7;
			const int media_tracks = rl02_unit_track_count(device);
			if (track >= media_tracks) {
				registers[(RL02_CSR - RL02_BASE) / 2] |= (1 << 10) | (1 << 15);
				rl02_trace("WRITE-ERR unit=%d media=%s track=%d beyond max=%d",
						device, rl02_unit_is_rl02(device) ? "RL02" : "RL01",
						track, media_tracks - 1);
				do_int = true;
				goto command_done;
			}

			uint32_t temp_disk_offset = calc_offset();
			uint32_t words_done       = 0;

			DOLOG(log_ss::LS_DISK, "RL02: device %d, write %d bytes (dec) to %d (dec) from %06o (oct) [cylinder: %d, head: %d, sector: %d]", device, count, temp_disk_offset, memory_address, track, head, sector);
			rl02_trace("WRITE-DATA unit=%d bytes=%u disk_off=%u bus=%06o map[%02u]=%08o phys=%08o trk=%d head=%u sec=%u MPR=%06o",
					device, (unsigned)count, (unsigned)temp_disk_offset,
					(unsigned)memory_address,
					(unsigned)rl02_unibus_map_entry(memory_address),
					(unsigned)rl02_unibus_map_base(b, memory_address),
					(unsigned)b->translate_unibus_for_monitor(memory_address),
					track, head, sector,
					registers[(RL02_MPR - RL02_BASE) / 2]);

			while(count > 0) {
				uint32_t cur = std::min(uint32_t(sizeof xfer_buffer), count);

				uint32_t copied = rl02_dma_read(b, memory_address, xfer_buffer, cur);
				if (copied != cur) {
					DOLOG(log_ss::LS_DISK, "RL02: DMA read from PDP memory short transfer, requested %u got %u at %06o", cur, copied, memory_address);
					rl02_trace("WRITE-DMA-ERR unit=%d bus=%06o requested=%u copied=%u",
							device, (unsigned)memory_address, (unsigned)cur, (unsigned)copied);
					cur = copied & ~1u;
					if (cur == 0)
						break;
				}
				memory_address += cur;
				words_done += cur / 2;

				if (fhs.at(device) == nullptr || fhs.at(device)->write(temp_disk_offset, cur, xfer_buffer, 256) == false) {
					DOLOG(log_ss::LS_DISK, "RL02: write error, device %d, disk offset %u, read size %u, cylinder %d, head %d, sector %d", device, temp_disk_offset, cur, track, head, sector);
					rl02_trace("WRITE-ERR unit=%d disk_off=%u len=%u trk=%d head=%u sec=%u",
							device, (unsigned)temp_disk_offset, (unsigned)cur,
							track, head, sector);
					break;
				}

				temp_disk_offset += cur;

				count -= cur;

				advance_disk_position(cur);
			}
			update_bus_address(memory_address);
			registers[(RL02_MPR - RL02_BASE) / 2] += words_done;
			mpr[0] = registers[(RL02_MPR - RL02_BASE) / 2];
			rl02_clamp_position_to_media(device, track, head, sector);
			update_dar();
			rl02_trace("WRITE-DONE unit=%d BAR=%06o map[%02u]=%08o phys=%08o DAR=%06o MPR=%06o trk=%d head=%u sec=%u words=%u",
					device, registers[(RL02_BAR - RL02_BASE) / 2],
					(unsigned)rl02_unibus_map_entry(get_bus_address()),
					(unsigned)rl02_unibus_map_base(b, get_bus_address()),
					(unsigned)b->translate_unibus_for_monitor(get_bus_address()),
					registers[(RL02_DAR - RL02_BASE) / 2],
					registers[(RL02_MPR - RL02_BASE) / 2],
					track, head, sector, (unsigned)words_done);

			do_int = true;
		}
		else if (command == 6 || command == 7) {  // read data / read data without header check
			if (disk_read_activity)
				*disk_read_activity = true;

			uint32_t memory_address   = get_bus_address();

			uint32_t count            = rl02_transfer_byte_count(registers[(RL02_MPR - RL02_BASE) / 2]);

			uint16_t temp             = registers[(RL02_DAR - RL02_BASE) / 2];

			sector = temp & 63;
			head   = (temp >> 6) & 1;
			track  = temp >> 7;
			const int media_tracks = rl02_unit_track_count(device);
			bool zero_read = false;
			if (track >= media_tracks) {
				rl02_trace("READ-ERR unit=%d media=%s track=%d beyond max=%d",
						device, rl02_unit_is_rl02(device) ? "RL02" : "RL01",
						track, media_tracks - 1);
				zero_read = true;
			}

			uint32_t temp_disk_offset = calc_offset();
			uint32_t words_done       = 0;

			DOLOG(log_ss::LS_DISK, "RL02: device %d, read %d bytes (dec) from %d (dec) to %06o (oct) [cylinder: %d, head: %d, sector: %d]", device, count, temp_disk_offset, memory_address, track, head, sector);
			rl02_trace("READ-DATA unit=%d bytes=%u disk_off=%u bus=%06o map[%02u]=%08o phys=%08o trk=%d head=%u sec=%u MPR=%06o",
					device, (unsigned)count, (unsigned)temp_disk_offset,
					(unsigned)memory_address,
					(unsigned)rl02_unibus_map_entry(memory_address),
					(unsigned)rl02_unibus_map_base(b, memory_address),
					(unsigned)b->translate_unibus_for_monitor(memory_address),
					track, head, sector,
					registers[(RL02_MPR - RL02_BASE) / 2]);

			while(count > 0) {
				uint32_t cur = std::min(uint32_t(sizeof xfer_buffer), count);

				if (zero_read || track >= media_tracks) {
					memset(xfer_buffer, 0, cur);
					zero_read = true;
				}
				else if (fhs.at(device) == nullptr || fhs.at(device)->read(temp_disk_offset, cur, xfer_buffer, 256) == false) {
					DOLOG(log_ss::LS_DISK, "RL02: read error, device %d, disk offset %u, read size %u, cylinder %d, head %d, sector %d", device, temp_disk_offset, cur, track, head, sector);
					rl02_trace("READ-ERR unit=%d disk_off=%u len=%u trk=%d head=%u sec=%u",
							device, (unsigned)temp_disk_offset, (unsigned)cur,
							track, head, sector);
					break;
				}

				uint32_t copied = rl02_dma_write(b, memory_address, xfer_buffer, cur);
				if (copied != cur) {
					DOLOG(log_ss::LS_DISK, "RL02: DMA write to PDP memory short transfer, requested %u got %u at %06o", cur, copied, memory_address);
					rl02_trace("READ-DMA-ERR unit=%d bus=%06o requested=%u copied=%u",
							device, (unsigned)memory_address, (unsigned)cur, (unsigned)copied);
					cur = copied & ~1u;
					if (cur == 0)
						break;
				}
				memory_address += cur;
				words_done += cur / 2;

				temp_disk_offset += cur;

				count -= cur;

				advance_disk_position(cur);
				if (track >= media_tracks)
					zero_read = true;
			}
			update_bus_address(memory_address);
			registers[(RL02_MPR - RL02_BASE) / 2] += words_done;
			mpr[0] = registers[(RL02_MPR - RL02_BASE) / 2];
			rl02_clamp_position_to_media(device, track, head, sector);
			update_dar();
			rl02_trace("READ-DONE unit=%d BAR=%06o map[%02u]=%08o phys=%08o DAR=%06o MPR=%06o trk=%d head=%u sec=%u words=%u",
					device, registers[(RL02_BAR - RL02_BASE) / 2],
					(unsigned)rl02_unibus_map_entry(get_bus_address()),
					(unsigned)rl02_unibus_map_base(b, get_bus_address()),
					(unsigned)b->translate_unibus_for_monitor(get_bus_address()),
					registers[(RL02_DAR - RL02_BASE) / 2],
					registers[(RL02_MPR - RL02_BASE) / 2],
					track, head, sector, (unsigned)words_done);
			do_int = true;
		}
		else {
			DOLOG(log_ss::LS_DISK, "RL02: command %d not implemented", command);
		}

command_done:
		if (do_int) {
			if (registers[(RL02_CSR - RL02_BASE) / 2] & 64) {  // interrupt enable?
				DOLOG(log_ss::LS_DISK, "RL02: triggering interrupt");
#if defined(ESP32)
				irq_pending_ticks = IRQ_DELAY_TICKS;
				rl02_trace("IRQ-SCHEDULE unit=%d vec=160 BR5 ticks=%d CSR=%06o",
						device, IRQ_DELAY_TICKS,
						registers[(RL02_CSR - RL02_BASE) / 2]);
#else
				rl02_trace("IRQ unit=%d vec=160 BR5 CSR=%06o",
						device, registers[(RL02_CSR - RL02_BASE) / 2]);
				b->getCpu()->queue_interrupt(5, 0160);
#endif
			}
			else {
				rl02_trace("NOIRQ unit=%d IE=0 CSR=%06o",
						device, registers[(RL02_CSR - RL02_BASE) / 2]);
			}
		}
	}
}
