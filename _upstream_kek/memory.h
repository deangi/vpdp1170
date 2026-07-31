// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#pragma once

#include "gen.h"
#if IS_POSIX
#include <ArduinoJson.h>
#endif
#include <cstdint>
#include <cstring>
#if defined(BUILD_FOR_PICO2W) || defined(TEENSY4_1)  // TODO also teensy4.1?
#define __LITTLE_ENDIAN 1
#define __BYTE_ORDER __LITTLE_ENDIAN
#elif !defined(_WIN32) && !defined(__APPLE__)
#include <endian.h>
#endif

class memory
{
private:
	// capacity: allocated host buffer size (bytes). size: logical PDP RAM
	// visible to the bus (bytes). size is always <= capacity so a full-size
	// PSRAM slab can be allocated once and reused for smaller guests.
	uint32_t       capacity { 0       };
	uint32_t       size     { 0       };
	uint8_t       *m        { nullptr };

public:
	// Allocate capacity_bytes (or logical_size when capacity_bytes == 0).
	// Does not zero the buffer; callers that need a clean slate use reset().
	explicit memory(const uint32_t logical_size,
	                const uint32_t capacity_bytes = 0);
	~memory();

	uint32_t get_memory_size() const { return size; }
	uint32_t get_capacity() const { return capacity; }

	// Change the visible PDP RAM size without freeing/reallocating. new_size
	// is clamped to capacity. Does not clear memory.
	void set_logical_size(const uint32_t new_size);

	void reset(const bool hard);

#if IS_POSIX
	JsonDocument serialize() const;
	static memory *deserialize(const JsonVariantConst j);
#endif

	uint16_t read_byte(const uint32_t a) const { return m[a]; }
	void write_byte(const uint32_t a, const uint16_t v) { m[a] = v; }
	bool read_block(const uint32_t a, uint8_t *const target, const uint32_t n) const {
		if (!target || a > size || n > size - a)
			return false;
		memcpy(target, &m[a], n);
		return true;
	}
	bool write_block(const uint32_t a, const uint8_t *const source, const uint32_t n) {
		if (!source || a > size || n > size - a)
			return false;
		memcpy(&m[a], source, n);
		return true;
	}
	bool clear_block(const uint32_t a, const uint32_t n) {
		if (a > size || n > size - a)
			return false;
		memset(&m[a], 0x00, n);
		return true;
	}

#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint16_t read_word(const uint32_t a) const { return *reinterpret_cast<uint16_t *>(&m[a]); }
	void write_word(const uint32_t a, const uint16_t v) { *reinterpret_cast<uint16_t *>(&m[a]) = v; }
#else
	uint16_t read_word(const uint32_t a) const { return m[a] | (m[a + 1] << 8); }
	void write_word(const uint32_t a, const uint16_t v) { m[a] = v; m[a + 1] = v >> 8; }
#endif
};
