// rh11.cpp - RH11 / RP04-RP06 secondary disk controller.
//
// Implements enough of the RH11/RP register set for an OS driver to probe
// RP0 and perform normal 512-byte sector read/write transfers. The controller
// is backed by disk slot DRIVE_RP0. Boot ROM support is deliberately separate.

#include "rh11.h"
#include "pdp1140.h"
#include "dd11.h"
#include "disk.h"

#include <Arduino.h>

// Xtensa's specreg.h defines PS as the processor-status special-register
// number. The PDP-11 core uses PS as its processor-status variable.
#ifdef PS
#undef PS
#endif

#include "appconfig.h"
#include "kd11.h"
#include "platform.h"

#define procNS kd11

namespace rh11 {

static constexpr uint32_t SECS_PER_TRACK = 22;
static constexpr uint32_t HEADS          = 19;
static constexpr uint32_t BYTES_PER_SEC  = 512;
static constexpr int      IRQ_DELAY_TICKS = 2;

static constexpr uint16_t CS1_GO  = 0000001;
static constexpr uint16_t CS1_IE  = 0000100;
static constexpr uint16_t CS1_RDY = 0000200;
static constexpr uint16_t CS1_TRE = 0100000;

static constexpr uint16_t CS2_NED = 0010000;

static constexpr uint16_t DS_VV   = 0000100;
static constexpr uint16_t DS_DRY  = 0000200;
static constexpr uint16_t DS_DPR  = 0000400;
static constexpr uint16_t DS_MOL  = 0010000;
static constexpr uint16_t DS_ERR  = 0040000;
static constexpr uint16_t DS_ATA  = 0100000;

static constexpr uint16_t ER1_ILF = 0000001;
static constexpr uint16_t ER1_AOE = 0000100;

static constexpr uint16_t F_NOP     = 000;
static constexpr uint16_t F_SEEK    = 004;
static constexpr uint16_t F_DCLR    = 010;
static constexpr uint16_t F_PRESET  = 020;
static constexpr uint16_t F_PACKACK = 022;
static constexpr uint16_t F_SEARCH  = 030;
static constexpr uint16_t F_WRITE   = 060;
static constexpr uint16_t F_READ    = 070;

struct Geometry {
    const char* name;
    uint16_t cylinders;
    uint16_t dtype;
};

static Geometry geom = { "rp06", 815, 020022 };

static uint16_t RHCS1 = CS1_RDY;
static uint16_t RHWC  = 0;
static uint16_t RHBA  = 0;
static uint16_t RHDA  = 0;
static uint16_t RHCS2 = 0;
static uint16_t RHDS  = 0;
static uint16_t RHER1 = 0;
static uint16_t RHAS  = 0;
static uint16_t RHDC  = 0;
static uint16_t RHOFF = 0;
static uint16_t RHDT  = 020022;
static uint16_t RHM1  = 0;
static uint16_t RHM2  = 0;
static uint16_t RHM3  = 0;
static uint16_t RHEC1 = 0;
static uint16_t RHEC2 = 0;
static uint16_t RHER2 = 0;
static uint16_t RHER3 = 0;
static uint16_t RHCS3 = 0;
static uint16_t RHBAE = 0;

static bool attached = false;
static int  irq_pending = 0;
static constexpr bool IRQ_TRACE = false;
static int irq_trace_left = 24;

static Geometry geometry_from_config()
{
    String t = cfg.disk_rp0_type;
    t.toLowerCase();
    if (t == "rp04") return { "rp04", 411, 020020 };
    if (t == "rp05") return { "rp05", 411, 020021 };
    return { "rp06", 815, 020022 };
}

static void trace_irq(const char* event, uint16_t value)
{
    if (IRQ_TRACE && irq_trace_left > 0) {
        LOG("RH11 IRQ %s val=%06o CS1=%06o PC=%06o PS=%06o pending=%d",
            event, (unsigned)value, (unsigned)RHCS1,
            (unsigned)kd11::curPC, (unsigned)kd11::PS, irq_pending);
        irq_trace_left--;
    }
}

static void schedule_done_irq()
{
    if (RHCS1 & CS1_IE) {
        irq_pending = IRQ_DELAY_TICKS;
        trace_irq("scheduled", RHCS1);
    }
}

static int selected_unit()
{
    return RHCS2 & 07;
}

static void update_drive_status()
{
    RHCS1 |= CS1_RDY;

    // This implementation exposes one RP-family drive. Units 1-7 do not
    // exist and must not inherit RP0's type or online status.
    if (selected_unit() != 0) {
        RHCS2 |= CS2_NED;
        RHDS = 0;
        return;
    }

    RHCS2 &= ~CS2_NED;
    RHDS = DS_DPR;
    if (attached) {
        RHDS |= DS_VV | DS_DRY | DS_MOL;
        if (disk_is_readonly(DRIVE_RP0)) RHDS |= 0004000;
    }
    if (RHER1 || (RHCS1 & CS1_TRE)) RHDS |= DS_ERR;
}

static void clear_errors()
{
    RHER1 = RHER2 = RHER3 = 0;
    RHCS1 &= ~CS1_TRE;
    RHCS2 &= ~CS2_NED;
    update_drive_status();
}

void reset()
{
    geom = geometry_from_config();
    attached = disk_is_mounted(DRIVE_RP0);
    RHCS1 = CS1_RDY;
    RHWC = RHBA = RHDA = RHCS2 = 0;
    RHER1 = RHAS = RHDC = RHOFF = 0;
    RHM1 = RHM2 = RHM3 = RHEC1 = RHEC2 = RHER2 = RHER3 = RHCS3 = RHBAE = 0;
    RHDT = geom.dtype;
    irq_pending = 0;
    irq_trace_left = 24;
    procNS::cancelinterrupt(INTRP);
    update_drive_status();
    if (attached) {
        LOG("RH11 RP0 attached as %s (%u bytes)",
            geom.name, (unsigned)disk_size_bytes(DRIVE_RP0));
    }
}

void media_changed(bool mounted)
{
    attached = mounted;
    update_drive_status();
}

static uint32_t bus_addr()
{
    return ((uint32_t)RHBA) | (((uint32_t)RHBAE & 03u) << 16);
}

static void set_bus_addr(uint32_t ba)
{
    RHBA = ba & 0xFFFF;
    RHBAE = (RHBAE & ~03u) | ((ba >> 16) & 03u);
}

static uint32_t disk_offset()
{
    uint32_t cyl  = RHDC & 01777;
    uint32_t head = (RHDA >> 8) & 037;
    uint32_t sec  = RHDA & 077;
    return ((cyl * HEADS + head) * SECS_PER_TRACK + sec) * BYTES_PER_SEC;
}

static bool validate_address()
{
    uint32_t cyl  = RHDC & 01777;
    uint32_t head = (RHDA >> 8) & 037;
    uint32_t sec  = RHDA & 077;
    if (selected_unit() != 0) {
        RHCS2 |= CS2_NED;
        RHCS1 |= CS1_TRE;
        update_drive_status();
        return false;
    }
    if (!attached || cyl >= geom.cylinders || head >= HEADS || sec >= SECS_PER_TRACK) {
        RHER1 |= ER1_AOE;
        RHCS1 |= CS1_TRE;
        update_drive_status();
        return false;
    }
    return true;
}

static void advance_da(uint32_t sectors)
{
    uint32_t cyl  = RHDC & 01777;
    uint32_t head = (RHDA >> 8) & 037;
    uint32_t sec  = RHDA & 077;
    sec += sectors;
    while (sec >= SECS_PER_TRACK) {
        sec -= SECS_PER_TRACK;
        head++;
        if (head >= HEADS) {
            head = 0;
            cyl++;
        }
    }
    RHDC = cyl & 01777;
    RHDA = (uint16_t)((head << 8) | sec);
}

static void transfer(bool writing)
{
    if (!validate_address()) return;

    uint32_t words_left = (uint32_t)((~RHWC + 1) & 0xFFFF);
    if (words_left == 0) return;

    uint32_t off = disk_offset();
    uint32_t ba = bus_addr();
    uint32_t total_words = words_left;
    uint8_t scratch[512];

    while (words_left) {
        uint32_t chunk_words = words_left > 256 ? 256 : words_left;
        uint32_t chunk_bytes = chunk_words * 2;

        if (writing) {
            for (uint32_t i = 0; i < chunk_words; i++) {
                uint16_t v = dd11::read16((ba + i * 2) & 0777777u);
                scratch[i * 2]     = (uint8_t)(v & 0xFF);
                scratch[i * 2 + 1] = (uint8_t)(v >> 8);
            }
            if (disk_write(DRIVE_RP0, off, scratch, chunk_bytes) < 0) {
                RHER1 |= ER1_AOE;
                RHCS1 |= CS1_TRE;
                break;
            }
        } else {
            if (disk_read(DRIVE_RP0, off, scratch, chunk_bytes) < 0) {
                RHER1 |= ER1_AOE;
                RHCS1 |= CS1_TRE;
                break;
            }
            for (uint32_t i = 0; i < chunk_words; i++) {
                uint16_t v = (uint16_t)scratch[i * 2]
                           | ((uint16_t)scratch[i * 2 + 1] << 8);
                dd11::write16((ba + i * 2) & 0777777u, v);
            }
        }

        ba = (ba + chunk_bytes) & 0777777u;
        off += chunk_bytes;
        words_left -= chunk_words;
        RHWC = (uint16_t)(RHWC + chunk_words);
    }

    set_bus_addr(ba);
    advance_da((total_words - words_left + 255) / 256);
}

static const char* func_name(uint16_t f)
{
    switch (f) {
    case F_NOP: return "NOP";
    case F_SEEK: return "SEEK";
    case F_DCLR: return "DCLR";
    case F_PRESET: return "PRESET";
    case F_PACKACK: return "PACKACK";
    case F_SEARCH: return "SEARCH";
    case F_WRITE: return "WRITE";
    case F_READ: return "READ";
    }
    return "?";
}

static void execute()
{
    uint16_t func = RHCS1 & 0000076;
    RHCS1 &= ~CS1_RDY;

    if (selected_unit() != 0) {
        RHCS2 |= CS2_NED;
        RHCS1 |= CS1_TRE;
    } else {
        switch (func) {
        case F_NOP:
            break;
        case F_DCLR:
        case F_PRESET:
        case F_PACKACK:
            clear_errors();
            break;
        case F_SEEK:
        case F_SEARCH:
            validate_address();
            break;
        case F_WRITE:
            transfer(true);
            break;
        case F_READ:
            transfer(false);
            break;
        default:
            LOG("RH11 unsupported function %06o (%s)", (unsigned)func, func_name(func));
            RHER1 |= ER1_ILF;
            RHCS1 |= CS1_TRE;
            break;
        }
    }

    update_drive_status();
    if (selected_unit() == 0) RHAS |= 1;
    schedule_done_irq();
}

void tick()
{
    if (irq_pending > 0 && --irq_pending == 0) {
        if (RHCS1 & CS1_IE) {
            trace_irq("request", INTRP);
            procNS::interrupt(INTRP, 5);
        }
    }
}

uint16_t read16(uint32_t a)
{
    switch (a) {
    case DEV_RH_CS1: return RHCS1;
    case DEV_RH_WC:  return RHWC;
    case DEV_RH_BA:  return RHBA;
    case DEV_RH_DA:  return RHDA;
    case DEV_RH_CS2: return RHCS2;
    case DEV_RH_DS:
        update_drive_status();
        return RHDS;
    case DEV_RH_ER1: return RHER1;
    case DEV_RH_AS:  return RHAS;
    case DEV_RH_LA:  return 0;
    case DEV_RH_DB:  return 0;
    case DEV_RH_MR:  return RHM1;
    case DEV_RH_DT:  return selected_unit() == 0 ? RHDT : 0;
    case DEV_RH_SN:  return selected_unit() == 0 ? 1 : 0;
    case DEV_RH_OF:  return RHOFF;
    case DEV_RH_DC:  return RHDC;
    case DEV_RH_CC:  return RHDC;
    case DEV_RH_ER2: return RHER2;
    case DEV_RH_ER3: return RHER3;
    case DEV_RH_EC1: return RHEC1;
    case DEV_RH_EC2: return RHEC2;
    case DEV_RH_BAE: return RHBAE;
    case DEV_RH_CS3: return RHCS3;
    }
    return 0;
}

void write16(uint32_t a, uint16_t v)
{
    switch (a) {
    case DEV_RH_CS1:
        procNS::cancelinterrupt(INTRP);
        irq_pending = 0;
        RHCS1 = (RHCS1 & (CS1_RDY | CS1_TRE)) | (v & ~(CS1_RDY | CS1_TRE));
        if (v & CS1_GO) execute();
        else {
            update_drive_status();
            schedule_done_irq();
        }
        break;
    case DEV_RH_WC:  RHWC = v; break;
    case DEV_RH_BA:  RHBA = v; break;
    case DEV_RH_DA:  RHDA = v; break;
    case DEV_RH_CS2:
        if (v & 040) {
            reset(); // controller clear
            break;
        }
        RHCS2 = v & 017;
        update_drive_status();
        break;
    case DEV_RH_AS:  RHAS &= ~v; break; // write-one-to-clear attention bits
    case DEV_RH_MR:  RHM1 = v; break;
    case DEV_RH_OF:  RHOFF = v; break;
    case DEV_RH_DC:  RHDC = v & 01777; break;
    case DEV_RH_EC1: RHEC1 = v; break;
    case DEV_RH_EC2: RHEC2 = v; break;
    case DEV_RH_BAE: RHBAE = v & 077; break;
    case DEV_RH_CS3: RHCS3 = v; break;
    default:
        break;
    }
}

}
