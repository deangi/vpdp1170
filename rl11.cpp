// rl11.cpp - RL11 / RL01-RL02 disk controller (vpdp1140 m3, fresh implementation).
//
// Implements the four operations the M9312 / sam11 RL boot ROM uses
// (get status, read header, seek, read data) plus minimal write data, so
// guest operating systems that boot from RL packs can load their first
// blocks. Data movement uses our disk_read/disk_write block I/O against
// the SD-card image files; no SdFat dependency.
//
// Register addresses live in pdp1140.h:
//   0774400  RLCS   Control / Status
//   0774402  RLBA   Bus Address (low 16 bits)
//   0774404  RLDA   Disk Address  (cyl/surf/sec packed)
//   0774406  RLMP   Multi-Purpose (word count for r/w, status word, etc.)
//   0774420  RLBAE  Bus Address Extension (high bits, 22-bit systems)
//
// RLDA layout (DEC RL11 manual):
//   bits 15:7 = cylinder (9 bits, 0..511 for RL02, 0..255 for RL01)
//   bit  6    = surface (0 or 1)
//   bits 5:0  = sector (0..39 valid; >=40 = HNF error)
//
// Geometry (both RL01 and RL02 use the same per-track layout; only the
// cylinder count differs):
//   sectors per track = 40
//   surfaces per cylinder = 2
//   bytes per sector = 256
//   RL01 = 256 cyl => 5,242,880 bytes
//   RL02 = 512 cyl => 10,485,760 bytes

#include "rl11.h"
#include "pdp1140.h"
#include "sam11_platform.h"
#include "sam11.h"
#include "dd11.h"
#include "disk.h"
#include "platform.h"
#ifdef PS
#undef PS
#endif
#include "kb11.h"
#include "kd11.h"

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

namespace rl11 {

static const uint32_t SECS_PER_TRACK = 40;
static const uint32_t SURFACES       = 2;
static const uint32_t BYTES_PER_SEC  = 256;
static const uint16_t RL01_CYLINDERS = 256;
static const uint16_t RL02_CYLINDERS = 512;
static const uint16_t IRQ_DELAY_TICKS = 2;

// Programmable registers (all 16-bit on the bus).
static uint16_t RLCS  = 0;
static uint16_t RLBA  = 0;
static uint16_t RLDA  = 0;
static uint16_t RLMP  = 0;
static uint16_t RLBAE = 0;

// Track current head position separately from RLDA, because RLDA is
// overloaded: for r/w it holds target address, for seek it holds a
// seek-diff command, and for status reads it should reflect the current
// position. We update these on SEEK and on read/write completion.
uint16_t rl_cur_cyl  = 0;
uint16_t rl_cur_surf = 0;

bool attached[4] = { false, false, false, false };
static uint16_t media_cylinders[4] = { 0, 0, 0, 0 };
static bool media_is_rl02[4] = { false, false, false, false };
static uint16_t irq_pending = 0;
static constexpr bool IRQ_TRACE = false;
static int irq_trace_left = 24;

static void trace_irq(const char *event, uint16_t value)
{
    if (IRQ_TRACE && irq_trace_left > 0) {
        LOG("RL11 IRQ %s val=%06o RLCS=%06o PC=%06o PS=%06o pending=%u",
            event, (unsigned)value, (unsigned)RLCS,
            (unsigned)kd11::curPC, (unsigned)kd11::PS,
            (unsigned)irq_pending);
        irq_trace_left--;
    }
}

bool valid_image_size(uint32_t bytes)
{
    return bytes == RL01_IMAGE_BYTES || bytes == RL02_IMAGE_BYTES;
}

const char* image_type_name(uint32_t bytes)
{
    if (bytes == RL01_IMAGE_BYTES) return "RL01";
    if (bytes == RL02_IMAGE_BYTES) return "RL02";
    return "invalid";
}

const char* mounted_media_type(int unit)
{
    if (unit < 0 || unit >= 4 || !disk_is_mounted(unit)) return "empty";
    return valid_image_size(disk_size_bytes(unit))
             ? image_type_name(disk_size_bytes(unit)) : "invalid";
}

static void refresh_media_type(int unit)
{
    if (unit < 0 || unit >= 4) return;
    media_cylinders[unit] = 0;
    media_is_rl02[unit] = false;
    attached[unit] = false;

    if (!disk_is_mounted(unit)) return;

    uint32_t bytes = disk_size_bytes(unit);
    if (bytes == RL01_IMAGE_BYTES) {
        media_cylinders[unit] = RL01_CYLINDERS;
        attached[unit] = true;
    } else if (bytes == RL02_IMAGE_BYTES) {
        media_cylinders[unit] = RL02_CYLINDERS;
        media_is_rl02[unit] = true;
        attached[unit] = true;
    }
}

bool validate_mounted_media(int unit)
{
    if (unit < 0 || unit >= 4) return false;
    refresh_media_type(unit);
    return attached[unit];
}

void reset()
{
    procNS::cancelinterrupt(INTRL);
    irq_pending = 0;
    irq_trace_left = 24;
    // Power-on default: control ready (bit 7) + drive ready (bit 0), no
    // errors. The bootrom polls bit 7 with TSTB / BPL .-2; some OS boot
    // loaders also check bit 0.
    RLCS  = 0x0081;
    RLBA  = 0;
    RLDA  = 0;
    RLMP  = 0;
    RLBAE = 0;
    rl_cur_cyl  = 0;
    rl_cur_surf = 0;
    // Map each RL drive to the corresponding disk slot (0..3). Only exact
    // RL01/RL02 image sizes are considered attached RL media.
    for (int i = 0; i < 4; i++) refresh_media_type(i);
}

void media_changed(int unit, bool mounted)
{
    if (unit < 0 || unit >= 4) return;
    if (mounted) refresh_media_type(unit);
    else {
        attached[unit] = false;
        media_cylinders[unit] = 0;
        media_is_rl02[unit] = false;
    }
    if (!mounted && (((RLCS >> 8) & 3) == unit))
        RLCS &= ~0x0001;
}

// Compute the byte offset into the disk image for RLDA's current
// cylinder/surface/sector triple.
static uint32_t da_to_offset(uint16_t da)
{
    uint32_t cyl  = (da >> 7) & 0x1FF;   // 9 bits
    uint32_t surf = (da >> 6) & 1;
    uint32_t sec  = da & 0x3F;           // 6 bits
    return ((cyl * SURFACES + surf) * SECS_PER_TRACK + sec) * BYTES_PER_SEC;
}

// 18-bit bus address: low 16 in RLBA, high 2 in RLCS bits 5:4.
static uint32_t bus_addr()
{
    return ((uint32_t)RLBA) | (((uint32_t)(RLCS & 0x0030)) << 12);
}

static void set_bus_addr(uint32_t ba)
{
    RLBA = ba & 0xFFFF;
    RLCS = (RLCS & ~0x0030) | (uint16_t)((ba >> 12) & 0x0030);
}

static const char* func_name(int f) {
    switch (f) {
    case 0: return "NOOP";
    case 1: return "WRTCHK";
    case 2: return "GETSTAT";
    case 3: return "SEEK";
    case 4: return "RDHDR";
    case 5: return "WRDATA";
    case 6: return "RDDATA";
    case 7: return "RDDATA-NOH";
    }
    return "?";
}

// Execute the function currently encoded in RLCS bits 3:1. Synchronous -
// the whole transfer happens before we return and set the ready bit.
// Real hardware would be interruptable; for our purposes instant is fine.
static void execute()
{
    int func = (RLCS >> 1) & 7;
    int drv  = (RLCS >> 8) & 3;

    // Clear all error bits first; we'll re-set if something goes wrong.
    RLCS &= ~0xFC00;

    if (!attached[drv]) {
        Serial.printf("[vpdp1170] RL11 %s drv=%d -> drive not attached\r\n",
                      func_name(func), drv);
        // Operation incomplete (bit 10) + composite error (bit 15).
        RLCS |= 0x8400;
        RLCS |= 0x0080;
        return;
    }

    // Log the first ~40 RL11 commands of each boot so the bootrom + early
    // disk activity is visible, then go quiet.
    static int dbg_left = 40;
    if (dbg_left > 0) {
        Serial.printf("[vpdp1170] RL11 %s drv=%d  DA=%06o MP=%06o BA=%06o CS=%06o  cur_cyl=%u surf=%u\r\n",
                      func_name(func), drv, RLDA, RLMP, RLBA, RLCS,
                      (unsigned)rl_cur_cyl, (unsigned)rl_cur_surf);
        dbg_left--;
        if (dbg_left == 0)
            Serial.printf("[vpdp1170] RL11 (silencing further command logs)\r\n");
    }

    switch (func) {
    case 0:   // NO-OP / maintenance
        break;

    case 1:   // WRITE CHECK - we just claim success
        break;

    case 2: { // GET STATUS
        // RLMP returns the drive status word.
        // Bit layout (DEC RL11 spec): bits 2:0=state(5=lock-on),
        // bit 3=brush home, bit 4=heads out, bit 6=head select,
        // bit 7=drive type (RL02), bit 13=write lock.
        uint16_t mp = 0;
        mp |= 0000005;       // state 5 = lock-on
        mp |= 0000010;       // brush home
        mp |= 0000020;       // heads out
        if (rl_cur_surf) mp |= 0000100;
        if (media_is_rl02[drv]) mp |= 0000200;
        if (disk_is_readonly(drv)) mp |= 0020000;
        RLMP = mp;
        break;
    }

    case 3: { // SEEK
        // DEC RL11 seek-difference word lives in RLDA (NOT RLMP):
        //   bit  0    = marker (must be 1 for a valid seek)
        //   bit  2    = direction (0 = -, 1 = +)
        //   bit  4    = head select (0 = surface 0, 1 = surface 1)
        //   bits 15:7 = cylinder difference (number of cylinders to move)
        // We track current cylinder/surface in module-static state so
        // we can report a sensible "current position" on subsequent
        // r/w and RDHDR commands.
        if ((RLDA & 1) == 0) break;             // no marker -> no-op
        uint16_t diff   = (RLDA & 0xFF80) >> 7; // cyl diff (9 bits)
        uint16_t newsurf = (RLDA >> 4) & 1;
        bool     dir_in = (RLDA & 4) != 0;       // 1 = toward higher cyl
        uint16_t max_cyl = media_cylinders[drv] ? media_cylinders[drv] - 1 : 0;
        if (dir_in) rl_cur_cyl = (rl_cur_cyl + diff > max_cyl) ? max_cyl : (rl_cur_cyl + diff);
        else        rl_cur_cyl = (diff > rl_cur_cyl) ? 0 : (rl_cur_cyl - diff);
        rl_cur_surf = newsurf;
        // After seek, RLDA reports the new (cyl, surf, sec=0) position.
        RLDA = (uint16_t)((rl_cur_cyl << 7) | (rl_cur_surf << 6));
        break;
    }

    case 4: { // READ HEADER
        // Real RL hardware returns the address of the *next sector under
        // the head*, which changes as the platter rotates. Boot loaders
        // like XXDP poll RDHDR in a tight loop waiting for "their" sector
        // to come around. If we return a static value they spin forever.
        // We fake rotation by incrementing the sector field on each call.
        // Position comes from our tracked cur_cyl/cur_surf, not RLDA
        // (which may hold a seek-cmd or r/w target).
        static uint16_t rotating_sec = 0;
        RLMP = (uint16_t)((rl_cur_cyl << 7) | (rl_cur_surf << 6) | rotating_sec);
        rotating_sec = (rotating_sec + 1) % SECS_PER_TRACK;
        break;
    }

    case 5:   // WRITE DATA
    case 6:   // READ DATA
    case 7: { // READ DATA WITHOUT HEADER CHECK
        bool writing = (func == 5);
        uint32_t cyl = (RLDA >> 7) & 0x1FF;
        uint32_t sec = RLDA & 0x3F;
        if (sec >= SECS_PER_TRACK || cyl >= media_cylinders[drv]) {
            RLCS |= 0x8400;
            break;
        }
        uint32_t off = da_to_offset(RLDA);
        uint32_t ba  = bus_addr();
        // Word count is 2's-complement in RLMP; transfer terminates
        // when it rolls to 0.
        // Pull/push 512 bytes at a time to amortize SD overhead.
        uint8_t  scratch[512];
        uint32_t words_left = (uint32_t)((~RLMP + 1) & 0xFFFF);
        uint32_t total_words = words_left;
        if (words_left == 0) { RLCS |= 0x0080; return; }

        while (words_left) {
            uint32_t chunk_words = words_left > 256 ? 256 : words_left;
            uint32_t chunk_bytes = chunk_words * 2;

            if (writing) {
                // pull from guest memory into our buffer
                for (uint32_t i = 0; i < chunk_words; i++) {
                    uint16_t v = dd11::read16((ba + i * 2) & 0777777u);
                    scratch[i * 2]     = (uint8_t)(v & 0xFF);
                    scratch[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
                }
                if (disk_write(drv, off, scratch, chunk_bytes) < 0) {
                    RLCS |= 0x8400;  // op incomplete + composite error
                    break;
                }
            } else {
                if (disk_read(drv, off, scratch, chunk_bytes) < 0) {
                    RLCS |= 0x8400;
                    break;
                }
                for (uint32_t i = 0; i < chunk_words; i++) {
                    uint16_t v = (uint16_t)scratch[i * 2]
                               | ((uint16_t)scratch[i * 2 + 1] << 8);
                    dd11::write16((ba + i * 2) & 0777777u, v);
                }
            }

            ba          = (ba + chunk_bytes) & 0777777u;
            off        += chunk_bytes;
            words_left -= chunk_words;
            RLMP       = (uint16_t)(RLMP + chunk_words);  // ticks toward 0
        }
        set_bus_addr(ba);
        // Advance RLDA's sector/surface/cyl by the number of sectors
        // we just transferred, and sync our shadowed head position.
        {
            uint32_t total_secs = (off - da_to_offset(RLDA)) / BYTES_PER_SEC;
            uint32_t cyl  = (RLDA >> 7) & 0x1FF;
            uint32_t surf = (RLDA >> 6) & 1;
            uint32_t sec  = RLDA & 0x3F;
            sec += total_secs;
            while (sec >= SECS_PER_TRACK) {
                sec -= SECS_PER_TRACK;
                surf ^= 1;
                if (surf == 0) cyl++;
            }
            RLDA = (uint16_t)((cyl << 7) | (surf << 6) | sec);
            rl_cur_cyl  = (uint16_t)cyl;
            rl_cur_surf = (uint16_t)surf;
        }
        break;
    }
    }

    // Operation complete: set Control Ready (bit 7) AND Drive Ready
    // (bit 0). Real RL11 takes ms to ms-tens of ms for seek/read; we
    // short-circuit synchronously, so drive is "ready" the moment the
    // function returns. XXDP's primary boot polls for DRDY before each
    // step, so failing to restore bit 0 deadlocks it.
    RLCS |= 0x0081;
}

uint16_t read16(uint32_t a)
{
    switch (a) {
    case DEV_RL_CS:  return RLCS;
    case DEV_RL_BS:  return RLBA;
    case DEV_RL_DA:  return RLDA;
    case DEV_RL_MP:  return RLMP;
    case DEV_RL_BAE: return RLBAE;
    }
    return 0;
}

void write16(uint32_t a, uint16_t v)
{
    switch (a) {
    case DEV_RL_CS:
        trace_irq("CSR-write", v);
        // The M9312-style boot ROM in sam11/bootrom.h does NOT set the
        // explicit GO bit (bit 0); it writes the function code and then
        // polls control-ready. We follow that behavior: any RLCS write
        // launches the encoded function. We preserve the high error
        // bits unless execute() clears them.
        procNS::cancelinterrupt(INTRL);
        irq_pending = 0;
        RLCS = (RLCS & 0xFC00) | (v & 0x03FE);   // CS bits 13:0 writable
        RLCS &= ~0x0080;                          // busy
        execute();
        if (RLCS & (1 << 6)) {
            // Let the instruction following the CSR write execute before
            // raising the completion interrupt. This is long enough for a
            // guest WAIT to take effect, but short enough that an OS device
            // probe cannot observe CRDY and rewrite RLCS first, cancelling
            // an interrupt that real hardware would already have asserted.
            irq_pending = IRQ_DELAY_TICKS;
            trace_irq("scheduled", RLCS);
        }
        break;
    case DEV_RL_BS:  RLBA  = v; break;
    case DEV_RL_DA:  RLDA  = v; break;
    case DEV_RL_MP:  RLMP  = v; break;
    case DEV_RL_BAE: RLBAE = v; break;
    }
}

void tick()
{
    if (irq_pending && --irq_pending == 0 && (RLCS & (1 << 6))) {
        trace_irq("request", INTRL);
        procNS::interrupt(INTRL, 5);
    }
}

};  // namespace rl11
