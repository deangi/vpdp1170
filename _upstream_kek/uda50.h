// UDA50 Unibus port and UQSSP transport.
// MSCP command interpretation is implemented separately.

#pragma once

#include <stdint.h>

#include "device.h"

#define UDA50_IP   0172150
#define UDA50_SA   0172152
#define UDA50_BASE UDA50_IP
#define UDA50_END  (UDA50_SA + 2)

class bus;

class uda50 final: public device
{
public:
	enum class state_t : uint8_t {
		step1,
		step1_wrap,
		step2,
		step3,
		step3_purge_sa,
		step3_purge_ip,
		step4,
		run,
		dead
	};

	explicit uda50(bus *const bus_);

	void reset(const bool hard) override;
	void show_state(console *const cnsl) const override;

	uint8_t  read_byte(const uint16_t addr) override;
	uint16_t read_word(const uint16_t addr) override;
	void write_byte(const uint16_t addr, const uint8_t v) override;
	void write_word(const uint16_t addr, const uint16_t v) override;

	void service_deferred();
	using trace_fn = void (*)(void *, const char *);
	void set_trace(uint32_t count, void *context, trace_fn fn);
	uint32_t trace_remaining() const { return trace_left; }
	bool transport_selftest();
	bool mscp_selftest();
	bool media_selftest();

	using media_read_fn = bool (*)(void *, uint32_t, uint8_t *, uint32_t);
	using media_write_fn = bool (*)(void *, uint32_t, const uint8_t *, uint32_t);
	using media_size_fn = uint32_t (*)(void *);
	void attach_media(void *context, media_read_fn read_fn, media_write_fn write_fn,
		media_size_fn size_fn);

	static constexpr uint16_t PACKET_BYTES = 64;
	bool command_pending() const { return command_valid; }
	bool take_command(uint8_t *target, uint16_t capacity, uint16_t *length);
	bool submit_response(const uint8_t *source, uint16_t length);

	state_t  get_state() const { return state; }
	uint16_t get_sa() const { return sa; }
	uint16_t get_interrupt_vector() const { return interrupt_vector; }
	uint32_t get_communication_address() const { return communication_address; }
	uint16_t get_command_ring_length() const { return command_ring_length; }
	uint16_t get_response_ring_length() const { return response_ring_length; }
	bool     is_running() const { return state == state_t::run; }
	bool     needs_deferred_service() const {
		return service_pending || poll_pending || state == state_t::run;
	}

private:
	static constexpr uint16_t SA_ERROR = 0x8000;
	static constexpr uint16_t SA_STEP4 = 0x4000;
	static constexpr uint16_t SA_STEP3 = 0x2000;
	static constexpr uint16_t SA_STEP2 = 0x1000;
	static constexpr uint16_t SA_STEP1 = 0x0800;

	static constexpr uint16_t S1_CONTROLLER_DIAGNOSTICS = 0x0100;
	static constexpr uint16_t S1_CONTROLLER_MAPPING = 0x0040;
	static constexpr uint16_t S1_HOST_VALID = 0x8000;
	static constexpr uint16_t S1_HOST_WRAP = 0x4000;
	static constexpr uint16_t S1_HOST_INTERRUPT_ENABLE = 0x0080;
	static constexpr uint16_t S1_HOST_VECTOR = 0x007f;
	static constexpr uint16_t S2_HOST_COMM_LOW = 0xfffe;
	static constexpr uint16_t S3_HOST_PURGE_POLL = 0x8000;
	static constexpr uint16_t S3_HOST_COMM_HIGH = 0x7fff;
	static constexpr uint16_t S4_HOST_GO = 0x0001;

	static constexpr uint16_t UDA50_PORT_MODEL = 6;
	static constexpr uint16_t UDA50_SOFTWARE_VERSION = 3;
	static constexpr uint32_t DESC_OWN = 0x80000000u;
	static constexpr uint32_t DESC_FLAG = 0x40000000u;
	static constexpr uint32_t DESC_ADDRESS = 0x003ffffeu;
	static constexpr uint16_t ERROR_PACKET_READ = 1;
	static constexpr uint16_t ERROR_PACKET_WRITE = 2;
	static constexpr uint16_t ERROR_QUEUE_READ = 6;
	static constexpr uint16_t ERROR_QUEUE_WRITE = 7;

	struct ring_t {
		int8_t interrupt_offset { 0 };
		uint32_t base { 0 };
		uint16_t byte_length { 0 };
		uint16_t index { 0 };
	};

	bus *const b;
	state_t state { state_t::step1 };
	uint16_t sa { 0 };
	uint16_t pending_sa_write { 0 };
	uint16_t step1_data { 0 };
	uint16_t interrupt_vector { 0 };
	uint16_t command_ring_length { 0 };
	uint16_t response_ring_length { 0 };
	uint32_t communication_address { 0 };
	bool interrupt_enabled { false };
	bool purge_interrupt { false };
	bool service_pending { false };
	bool poll_pending { false };
	bool command_valid { false };
	bool response_valid { false };
	uint8_t response_delay_ticks { 0 };
	bool first_mscp_response { true };
	uint16_t controller_flags { 0 };
	uint16_t host_timeout { 0 };
	bool unit_online { false };
	uint32_t trace_left { 0 };
	void *trace_context { nullptr };
	trace_fn trace_callback { nullptr };
	void *media_context { nullptr };
	media_read_fn media_read { nullptr };
	media_write_fn media_write { nullptr };
	media_size_fn media_size { nullptr };
	uint16_t command_packet_length { 0 };
	uint16_t response_packet_length { 0 };
	uint8_t command_packet[PACKET_BYTES] { 0 };
	uint8_t response_packet[PACKET_BYTES] { 0 };
	ring_t command_ring;
	ring_t response_ring;

	void tracef(const char *format, ...);
	void begin_step4();
	void post_initialization_interrupt();
	void post_ring_interrupt(const ring_t &ring);
	void service_initialization();
	void service_transport();
	void fatal(uint16_t error);
	bool initialize_communication_area();
	bool read_descriptor(const ring_t &ring, uint32_t *descriptor) const;
	bool release_descriptor(ring_t *ring, uint32_t descriptor);
	bool fetch_command();
	bool post_response();
	bool process_command();
	bool transfer(bool write);
	bool media_present() const;
	bool build_response(uint8_t opcode, uint16_t status, uint16_t payload_length);
	uint16_t packet_word(const uint8_t *packet, uint16_t index) const;
	void set_packet_word(uint8_t *packet, uint16_t index, uint16_t value);
	uint16_t packet_length(const uint8_t *packet) const;
	void update_byte(uint16_t *word, const uint16_t addr, const uint8_t value);
};
