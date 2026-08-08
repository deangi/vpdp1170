// (C) 2018-2026 by Folkert van Heusden
// Released under MIT license

#pragma once

#include "gen.h"
#if IS_POSIX
#include <ArduinoJson.h>
#endif
#include <assert.h>
#include <cstring>
#include <mutex>
#include <stdint.h>
#include <stdio.h>

#include "device.h"
#include "dc11.h"
#include "dz11.h"
#include "mmu.h"
#include "rk05.h"
#include "rl02.h"
#include "rp06.h"
#include "uda50.h"
#if !defined(TEENSY4_1)
#include "tm-11.h"
#endif

#define ADDR_MMR0 0177572
#define ADDR_MMR1 0177574
#define ADDR_MMR2 0177576
#define ADDR_MMR3 0172516

#define ADDR_PIR  0177772
#define ADDR_LFC  0177546  // line frequency
#define ADDR_MAINT 0177750
#define ADDR_CONSW 0177570
#define ADDR_LP11CSR 0177514  // printer CSR (LP11)
#define ADDR_LP11DB  0177516  // printer data buffer

#define ADDR_PSW      0177776
#define ADDR_STACKLIM 0177774
#define ADDR_KERNEL_R 0177700
#define ADDR_USER_R   0177710
#define ADDR_KERNEL_SP 0177706
#define ADDR_PC       0177707
#define ADDR_SV_SP    0177716
#define ADDR_USER_SP  0177717

#define ADDR_CPU_ERR 0177766
#define ADDR_SYSSIZE 0177760
#define ADDR_MICROPROG_BREAK_REG 0177770
#define ADDR_CCR 0177746
#define ADDR_SYSTEM_ID 0177764
#define ADDR_UNIBUS_MAP_START 0170200
#define ADDR_UNIBUS_MAP_END   0170377

constexpr uint32_t UNIBUS_MAP_PAGE_SIZE = 020000;
constexpr uint32_t UNIBUS_MAP_PAGE_MASK = UNIBUS_MAP_PAGE_SIZE - 1;
constexpr int      UNIBUS_MAP_ENTRIES   = 32;

class console;
class cpu;
class dc11;
class deqna;
class dz11;
class kw11_l;
class memory;
class rk05;
class rl02;
class rp06;
class uda50;
#if !defined(TEENSY4_1)
class tm_11;
#endif
class tty;

typedef struct {
	bool is_psw;
} write_rc_t;

class bus: public device
{
private:
	cpu     *c       { nullptr };
#if !defined(TEENSY4_1)
	tm_11   *tm11    { nullptr };
#endif
	rk05    *rk05_   { nullptr };
	rl02    *rl02_   { nullptr };
	tty     *tty_    { nullptr };
	kw11_l  *kw11_l_ { nullptr };
	mmu     *mmu_    { nullptr };
	memory  *m       { nullptr };
	dc11    *dc11_   { nullptr };
	dz11    *dz11_   { nullptr };
	rp06    *rp06_   { nullptr };
	uda50   *uda50_  { nullptr };
	deqna   *deqna_  { nullptr };

	uint16_t microprogram_break_register { 0 };

	uint16_t console_switches { 0 };
	uint16_t console_leds     { 0 };
	uint32_t unibus_map[UNIBUS_MAP_ENTRIES] { 0 };

	// Guest instruction cache (#3): 128 × 32B = 4 KB, direct-mapped by PA.
	static constexpr int ICACHE_LINES = 128;
	static constexpr int ICACHE_LINE_BYTES = 32;
	static constexpr int ICACHE_LINE_SHIFT = 5;
	uint8_t  icache_data[ICACHE_LINES][ICACHE_LINE_BYTES] { };
	uint32_t icache_tag[ICACHE_LINES] { };
	uint8_t  icache_valid[ICACHE_LINES] { };

	KEK_ALWAYS_INLINE void icache_flush() {
		memset(icache_valid, 0, sizeof icache_valid);
	}

	KEK_ALWAYS_INLINE void icache_invalidate_pa(const uint32_t pa) {
		const uint32_t line = pa >> ICACHE_LINE_SHIFT;
		const int idx = int(line & (ICACHE_LINES - 1));
		if (icache_valid[idx] && icache_tag[idx] == line)
			icache_valid[idx] = 0;
	}

	KEK_ALWAYS_INLINE uint16_t icache_fetch_word(const uint32_t pa) {
		const uint32_t line = pa >> ICACHE_LINE_SHIFT;
		const int idx = int(line & (ICACHE_LINES - 1));
		const int off = int(pa & (ICACHE_LINE_BYTES - 1));
		if (!(icache_valid[idx] && icache_tag[idx] == line)) [[unlikely]] {
			const uint32_t base = line << ICACHE_LINE_SHIFT;
			const uint32_t mem_size = m->get_memory_size();
			if (base + ICACHE_LINE_BYTES <= mem_size) [[likely]] {
				m->read_block(base, icache_data[idx], ICACHE_LINE_BYTES);
			}
			else {
				memset(icache_data[idx], 0, ICACHE_LINE_BYTES);
				if (base < mem_size)
					m->read_block(base, icache_data[idx], mem_size - base);
			}
			icache_tag[idx] = line;
			icache_valid[idx] = 1;
		}
#if __BYTE_ORDER == __LITTLE_ENDIAN
		return *reinterpret_cast<uint16_t *>(&icache_data[idx][off]);
#else
		return uint16_t(icache_data[idx][off] | (icache_data[idx][off + 1] << 8));
#endif
	}

	uint16_t read_IO (const uint16_t a, const word_mode_t word_mode, const int run_mode, const d_i_space_t space, const int page, const int page_index);
	bool     write_IO(const uint16_t a, const word_mode_t word_mode,                                              const int page, uint16_t value);

	void     verify_pointer_bounds(const uint32_t m_offset, const int page_index);
	void     reset_unibus_map();
	uint16_t read_unibus_map_register(const uint16_t a, const word_mode_t word_mode) const;
	void     write_unibus_map_register(const uint16_t a, const word_mode_t word_mode, const uint16_t value);
	uint32_t translate_unibus_address(const uint32_t a) const;

public:
	bus();
	~bus();

#if IS_POSIX
	JsonDocument serialize() const;
	static bus *deserialize(const JsonDocument j, console *const cnsl, kek_event_t *const event);
#endif

	void reset(const bool hard) override;
	void init ();

	void show_state(console *const cnsl) const override;

	void     set_console_switches(const uint16_t new_state       ) { console_switches = new_state; }
	void     set_console_switch  (const int bit, const bool state) { console_switches &= ~(1 << bit); console_switches |= state << bit; }
	uint16_t get_console_switches() { return console_switches; }
	void     set_debug_mode      () { console_switches |= 128; }
	uint16_t get_console_leds    () { return console_leds;     }

	// Set the visible PDP RAM size in 8 KB pages. When capacity_pages > 0,
	// the host buffer is allocated (or retained) at that capacity so later
	// smaller sizes reuse the same slab instead of free/ps_malloc.
	void set_memory_size(const int n_pages, const int capacity_pages = 0);
	uint32_t get_memory_size() const { return m->get_memory_size(); }

	void add_ram   (memory *const m      );
	void add_cpu   (cpu    *const c      );
	void add_mmu   (mmu    *const mmu_   );
#if !defined(TEENSY4_1)
	void add_tm11  (tm_11  *const tm11   );
#endif
	void add_rk05  (rk05   *const rk05_  );
	void add_rl02  (rl02   *const rl02_  );
	void add_tty   (tty    *const tty_   );
	void add_KW11_L(kw11_l *const kw11_l_);
	void add_DC11  (dc11   *const dc11_  );
	void add_DZ11  (dz11   *const dz11_  );
	// required to release devices when doing a reload
	void del_DZ11  ();
	void add_RP06  (rp06   *const rp06_  );
	void add_UDA50 (uda50  *const uda50_ );
	void add_DEQNA (deqna  *const deqna_ );

	memory *getRAM()    { return m;       }
	cpu    *getCpu()    { return c;       }
	kw11_l *getKW11_L() { return kw11_l_; }
	tty    *getTty()    { return tty_;    }
	mmu    *getMMU()    { return mmu_;    }
	rk05   *getRK05()   { return rk05_;   }
	rl02   *getRL02()   { return rl02_;   }
	dc11   *getDC11()   { return dc11_;   }
	dz11   *getDZ11()   { return dz11_;   }
#if !defined(TEENSY4_1)
	tm_11  *getTM11()   { return tm11;    }
#endif
	rp06   *getRP06()   { return rp06_;   }
	uda50  *getUDA50()  { return uda50_;  }
	deqna  *getDEQNA()  { return deqna_;  }
	uint32_t get_unibus_map_entry(int entry) const {
		return entry >= 0 && entry < UNIBUS_MAP_ENTRIES ? unibus_map[entry] : 0;
	}
	uint32_t translate_unibus_for_monitor(uint32_t address) const {
		return translate_unibus_address(address);
	}

	KEK_HOT uint16_t read(const uint16_t a, const word_mode_t word_mode, const int run_mode, const d_i_space_t s = i_space);
	uint8_t  read_byte(const uint16_t a) override { return read(a, wm_byte, c->getPSW_runmode()); }
	uint16_t read_word(const uint16_t a, const d_i_space_t s);
	uint16_t read_word(const uint16_t a) override { return read_word(a, i_space); }
	KEK_HOT uint16_t fetch_instruction_word(const uint16_t a, const int run_mode, uint32_t *physical);
	std::optional<uint16_t> peek_word(const int run_mode, const uint16_t a);
	uint8_t  read_unibus_byte(const uint32_t a) const;
	uint16_t read_unibus_word(const uint32_t a) const;
	uint32_t read_unibus_block(uint32_t a, uint8_t *target, uint32_t n) const;
	uint16_t read_physical(const uint32_t a);
	uint16_t read_physical_byte(const uint32_t a);
	uint32_t read_physical_block(uint32_t a, uint8_t *target, uint32_t n) const;
	bool     clear_physical_block(const uint32_t a, const uint32_t n);

	KEK_HOT bool write(const uint16_t a, const word_mode_t word_mode, const uint16_t value, const int run_mode, const d_i_space_t s = i_space);
	void     write_unibus_byte(const uint32_t a, const uint8_t value);
	uint32_t write_unibus_block(uint32_t a, const uint8_t *source, uint32_t n);
	void     write_byte(const uint16_t a, const uint8_t value) override { write(a, wm_byte, value, c->getPSW_runmode()); }
	void     write_word(const uint16_t a, const uint16_t value, const d_i_space_t s);
	void     write_word(const uint16_t a, const uint16_t value) override { write_word(a, value, i_space); }
	void     write_physical(const uint32_t a, const uint16_t value);
	void     write_physical_byte(const uint32_t a, const uint8_t value);
	uint32_t write_physical_block(uint32_t a, const uint8_t *source, uint32_t n);
	void     write_unibus_word(const uint32_t a, const uint16_t value);
};
