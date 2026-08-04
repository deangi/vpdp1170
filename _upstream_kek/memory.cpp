// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#if defined(ESP32)
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "memory.h"


#if defined(TEENSY4_1)
extern "C" uint8_t external_psram_size;
#endif

memory::memory(const uint32_t logical_size, const uint32_t capacity_bytes)
{
	capacity = capacity_bytes ? capacity_bytes : logical_size;
	if (capacity < logical_size)
		capacity = logical_size;
	size = logical_size;

#if defined(ESP32)
	DOLOG(log_ss::LS_GENERIC,
	      "Memory size (in bytes, decimal): %u (capacity %u)",
	      (unsigned)size, (unsigned)capacity);

	if (psramFound()) {
		DOLOG(log_ss::LS_GENERIC, "Using PSRAM");
		m = reinterpret_cast<uint8_t *>(ps_malloc(capacity));
	}
	else {
		m = reinterpret_cast<uint8_t *>(calloc(1, capacity));
	}
#elif defined(TEENSY4_1)
	if (external_psram_size >= capacity / 1024 / 1024) {
		m = reinterpret_cast<uint8_t *>(extmem_malloc(capacity));
	}
	else {
		m = reinterpret_cast<uint8_t *>(calloc(1, capacity));
	}
#elif defined(BUILD_FOR_PICO2W)
	uint32_t psram_pages = rp2040.getFreePSRAMHeap() / 8192;
	uint32_t main_ram    = rp2040.getFreeHeap() / 8192;
	if (main_ram < psram_pages) {
		m = reinterpret_cast<uint8_t *>(pmalloc(capacity));
	}
	else {
		m = reinterpret_cast<uint8_t *>(calloc(1, capacity));
	}
#else
	m = reinterpret_cast<uint8_t *>(calloc(1, capacity));
#endif
}

memory::~memory()
{
#if defined(TEENSY4_1)
	if (external_psram_size >= capacity / 1024 / 1024)
		extmem_free(m);
	else
		free(m);
#else
	free(m);
#endif
}

void memory::set_logical_size(const uint32_t new_size)
{
	size = std::min(new_size, capacity);
}

void memory::reset(const bool hard)
{
	// Clear only the visible PDP RAM. Capacity above size is retained for
	// reuse across emulator resets and is left untouched.
	if (!(hard && m && size))
		return;
#if defined(ESP32)
	// Chunked clear so a multi-megabyte PSRAM zero does not stall the
	// FreeRTOS scheduler / UI / network for seconds without yielding.
	constexpr uint32_t kChunk = 65536u;
	for (uint32_t off = 0; off < size; ) {
		const uint32_t n = std::min(kChunk, size - off);
		memset(m + off, 0x00, n);
		off += n;
		if (off < size)
			vTaskDelay(1);
	}
#else
	memset(m, 0x00, size);
#endif
}

#if IS_POSIX
JsonDocument memory::serialize() const
{
	JsonDocument j;

	j["size"] = size;

	JsonDocument ja;
	JsonArray ja_work = ja.to<JsonArray>();
	for(size_t i=0; i<size; i++)
		ja_work.add(m[i]);
	j["contents"] = ja;

	return j;
}

memory *memory::deserialize(const JsonVariantConst j)
{
	size_t  size = j["size"];
	memory *m    = new memory(size);

	JsonArrayConst ja = j["contents"].as<JsonArrayConst>();
	uint32_t  i  = 0;
	for(auto v: ja)
		m->m[i++] = v;

	return m;
}
#endif
