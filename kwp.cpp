// DEC KW11-P programmable real-time clock.
//
// Register layout at 0o772540:
//   CSR  control/status (read clears DONE, ERR, and the pending interrupt)
//   CSB  write-only count-set buffer
//   CNT  read-only live 16-bit counter
//
// CSR bits:
//   15 ERR, 7 DONE, 6 IE, 5 FIX, 4 UP/DOWN, 3 REPEAT,
//   2-1 RATE (100 kHz, 10 kHz, line, external), 0 RUN.

#include "kwp.h"

#include <Arduino.h>

// Xtensa's specreg.h defines PS as the processor-status special-register
// number. The PDP-11 core uses PS as its processor-status variable.
#ifdef PS
#undef PS
#endif

#include "platform.h"
#include "kw11.h"
#include "kb11.h"
#include "kd11.h"
#include "sam11_platform.h"

#if USE_11_45 && !STRICT_11_40
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kwp {

static constexpr uint16_t CSR_ERR    = 0100000;
static constexpr uint16_t CSR_DONE   = 0000200;
static constexpr uint16_t CSR_IE     = 0000100;
static constexpr uint16_t CSR_FIX    = 0000040;
static constexpr uint16_t CSR_UPDN   = 0000020;
static constexpr uint16_t CSR_MODE   = 0000010;
static constexpr uint16_t CSR_RATE   = 0000006;
static constexpr uint16_t CSR_GO     = 0000001;
static constexpr uint16_t CSR_RDMASK = 0100377;
static constexpr uint16_t CSR_WRMASK = 0000137;

// SIMH models the otherwise-unavailable external trigger as 10 Hz.
static constexpr uint32_t RATE_US[4] = {
    10,       // 100 kHz crystal
    100,      // 10 kHz crystal
    16667,    // 60 Hz line frequency
    100000,   // external input, modeled as 10 Hz
};

uint16_t CSR  = 0;
uint16_t CSB  = 0;
uint16_t CNTR = 0;
bool enabled = false;

static uint32_t s_last_us = 0;
static uint32_t s_remainder_us = 0;
static uint8_t s_poll_divider = 0;
static bool s_irq_pending = false;

static uint8_t rate_index(uint16_t csr)
{
    return (uint8_t)((csr & CSR_RATE) >> 1);
}

static const char *register_name(uint32_t a)
{
    switch (a) {
        case DEV_KWP_CSR:  return "CSR";
        case DEV_KWP_CSB:  return "CSB";
        case DEV_KWP_CNTR: return "CNT";
        case DEV_KWP:      return "UNU";
        default:           return "EXT";
    }
}

static void log_access(const char *operation, uint32_t a, uint16_t value)
{
    char detail[24];
    snprintf(detail, sizeof(detail), "%s %s", operation, register_name(a));
    kw11::trace_access("KW11-P", detail, a, value);
}

static void clear_interrupt()
{
    if (s_irq_pending) {
        procNS::cancelinterrupt(INTRTC);
        s_irq_pending = false;
    }
}

static void request_interrupt()
{
    if ((CSR & CSR_IE) && !s_irq_pending) {
        kw11::trace_interrupt("KW11-P", "request", INTRTC, 6);
        procNS::interrupt(INTRTC, 6);
        s_irq_pending = true;
    }
}

static uint32_t steps_to_expiry(uint16_t value)
{
    uint32_t steps = (CSR & CSR_UPDN)
        ? (0x10000u - (uint32_t)value)
        : (uint32_t)value;
    return steps ? steps : 0x10000u;
}

static void advance_without_expiry(uint32_t steps)
{
    if (CSR & CSR_UPDN)
        CNTR = (uint16_t)(CNTR + steps);
    else
        CNTR = (uint16_t)(CNTR - steps);
}

static void record_expirations(uint64_t count)
{
    if (count == 0) return;
    if ((CSR & CSR_DONE) || count > 1)
        CSR |= CSR_ERR;
    CSR |= CSR_DONE;
    request_interrupt();
}

static void advance_steps(uint64_t steps)
{
    if (!(CSR & CSR_GO) || steps == 0) return;

    uint32_t first = steps_to_expiry(CNTR);
    if (steps < first) {
        advance_without_expiry((uint32_t)steps);
        return;
    }

    steps -= first;
    CNTR = 0;
    uint64_t expirations = 1;

    if (!(CSR & CSR_MODE)) {
        record_expirations(expirations);
        CSB = 0;
        CSR &= (uint16_t)~CSR_GO;
        s_remainder_us = 0;
        return;
    }

    CNTR = CSB;
    uint32_t cycle = steps_to_expiry(CNTR);
    if (steps >= cycle) {
        expirations += steps / cycle;
        steps %= cycle;
    }
    if (steps)
        advance_without_expiry((uint32_t)steps);

    record_expirations(expirations);
}

static void update_clock()
{
    uint32_t now = micros();
    uint32_t elapsed = now - s_last_us;
    s_last_us = now;

    if (!(CSR & CSR_GO)) {
        s_remainder_us = 0;
        return;
    }

    uint32_t period = RATE_US[rate_index(CSR)];
    uint64_t accumulated = (uint64_t)s_remainder_us + elapsed;
    uint64_t steps = accumulated / period;
    s_remainder_us = (uint32_t)(accumulated % period);
    advance_steps(steps);
}

static void manual_tick()
{
    if (CSR & CSR_GO) return;

    if (CSR & CSR_UPDN)
        CNTR = (uint16_t)(CNTR + 1);
    else
        CNTR = (uint16_t)(CNTR - 1);

    if (CNTR == 0) {
        record_expirations(1);
        if (!(CSR & CSR_MODE))
            CSB = 0;
        else
            CNTR = CSB;
    }
}

void reset()
{
    clear_interrupt();
    CSR = 0;
    CSB = 0;
    CNTR = 0;
    s_remainder_us = 0;
    s_poll_divider = 0;
    s_last_us = micros();
}

uint16_t read16(uint32_t a)
{
    if (!enabled) {
        log_access("READ ", a, 0);
        return 0;
    }

    update_clock();

    uint16_t value = 0;
    switch (a) {
        case DEV_KWP_CSR: {
            value = CSR & CSR_RDMASK;
            CSR &= (uint16_t)~(CSR_ERR | CSR_DONE);
            clear_interrupt();
            break;
        }
        case DEV_KWP_CSB:
            value = 0;              // count-set buffer is write-only
            break;
        case DEV_KWP_CNTR:
            value = CNTR;
            break;
        default:
            value = 0;
            break;
    }

    log_access("READ ", a, value);
    return value;
}

void write16(uint32_t a, uint16_t v)
{
    if (!enabled) {
        log_access("WRITE", a, v);
        return;
    }

    update_clock();

    switch (a) {
        case DEV_KWP_CSR: {
            uint16_t old = CSR;
            uint8_t old_rate = rate_index(old);
            bool was_running = (old & CSR_GO) != 0;

            clear_interrupt();
            CSR = v & CSR_WRMASK;

            if (!(CSR & CSR_GO)) {
                s_remainder_us = 0;
                if (v & CSR_FIX)
                    manual_tick();
            } else if (!was_running || old_rate != rate_index(CSR)) {
                CNTR = CSB;
                s_remainder_us = 0;
            }
            s_last_us = micros();
            break;
        }

        case DEV_KWP_CSB:
            CSB = v;
            CNTR = v;
            CSR &= (uint16_t)~(CSR_ERR | CSR_DONE);
            clear_interrupt();
            s_remainder_us = 0;
            s_last_us = micros();
            break;

        case DEV_KWP_CNTR:
            break;                  // live counter is read-only

        default:
            break;
    }

    log_access("WRITE", a, v);
}

void tick()
{
    // Register accesses always synchronize immediately. During normal CPU
    // execution, checking the host timer every 64 instructions avoids making
    // micros() a significant part of the emulator's instruction budget.
    if (enabled && ++s_poll_divider >= 64) {
        s_poll_divider = 0;
        update_clock();
    }
}

};  // namespace kwp
