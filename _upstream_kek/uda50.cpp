// UDA50 Unibus port and UQSSP ring transport.
// MSCP command processing is intentionally deferred.

#include "gen.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "bus.h"
#include "cpu.h"
#include "uda50.h"
#include "utils.h"

uda50::uda50(bus *const bus_) : b(bus_)
{
	reset(true);
}

void uda50::attach_media(void *const context, media_read_fn const read_fn,
		media_write_fn const write_fn, media_size_fn const size_fn)
{
	media_context = context;
	media_read = read_fn;
	media_write = write_fn;
	media_size = size_fn;
	unit_online = false;
}

void uda50::set_trace(const uint32_t count, void *const context, const trace_fn fn)
{
	trace_left = count;
	trace_context = context;
	trace_callback = fn;
	tracef("UDA trace armed count=%lu", (unsigned long)count);
}

void uda50::tracef(const char *const format, ...)
{
	if (!trace_left || !trace_callback) return;
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof line, format, args);
	va_end(args);
	trace_left--;
	trace_callback(trace_context, line);
}

void uda50::reset(const bool)
{
	if (interrupt_vector && b && b->getCpu())
		b->getCpu()->unqueue_interrupt(5, interrupt_vector);

	state = state_t::step1;
	sa = SA_STEP1 | S1_CONTROLLER_DIAGNOSTICS | S1_CONTROLLER_MAPPING;
	tracef("RESET state=step1 SA=%06o", sa);
	pending_sa_write = 0;
	step1_data = 0;
	interrupt_vector = 0154;  // Standard UDA50 default; zero STEP1 vector preserves it.
	command_ring_length = 0;
	response_ring_length = 0;
	communication_address = 0;
	interrupt_enabled = false;
	purge_interrupt = false;
	service_pending = false;
	poll_pending = false;
	command_valid = false;
	response_valid = false;
	response_delay_ticks = 0;
	first_mscp_response = true;
	controller_flags = 0;
	host_timeout = 0;
	unit_online = false;
	command_packet_length = 0;
	response_packet_length = 0;
	memset(command_packet, 0, sizeof command_packet);
	memset(response_packet, 0, sizeof response_packet);
	command_ring = ring_t();
	response_ring = ring_t();
}

void uda50::show_state(console *const cnsl) const
{
	if (!cnsl)
		return;

	cnsl->put_string_lf(format(
		"UDA50 state %u, SA %06o, vector %03o, comm %08o, cmd/rsp %u/%u, pending %u/%u",
		static_cast<unsigned>(state), sa, interrupt_vector,
		static_cast<unsigned>(communication_address),
		command_ring_length, response_ring_length,
		command_valid ? 1u : 0u, response_valid ? 1u : 0u));
}

uint8_t uda50::read_byte(const uint16_t addr)
{
	const uint16_t value = read_word(addr & ~1);
	return addr & 1 ? value >> 8 : value & 0xff;
}

uint16_t uda50::read_word(const uint16_t addr)
{
	switch (addr) {
		case UDA50_IP:
			// The IP always reads as zero.  During the optional purge/poll
			// test, reading it is the final host acknowledgement.
			if (state == state_t::step3_purge_ip)
				begin_step4();
			else if (state == state_t::run) {
				poll_pending = true;
				service_pending = true;
			}
			return 0;

		case UDA50_SA:
			return sa;

		default:
			return 0;
	}
}

void uda50::update_byte(uint16_t *const word, const uint16_t addr,
		const uint8_t value)
{
	if (addr & 1)
		*word = (*word & 0x00ff) | (uint16_t(value) << 8);
	else
		*word = (*word & 0xff00) | value;
}

void uda50::write_byte(const uint16_t addr, const uint8_t value)
{
	if ((addr & ~1) == UDA50_IP) {
		reset(true);
		return;
	}

	if ((addr & ~1) == UDA50_SA) {
		update_byte(&pending_sa_write, addr, value);
		service_pending = true;
	}
}

void uda50::write_word(const uint16_t addr, const uint16_t value)
{
	switch (addr) {
		case UDA50_IP:
			// Any IP write starts controller initialization.
			reset(true);
			break;

		case UDA50_SA:
			pending_sa_write = value;
			service_pending = true;
			break;
	}
}

void uda50::post_initialization_interrupt()
{
	tracef("INIT IRQ enabled=%u vector=%03o state=%u SA=%06o", interrupt_enabled ? 1u : 0u, interrupt_vector, (unsigned)state, sa);
	if (interrupt_enabled && interrupt_vector && b && b->getCpu())
		b->getCpu()->queue_interrupt(5, interrupt_vector);
}

void uda50::post_ring_interrupt(const ring_t &ring)
{
	// Always raise the soft ring-transition flag in the communications area
	// so polling drivers (rauboot, standalone MSCP) can see completion.
	// Only post a Unibus interrupt when the host enabled IE during STEP1 —
	// otherwise vector 154 is often still boot-block garbage and we jump
	// into random code (PiDP 2.11BSD odd-PC at 010067).
	tracef("RING IRQ enabled=%u vector=%03o base=%08o index=%u flag@%08o",
			interrupt_enabled ? 1u : 0u, interrupt_vector, (unsigned)ring.base,
			ring.index, (unsigned)(communication_address + ring.interrupt_offset));
	const uint16_t one = 1;
	b->write_unibus_word(communication_address + ring.interrupt_offset, one);
	if (interrupt_enabled && interrupt_vector && b->getCpu())
		b->getCpu()->queue_interrupt(5, interrupt_vector);
}

void uda50::fatal(const uint16_t error)
{
	sa = SA_ERROR | error;
	state = state_t::dead;
	poll_pending = false;
}

void uda50::begin_step4()
{
	if (!initialize_communication_area()) {
		fatal(ERROR_QUEUE_WRITE);
		return;
	}

	// UDA50A model 6, controller software version 3.
	sa = SA_STEP4 | (UDA50_PORT_MODEL << 4) | UDA50_SOFTWARE_VERSION;
	state = state_t::step4;
	post_initialization_interrupt();
}

void uda50::service_initialization()
{
	const uint16_t value = pending_sa_write;
	tracef("INIT SA-WR state=%u value=%06o SA=%06o", (unsigned)state, value, sa);
	service_pending = false;

	switch (state) {
		case state_t::step1:
			if (!(value & S1_HOST_VALID))
				return;

			if (value & S1_HOST_WRAP) {
				sa = value;
				state = state_t::step1_wrap;
				return;
			}

			step1_data = value;
			command_ring_length = uint16_t(1u << ((value >> 11) & 7));
			response_ring_length = uint16_t(1u << ((value >> 8) & 7));
			interrupt_enabled = (value & S1_HOST_INTERRUPT_ENABLE) != 0;
			if (value & S1_HOST_VECTOR)
				interrupt_vector = uint16_t((value & S1_HOST_VECTOR) << 2);
			sa = SA_STEP2 | ((value >> 8) & 0xff);
			state = state_t::step2;
			post_initialization_interrupt();
			break;

		case state_t::step1_wrap:
			sa = value;
			break;

		case state_t::step2:
			communication_address = value & S2_HOST_COMM_LOW;
			purge_interrupt = (value & 1) != 0;
			sa = SA_STEP3 | ((step1_data >> 8) & 0xff);
			state = state_t::step3;
			post_initialization_interrupt();
			break;

		case state_t::step3:
			communication_address |=
				uint32_t(value & S3_HOST_COMM_HIGH) << 16;
			if (value & S3_HOST_PURGE_POLL) {
				sa = 0;
				state = state_t::step3_purge_sa;
			}
			else {
				begin_step4();
			}
			break;

		case state_t::step3_purge_sa:
			if (value != 0) {
				sa = SA_ERROR | 1;
				state = state_t::dead;
			}
			else {
				state = state_t::step3_purge_ip;
			}
			break;

		case state_t::step4:
			if (value & S4_HOST_GO) {
				sa = 0;
				state = state_t::run;
				poll_pending = true;
			}
			break;

		case state_t::step3_purge_ip:
		case state_t::run:
		case state_t::dead:
			break;
	}
}

bool uda50::initialize_communication_area()
{
	response_ring.interrupt_offset = -2;
	response_ring.base = communication_address;
	response_ring.byte_length = uint16_t(response_ring_length * 4u);
	response_ring.index = 0;
	command_ring.interrupt_offset = -4;
	command_ring.base = communication_address + response_ring.byte_length;
	command_ring.byte_length = uint16_t(command_ring_length * 4u);
	command_ring.index = 0;

	const uint32_t clear_base = communication_address + (purge_interrupt ? uint32_t(-8) : uint32_t(-4));
	const uint16_t clear_length = uint16_t(
		command_ring.base + command_ring.byte_length - clear_base);
	uint8_t zeroes[8] { 0 };
	uint32_t address = clear_base;
	uint16_t remaining = clear_length;
	while (remaining) {
		const uint16_t chunk = remaining > sizeof zeroes ? sizeof zeroes : remaining;
		if (b->write_unibus_block(address, zeroes, chunk) != chunk)
			return false;
		address += chunk;
		remaining -= chunk;
	}
	return true;
}

bool uda50::read_descriptor(const ring_t &ring, uint32_t *const descriptor) const
{
	if (!descriptor || ring.byte_length < 4)
		return false;
	const uint32_t address = ring.base + ring.index;
	const uint16_t low = b->read_unibus_word(address);
	const uint16_t high = b->read_unibus_word(address + 2);
	*descriptor = uint32_t(low) | (uint32_t(high) << 16);
	return true;
}

bool uda50::release_descriptor(ring_t *const ring, const uint32_t descriptor)
{
	if (!ring || ring->byte_length < 4)
		return false;

	const uint32_t address = ring->base + ring->index;
	const uint32_t released = (descriptor & ~DESC_OWN) | DESC_FLAG;
	b->write_unibus_word(address, uint16_t(released));
	b->write_unibus_word(address + 2, uint16_t(released >> 16));

	if (descriptor & DESC_FLAG) {
		bool transition = ring->byte_length == 4;
		if (!transition) {
			const uint16_t previous_index =
				uint16_t((ring->index - 4) & (ring->byte_length - 1));
			const uint32_t previous_address = ring->base + previous_index;
			const uint32_t previous =
				uint32_t(b->read_unibus_word(previous_address)) |
				(uint32_t(b->read_unibus_word(previous_address + 2)) << 16);
			transition = (previous & DESC_OWN) != 0;
		}
		if (transition)
			post_ring_interrupt(*ring);
	}

	ring->index = uint16_t((ring->index + 4) & (ring->byte_length - 1));
	return true;
}

uint16_t uda50::packet_length(const uint8_t *const packet) const
{
	if (!packet)
		return 0;
	const uint16_t payload = uint16_t(packet[0]) | (uint16_t(packet[1]) << 8);
	const uint32_t total = uint32_t(payload) + 4u;
	return total > PACKET_BYTES ? PACKET_BYTES : uint16_t(total);
}

bool uda50::fetch_command()
{
	if (command_valid)
		return true;

	uint32_t descriptor = 0;
	if (!read_descriptor(command_ring, &descriptor)) {
		fatal(ERROR_QUEUE_READ);
		return false;
	}
	if (!(descriptor & DESC_OWN)) {
		poll_pending = false;
		return true;
	}

	const uint32_t packet_address = descriptor & DESC_ADDRESS;
	if (packet_address < 4 ||
			b->read_unibus_block(packet_address - 4, command_packet,
				PACKET_BYTES) != PACKET_BYTES) {
		fatal(ERROR_PACKET_READ);
		return false;
	}
	command_packet_length = packet_length(command_packet);
	if (command_packet_length < 4)
		command_packet_length = 4;
	command_valid = true;
	tracef("CMD fetch desc=%011lo pkt=%08lo len=%u op=%u unit=%u ref=%06o", (unsigned long)descriptor, (unsigned long)(packet_address - 4), command_packet_length, (unsigned)(packet_word(command_packet, 6) & 0377), packet_word(command_packet, 4), packet_word(command_packet, 2));
	if (!release_descriptor(&command_ring, descriptor)) {
		fatal(ERROR_QUEUE_WRITE);
		return false;
	}
	return true;
}

bool uda50::post_response()
{
	if (!response_valid)
		return true;

	// Do not let a command complete in the same guest instruction window in
	// which the host submitted it. RSX finishes linking/arming its I/O packet
	// after ringing the controller poll doorbell; an immediate response can
	// run the interrupt handler first, after which RSX clears the completion
	// flag and waits forever. Deferred device service runs every eight guest
	// instructions, so 16 ticks is about 128 instructions (sub-millisecond at
	// normal emulation speed) while still resembling asynchronous hardware.
	if (response_delay_ticks) {
		--response_delay_ticks;
		return true;
	}

	// The CPU queue represents pending vectors as a set. If two MSCP
	// responses are posted before the guest services the first UDA
	// interrupt, a second identical vector would otherwise be collapsed.
	// Also avoid completing a command synchronously inside the current
	// BR5 UDA handler. RSX submits ONLINE while processing the SCC response;
	// a real controller cannot return ONLINE until that handler has dropped
	// IPL below BR5.
	if (interrupt_enabled && interrupt_vector && b && b->getCpu() &&
			(b->getCpu()->has_queued_interrupt(5, interrupt_vector) ||
			 b->getCpu()->getPSW_spl() >= 5))
		return true;

	uint32_t descriptor = 0;
	if (!read_descriptor(response_ring, &descriptor)) {
		fatal(ERROR_QUEUE_READ);
		return false;
	}
	if (!(descriptor & DESC_OWN))
		return true;

	const uint32_t packet_address = descriptor & DESC_ADDRESS;
	if (packet_address < 4 ||
			b->write_unibus_block(packet_address - 4, response_packet,
				response_packet_length) != response_packet_length) {
		fatal(ERROR_PACKET_WRITE);
		return false;
	}
	tracef("RSP post desc=%011lo pkt=%08lo len=%u op=%u status=%06o credits=%u", (unsigned long)descriptor, (unsigned long)(packet_address - 4), response_packet_length, (unsigned)(packet_word(response_packet, 6) & 0377), packet_word(response_packet, 7), packet_word(response_packet, 1) & 017);
	if (!release_descriptor(&response_ring, descriptor)) {
		fatal(ERROR_QUEUE_WRITE);
		return false;
	}
	response_valid = false;
	response_delay_ticks = 0;
	response_packet_length = 0;
	return true;
}

uint16_t uda50::packet_word(const uint8_t *const packet, const uint16_t index) const
{
	const uint16_t offset = uint16_t(index * 2);
	if (!packet || offset + 1 >= PACKET_BYTES) return 0;
	return uint16_t(packet[offset]) | (uint16_t(packet[offset + 1]) << 8);
}

void uda50::set_packet_word(uint8_t *const packet, const uint16_t index, const uint16_t value)
{
	const uint16_t offset = uint16_t(index * 2);
	if (!packet || offset + 1 >= PACKET_BYTES) return;
	packet[offset] = uint8_t(value);
	packet[offset + 1] = uint8_t(value >> 8);
}

bool uda50::build_response(const uint8_t opcode, const uint16_t status, const uint16_t payload_length)
{
	if (payload_length + 4u > PACKET_BYTES) return false;
	uint8_t response[PACKET_BYTES] { 0 };
	set_packet_word(response, 0, payload_length);
	set_packet_word(response, 1, first_mscp_response ? 15 : 1);
	set_packet_word(response, 2, packet_word(command_packet, 2));
	set_packet_word(response, 3, packet_word(command_packet, 3));
	set_packet_word(response, 4, packet_word(command_packet, 4));
	set_packet_word(response, 6, uint16_t(opcode) | 0x0080);
	set_packet_word(response, 7, status);
	first_mscp_response = false;
	return submit_response(response, uint16_t(payload_length + 4u));
}

bool uda50::media_present() const
{
	return media_read && media_write && media_size && media_size(media_context) >= 512;
}

bool uda50::transfer(const bool write)
{
	static constexpr uint16_t ST_OFL_NV = 3 | (1 << 5);
	static constexpr uint16_t ST_AVL = 4;
	static constexpr uint16_t ST_HST_OA = 9 | (1 << 5);
	static constexpr uint16_t ST_HST_OC = 9 | (2 << 5);
	static constexpr uint16_t ST_HST_NXM = 9 | (3 << 5);
	static constexpr uint16_t ST_CMD_BCNT = 1 | (12 << 8);
	static constexpr uint16_t ST_CMD_LBN = 1 | (28 << 8);
	static constexpr uint16_t ST_DRV = 11;
	uint32_t count = uint32_t(packet_word(command_packet, 8)) |
		(uint32_t(packet_word(command_packet, 9)) << 16);
	const uint32_t requested_count = count;
	uint32_t address = (uint32_t(packet_word(command_packet, 10)) |
		(uint32_t(packet_word(command_packet, 11)) << 16)) & 0x003fffffu;
	const uint32_t lbn = uint32_t(packet_word(command_packet, 16)) |
		(uint32_t(packet_word(command_packet, 17)) << 16);
	uint16_t status = 0;
	tracef("%s start lbn=%lu bytes=%lu dma=%08lo online=%u media=%lu", write ? "WRITE" : "READ", (unsigned long)lbn, (unsigned long)count, (unsigned long)address, unit_online ? 1u : 0u, (unsigned long)(media_size ? media_size(media_context) : 0));
	if (!media_present()) status = ST_OFL_NV;
	else if (!unit_online) status = ST_AVL;
	else if (address & 1) status = ST_HST_OA;
	else if (count & 1) status = ST_HST_OC;
	else if (count > 0x0fffffffu) status = ST_CMD_BCNT;
	else if (uint64_t(lbn) * 512u + count > media_size(media_context)) status = ST_CMD_LBN;
	uint8_t buffer[512];
	uint32_t offset = lbn * 512u;
	while (!status && count) {
		const uint32_t chunk = count > sizeof buffer ? sizeof buffer : count;
		if (write) {
			if (b->read_unibus_block(address, buffer, chunk) != chunk)
				status = ST_HST_NXM;
			else if (!media_write(media_context, offset, buffer, chunk))
				status = ST_DRV;
		} else {
			if (!media_read(media_context, offset, buffer, chunk))
				status = ST_DRV;
			else if (b->write_unibus_block(address, buffer, chunk) != chunk)
				status = ST_HST_NXM;
		}
		if (status) break;
		address += chunk; offset += chunk; count -= chunk;
	}
	const bool accepted = build_response(write ? 34 : 33, status, 32);
	if (accepted) {
		const uint32_t processed = requested_count - count;
		set_packet_word(response_packet, 8, uint16_t(processed));
		set_packet_word(response_packet, 9, uint16_t(processed >> 16));
	}
	tracef("%s done status=%06o processed=%lu accepted=%u", write ? "WRITE" : "READ", status, (unsigned long)(requested_count - count), accepted ? 1u : 0u);
	return accepted;
}
bool uda50::process_command()
{
	static constexpr uint16_t ST_OFL_NV = 3 | (1 << 5);
	static constexpr uint16_t ST_BAD_OP = 1 | (8 << 8);
	static constexpr uint16_t ST_BAD_VERSION = 1 | (12 << 8);
	if (!command_valid || response_valid) return false;
	const uint8_t opcode = uint8_t(packet_word(command_packet, 6));
	tracef("MSCP op=%u unit=%u ref=%06o modifier=%06o flags=%06o", (unsigned)opcode, packet_word(command_packet, 4), packet_word(command_packet, 2), packet_word(command_packet, 7), packet_word(command_packet, 9));
	bool accepted = false;
	switch (opcode) {
		case 4: // Set Controller Characteristics
			if (packet_word(command_packet, 8) != 0) {
				accepted = build_response(0, ST_BAD_VERSION, 12);
				break;
			}
			controller_flags = packet_word(command_packet, 9) & 0x80f0;
			host_timeout = packet_word(command_packet, 10);
			if (host_timeout) host_timeout = uint16_t(host_timeout + 2);
			accepted = build_response(4, 0, 32);
			if (accepted) {
				set_packet_word(response_packet, 9, controller_flags);
				set_packet_word(response_packet, 10, 120);
				set_packet_word(response_packet, 11, UDA50_SOFTWARE_VERSION);
				set_packet_word(response_packet, 15, uint16_t((2u << 8) | UDA50_PORT_MODEL));
			}
			break;
		case 3: { // Get Unit Status
			const uint16_t status = !media_present() ? ST_OFL_NV : (unit_online ? 0 : 4);
			accepted = build_response(3, status, 48);
			if (accepted && media_present()) {
				set_packet_word(response_packet, 9, 0x8000);
				set_packet_word(response_packet, 12, packet_word(command_packet, 4));
				set_packet_word(response_packet, 15, uint16_t((2u << 8) | 5));
				set_packet_word(response_packet, 16, 0x1051);
				set_packet_word(response_packet, 17, 0x2564);
				// RA81 geometry. GUS_CYL is groups per cylinder, not the
				// number of cylinders. RSX uses these fields when bringing
				// the booted unit online.
				set_packet_word(response_packet, 20, 51);
				set_packet_word(response_packet, 21, 14);
				set_packet_word(response_packet, 22, 1);
				set_packet_word(response_packet, 23, 0);
				set_packet_word(response_packet, 24, 2856);
				set_packet_word(response_packet, 25, 0401);
			}
			break;
		}
		case 9: // Online
			unit_online = media_present();
			accepted = build_response(9, unit_online ? 0 : ST_OFL_NV, 44);
			if (accepted && unit_online) {
				// Never advertise blocks beyond the mounted backing image.
				// A short image is still identified as an RA81, but exposing
				// its real LBN count prevents the guest from issuing writes
				// beyond the end of the file.
				const uint32_t blocks = media_size(media_context) / 512u;
				set_packet_word(response_packet, 9, 0x8000);
				set_packet_word(response_packet, 12, packet_word(command_packet, 4));
				set_packet_word(response_packet, 15, uint16_t((2u << 8) | 5));
				set_packet_word(response_packet, 16, 0x1051);
				set_packet_word(response_packet, 17, 0x2564);
				set_packet_word(response_packet, 20, uint16_t(blocks));
				set_packet_word(response_packet, 21, uint16_t(blocks >> 16));
				set_packet_word(response_packet, 22, 01234);
				set_packet_word(response_packet, 23, 0);
				tracef("ONLINE capacity blocks=%lu bytes=%lu",
					(unsigned long)blocks,
					(unsigned long)media_size(media_context));
			}
			break;
		case 10: // Set Unit Characteristics
			accepted = build_response(10, media_present() ? 0 : ST_OFL_NV, 44);
			break;
		case 8: // Available
			unit_online = false;
			accepted = build_response(8, media_present() ? 0 : ST_OFL_NV, 12);
			break;
		case 33: // Read
			accepted = transfer(false);
			break;
		case 34: // Write
			accepted = transfer(true);
			break;
		case 11: // Determine Access Paths
		case 17: // Compare Controller Data
		case 19: // Flush
			accepted = build_response(opcode, 0, 12);
			break;
		default:
			accepted = build_response(0, ST_BAD_OP, 12);
			break;
	}
	if (accepted) {
		static constexpr uint8_t COMMAND_RESPONSE_DELAY_TICKS = 16;
		response_delay_ticks = COMMAND_RESPONSE_DELAY_TICKS;
		tracef("MSCP complete op=%u rsp-op=%u status=%06o online=%u", (unsigned)opcode, (unsigned)(packet_word(response_packet, 6) & 0377), packet_word(response_packet, 7), unit_online ? 1u : 0u);
		command_valid = false;
		command_packet_length = 0;
		poll_pending = true;
	}
	return accepted;
}
void uda50::service_transport()
{
	if (response_valid && !post_response())
		return;
	if (poll_pending && !command_valid && !fetch_command())
		return;
	if (command_valid && !response_valid)
		process_command();
	if (response_valid)
		post_response();
}

bool uda50::take_command(uint8_t *const target, const uint16_t capacity,
		uint16_t *const length)
{
	if (!command_valid || !target || capacity < command_packet_length)
		return false;
	memcpy(target, command_packet, command_packet_length);
	if (length)
		*length = command_packet_length;
	command_valid = false;
	command_packet_length = 0;
	poll_pending = true;
	service_pending = true;
	return true;
}

bool uda50::submit_response(const uint8_t *const source, const uint16_t length)
{
	if (!source || length < 4 || length > PACKET_BYTES || response_valid)
		return false;
	memcpy(response_packet, source, length);
	response_packet_length = length;
	response_valid = true;
	service_pending = true;
	return true;
}

void uda50::service_deferred()
{
	if (service_pending)
		service_initialization();

	if (state == state_t::run)
		service_transport();
	service_pending = response_valid || (poll_pending && !command_valid);
}

bool uda50::transport_selftest()
{
	static constexpr uint32_t comm = 04000;
	static constexpr uint32_t command_payload = 05004;
	static constexpr uint32_t response_payload = 06004;
	uint8_t pattern[PACKET_BYTES] { 0 };
	uint8_t fetched[PACKET_BYTES] { 0 };
	for (uint16_t i = 0; i < PACKET_BYTES; ++i)
		pattern[i] = uint8_t(i ^ 0125);
	pattern[0] = 60;
	pattern[1] = 0;

	reset(true);
	write_word(UDA50_SA, S1_HOST_VALID);
	service_deferred();
	if (state != state_t::step2)
		return false;
	write_word(UDA50_SA, uint16_t(comm));
	service_deferred();
	write_word(UDA50_SA, uint16_t(comm >> 16));
	service_deferred();
	if (state != state_t::step4)
		return false;
	write_word(UDA50_SA, S4_HOST_GO);
	service_deferred();
	if (state != state_t::run)
		return false;

	const uint32_t response_descriptor = DESC_OWN | DESC_FLAG | response_payload;
	const uint32_t command_descriptor = DESC_OWN | DESC_FLAG | command_payload;
	b->write_unibus_word(comm, uint16_t(response_descriptor));
	b->write_unibus_word(comm + 2, uint16_t(response_descriptor >> 16));
	b->write_unibus_word(comm + 4, uint16_t(command_descriptor));
	b->write_unibus_word(comm + 6, uint16_t(command_descriptor >> 16));
	if (b->write_unibus_block(command_payload - 4, pattern, PACKET_BYTES) !=
			PACKET_BYTES)
		return false;

	read_word(UDA50_IP);
	if (!fetch_command())
		return false;
	uint16_t fetched_length = 0;
	if (!take_command(fetched, sizeof fetched, &fetched_length) ||
			fetched_length != PACKET_BYTES ||
			memcmp(pattern, fetched, PACKET_BYTES) != 0)
		return false;
	if (b->read_unibus_word(comm + 6) & 0x8000)
		return false;

	if (!submit_response(fetched, fetched_length))
		return false;
	if (!post_response())
		return false;
	uint8_t returned[PACKET_BYTES] { 0 };
	if (b->read_unibus_block(response_payload - 4, returned, PACKET_BYTES) !=
			PACKET_BYTES ||
			memcmp(pattern, returned, PACKET_BYTES) != 0)
		return false;
	if (b->read_unibus_word(comm + 2) & 0x8000)
		return false;
	if (b->read_unibus_word(comm - 2) != 1)
		return false;

	reset(true);
	return true;
}
bool uda50::mscp_selftest()
{
	static constexpr uint32_t comm = 07000;
	static constexpr uint32_t command_payload = 010004;
	static constexpr uint32_t response_payload = 011004;
	static constexpr uint32_t own_flag = DESC_OWN | DESC_FLAG;
	uint8_t command[PACKET_BYTES] { 0 };
	uint8_t response[PACKET_BYTES] { 0 };

	reset(true);
	write_word(UDA50_SA, S1_HOST_VALID); service_deferred();
	write_word(UDA50_SA, uint16_t(comm)); service_deferred();
	write_word(UDA50_SA, uint16_t(comm >> 16)); service_deferred();
	write_word(UDA50_SA, S4_HOST_GO); service_deferred();
	if (state != state_t::run) return false;

	set_packet_word(command, 0, 32);
	set_packet_word(command, 2, 012345);
	set_packet_word(command, 3, 067);
	set_packet_word(command, 6, 4);
	set_packet_word(command, 9, 0x0080);
	set_packet_word(command, 10, 10);
	b->write_unibus_word(comm, uint16_t(own_flag | response_payload));
	b->write_unibus_word(comm + 2, uint16_t((own_flag | response_payload) >> 16));
	b->write_unibus_word(comm + 4, uint16_t(own_flag | command_payload));
	b->write_unibus_word(comm + 6, uint16_t((own_flag | command_payload) >> 16));
	if (b->write_unibus_block(command_payload - 4, command, sizeof command) != sizeof command)
		return false;

	read_word(UDA50_IP);
	for (uint8_t i = 0; i < 18; ++i)
		service_deferred();
	if (b->read_unibus_block(response_payload - 4, response, sizeof response) != sizeof response)
		return false;
	if (packet_word(response, 0) != 32 ||
			(packet_word(response, 1) & 0xf) != 15 ||
			packet_word(response, 2) != 012345 ||
			packet_word(response, 3) != 067 ||
			uint8_t(packet_word(response, 6)) != uint8_t(4 | 0x80) ||
			packet_word(response, 7) != 0 ||
			packet_word(response, 9) != 0x0080 ||
			packet_word(response, 15) != uint16_t((2u << 8) | UDA50_PORT_MODEL))
		return false;

	reset(true);
	return true;
}

bool uda50::media_selftest()
{
	uint8_t media[1024] { 0 };
	uint8_t pattern[512] { 0 };
	uint8_t returned[512] { 0 };
	for (uint16_t i = 0; i < sizeof pattern; ++i)
		pattern[i] = uint8_t(i ^ 0xa5);

	void *const saved_context = media_context;
	const media_read_fn saved_read = media_read;
	const media_write_fn saved_write = media_write;
	const media_size_fn saved_size = media_size;
	const bool saved_online = unit_online;
	attach_media(media,
		[](void *c, uint32_t o, uint8_t *d, uint32_t n) -> bool {
			if (o + n > 1024) return false;
			memcpy(d, static_cast<uint8_t *>(c) + o, n); return true;
		},
		[](void *c, uint32_t o, const uint8_t *d, uint32_t n) -> bool {
			if (o + n > 1024) return false;
			memcpy(static_cast<uint8_t *>(c) + o, d, n); return true;
		},
		[](void *) -> uint32_t { return 1024; });
	unit_online = true;
	static constexpr uint32_t dma = 012000;
	bool ok = b->write_unibus_block(dma, pattern, sizeof pattern) == sizeof pattern;
	memset(command_packet, 0, sizeof command_packet);
	set_packet_word(command_packet, 8, sizeof pattern);
	set_packet_word(command_packet, 10, uint16_t(dma));
	set_packet_word(command_packet, 11, uint16_t(dma >> 16));
	set_packet_word(command_packet, 16, 1);
	command_valid = true;
	response_valid = false;
	ok = ok && transfer(true) && packet_word(response_packet, 8) == sizeof pattern &&
		memcmp(media + 512, pattern, sizeof pattern) == 0;

	uint8_t zero[512] { 0 };
	ok = ok && b->write_unibus_block(dma, zero, sizeof zero) == sizeof zero;
	response_valid = false;
	command_valid = true;
	ok = ok && transfer(false) && packet_word(response_packet, 8) == sizeof pattern;
	ok = ok && b->read_unibus_block(dma, returned, sizeof returned) == sizeof returned;
	ok = ok && memcmp(returned, pattern, sizeof pattern) == 0;

	memset(command_packet, 0, sizeof command_packet);
	set_packet_word(command_packet, 6, 9);
	command_valid = true;
	response_valid = false;
	response_delay_ticks = 0;
	unit_online = false;
	ok = ok && process_command() &&
		packet_word(response_packet, 20) == 2 &&
		packet_word(response_packet, 21) == 0;

	media_context = saved_context;
	media_read = saved_read;
	media_write = saved_write;
	media_size = saved_size;
	unit_online = saved_online;
	command_valid = false;
	response_valid = false;
	return ok;
}