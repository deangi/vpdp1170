/*
Modified BSD License

Copyright (c) 2021 Chloe Lunn

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// sam11 software emulation of DEC PDP-11/40 KL11 Main TTY

#include "kl11.h"

#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "sam11.h"
#include "termopts.h"
#include "platform.h"
#include "fifo.h"

#include <Arduino.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// vpdp1140 m2: KL11 is routed through the v8088-inherited host scaffolding
// so guest TTY output appears on the TFT 80x25 grid AND any Telnet client
// AND USB-Serial, and guest TTY input is fed from any of those sources.
// m11 (2026-05-28): each host channel sits behind an 8 KB FIFO so a
// bursty guest (RT-11 DIR, V6 ls -l, type) can hand off output without
// being throttled by the slowest sink, and the host RX path can absorb
// fast-typed input without backpressuring the producers.
#include "console.h"
#include "telnet.h"
#include "emu_control.h"

#if USE_11_45
#define procNS kb11
#else
#define procNS kd11
#endif

namespace kl11 {

uint16_t TKS;
uint16_t TKB;
uint16_t TPS;
uint16_t TPB;

static uint32_t console_trace_count = 0;

void set_console_trace(uint32_t count)
{
    console_trace_count = count;
}

uint32_t console_trace_remaining()
{
    return console_trace_count;
}

static void trace_console_char(const char* direction, uint8_t value)
{
    if (console_trace_count == 0) return;
    console_trace_count--;

    char display[8];
    if (value >= 0x20 && value <= 0x7e) {
        display[0] = '\'';
        display[1] = (char)value;
        display[2] = '\'';
        display[3] = '\0';
    } else {
        snprintf(display, sizeof(display), "^%c",
                 value < 0x20 ? (char)(value + '@') : '?');
    }

    LOG("CONSOLE %s char=%03o %s PC=%06o remaining=%u",
        direction, (unsigned)value, display, (unsigned)procNS::curPC,
        (unsigned)console_trace_count);
}

// 8 KB KL11->USB-Serial FIFO. The TFT and Telnet sinks own their own
// FIFOs inside console.cpp / telnet.cpp; this one stays here because
// there's no "serial" module to put it in. Storage lives in PSRAM.
#define VPDP_KL11_FIFO_BYTES 8192
EXT_RAM_BSS_ATTR static uint8_t serial_out_storage[VPDP_KL11_FIFO_BYTES];
static Fifo g_serial_out;
static bool g_serial_out_inited = false;

#define VPDP_CONTROL_REPLY_BYTES 1024
static uint8_t control_reply_storage[VPDP_CONTROL_REPLY_BYTES];
static Fifo g_control_reply;
static bool g_control_reply_inited = false;

static const uint8_t CONTROL_PREFIX[] = { 033, ']', 'V', 'P', 'D', 'P', ';' };
static constexpr size_t CONTROL_PREFIX_LEN = sizeof(CONTROL_PREFIX);
// RSTS/E BASIC-PLUS has been observed emitting '$' for CHR$(27) on console
// output. Accept "$]VPDP;" as a compatibility spelling so a BASIC program can
// still reach the private channel. ESC remains the canonical prefix.
static constexpr uint8_t CONTROL_PREFIX_BASIC = '$';
static constexpr size_t CONTROL_COMMAND_MAX = 256;
enum ControlParseState {
    CONTROL_IDLE,
    CONTROL_PREFIX_MATCH,
    CONTROL_TEXT
};
static ControlParseState control_state = CONTROL_IDLE;
static uint8_t control_prefix_pos = 0;
static uint8_t control_prefix_first = 0;
static char control_command[CONTROL_COMMAND_MAX + 1];
static size_t control_command_len = 0;

// Inter-character delay (ms) between successive TKB loads (set from
// [diag] serialdelay in pdpconfig.ini). After each addchar we record the
// host's millis(); the next byte can't enter TKB until at least this
// many ms have elapsed. Closes the burst-induced klrint re-entry window
// at the KL11 layer in an OS-agnostic way (no PSW priority inspection).
// Default 0 = no delay; recommended 10-50 ms for interactive guests.
uint32_t serial_in_delay_ms = 0;
static uint32_t s_last_addchar_ms = 0;
static bool     s_fifo_drained    = true;   // true at boot -> first char is immediate

// Input receive polling is host-side work, not guest-visible UART timing:
// it scans sam11's interrupt table, calls millis(), and probes host FIFOs.
// Doing that every PDP-11 instruction is expensive. Check every N KL11
// polls instead; output timing below still runs every poll.
#define KL11_RX_POLL_DIV 100
static uint8_t  s_rx_poll_div = KL11_RX_POLL_DIV - 1;

static void update_rx_interrupt()
{
    if ((TKS & 0x80) && (TKS & (1 << 6)))
        procNS::interrupt(INTTTYIN, 4);
    else
        procNS::cancelinterrupt(INTTTYIN);
}

static void update_tx_interrupt()
{
    if ((TPS & 0x80) && (TPS & (1 << 6)))
        procNS::interrupt(INTTTYOUT, 4);
    else
        procNS::cancelinterrupt(INTTTYOUT);
}

void reset()
{
    TKS = 0;
    TPS = 1 << 7;
    TKB = 0;
    TPB = 0;
    if (!g_serial_out_inited) {
        g_serial_out.init(serial_out_storage, VPDP_KL11_FIFO_BYTES);
        g_serial_out_inited = true;
    }
    if (!g_control_reply_inited) {
        g_control_reply.init(control_reply_storage, VPDP_CONTROL_REPLY_BYTES);
        g_control_reply_inited = true;
    }
    control_state = CONTROL_IDLE;
    control_prefix_pos = 0;
    control_prefix_first = 0;
    control_command_len = 0;
    s_rx_poll_div = KL11_RX_POLL_DIV - 1;
}

void drain_serial_out()
{
    // Same chunked drain as telnet_poll: contiguous runs from the ring,
    // up to whatever Serial.write() will take in one call. Serial.write
    // on USB-CDC is generally non-blocking up to the host driver's buffer.
    if (g_serial_silenced) { g_serial_out.clear(); return; }
    const uint8_t* p;
    size_t n;
    while ((n = g_serial_out.peek(&p)) > 0) {
        size_t w = Serial.write(p, n);
        if (w == 0) break;
        g_serial_out.consume(w);
        if (w < n) break;
    }
}

void queue_serial_out(uint8_t out)
{
    if (g_serial_silenced) return;
    if (!g_serial_out_inited) {
        g_serial_out.init(serial_out_storage, VPDP_KL11_FIFO_BYTES);
        g_serial_out_inited = true;
    }
    g_serial_out.push(out);
}

static void addchar(char c)
{
#if CR_TO_LF && !LF_TO_CR
    if (c == _CR)
        c = _LF;
#endif

#if !CR_TO_LF && LF_TO_CR
    if (c == _LF)
        c = _CR;
#endif

#if BS_TO_DEL && !DEL_TO_BS
    // If enabled, change backspaces into deletes
    if (c == _BS)
        c = _DEL;
#endif

#if !BS_TO_DEL && DEL_TO_BS
    // If enabled, change deletes into backspaces
    if (c == _DEL)
        c = _BS;
#endif

#if REMAP_WITH_TABLE
    // If enabled, use the keymap array to change the character
    TKB = ascii_chart[c & 0x7F] & 0x7F;
#else
    // If not then just pass the character forward as is
    TKB = c & 0x7F;
#endif

    TKS |= 0x80;
    update_rx_interrupt();
}

bool queue_control_reply(const char* payload)
{
    if (!payload) return false;
    if (!g_control_reply_inited) {
        g_control_reply.init(control_reply_storage, VPDP_CONTROL_REPLY_BYTES);
        g_control_reply_inited = true;
    }
    size_t payload_len = strlen(payload);
    size_t required = CONTROL_PREFIX_LEN + payload_len + 1;
    if (required > g_control_reply.capacity() - g_control_reply.count())
        return false;
    for (size_t i = 0; i < CONTROL_PREFIX_LEN; i++)
        g_control_reply.push(CONTROL_PREFIX[i]);
    for (size_t i = 0; i < payload_len; i++)
        g_control_reply.push((uint8_t)payload[i]);
    g_control_reply.push(003);
    return true;
}

bool queue_input_bytes(const uint8_t* data, size_t bytes)
{
    if (!data || bytes == 0) return true;
    if (!g_control_reply_inited) {
        g_control_reply.init(control_reply_storage, VPDP_CONTROL_REPLY_BYTES);
        g_control_reply_inited = true;
    }
    if (bytes > g_control_reply.capacity() - g_control_reply.count())
        return false;
    for (size_t i = 0; i < bytes; i++)
        g_control_reply.push(data[i] & 0x7f);
    return true;
}

static void emit_console_byte(uint8_t out)
{
    console_feed(out);
    telnet_write(out);
    g_serial_out.push(out);
}

static void reset_control_parser()
{
    control_state = CONTROL_IDLE;
    control_prefix_pos = 0;
    control_prefix_first = 0;
    control_command_len = 0;
}

static void process_console_output(uint8_t out)
{
    if (control_state == CONTROL_IDLE) {
        if (out == CONTROL_PREFIX[0] || out == CONTROL_PREFIX_BASIC) {
            control_state = CONTROL_PREFIX_MATCH;
            control_prefix_pos = 1;
            control_prefix_first = out;
        } else {
            emit_console_byte(out);
        }
        return;
    }

    if (control_state == CONTROL_PREFIX_MATCH) {
        if (out == CONTROL_PREFIX[control_prefix_pos]) {
            if (++control_prefix_pos == CONTROL_PREFIX_LEN) {
                control_state = CONTROL_TEXT;
                control_command_len = 0;
                if (control_prefix_first == CONTROL_PREFIX_BASIC)
                    LOG("EMU control: accepted BASIC-PLUS $]VPDP compatibility prefix");
            }
            return;
        }

        // Not our private sequence. Replay the delayed candidate unchanged.
        emit_console_byte(control_prefix_first);
        for (uint8_t i = 1; i < control_prefix_pos; i++)
            emit_console_byte(CONTROL_PREFIX[i]);
        emit_console_byte(out);
        reset_control_parser();
        return;
    }

    if (out == 003 || out == 004) {
        control_command[control_command_len] = 0;
        if (!emu_control::submit(control_command))
            LOGE("EMU command queue full; command discarded");
        reset_control_parser();
        return;
    }

    // CR, LF, BEL, and TAB are permitted inside command text. This allows
    // OUTASCII to carry formatting characters and lets structured command
    // handlers remove terminal-inserted wrapping from path arguments.
    bool allowed_control =
        out == '\r' || out == '\n' || out == '\a' || out == '\t';

    // ETX and EOT are valid terminators. Any other non-printable character,
    // or a lost terminator that lets the command exceed 256 characters,
    // aborts the hidden frame and restores normal console parsing.
    if ((!allowed_control && (out < 0x20 || out > 0x7e)) ||
        control_command_len >= CONTROL_COMMAND_MAX) {
        LOGE("EMU command aborted: missing ETX/EOT or invalid byte %03o",
             (unsigned)out);
        reset_control_parser();
        return;
    }
    control_command[control_command_len++] = (char)out;
}

uint8_t count;

void poll()
{
    // Read: round-robin between the host's two input FIFOs (Serial via
    // console_key_pop, Telnet via telnet_in_pop). Alternation happens
    // only on a successful pop, so if just one source has data we drain
    // it without skipped polls. Cross-source order can't be preserved
    // (both clients type independently), but each source's own order is.
    //
    // Three-stage guard before loading TKB:
    //  (a) bit 7 (RDONE) clear -> the guest has already read the prior
    //      byte from TKB. Without this we'd overwrite TKB between reads.
    //  (b) no INTTTYIN (vec 060) still pending in sam11's itab -> the
    //      guest has actually taken the prior IRQ via handleinterrupt
    //      (which pops it from itab).
    //  (c) inter-character delay: at least serial_in_delay_ms ms have
    //      elapsed since the last addchar. Without (c), a fast host-side
    //      burst can otherwise load the next byte immediately after the
    //      prior IRQ is acknowledged. A small ms gap matches what a real
    //      serial line would have enforced via baud-rate timing and stays
    //      OS-agnostic (no PSW priority inspection). serial_in_delay_ms is
    //      set from [diag] serialdelay in pdpconfig.ini.
    //
    // Wraparound: millis() wraps every ~49.7 days. The unsigned
    // subtraction (now - s_last_addchar_ms) wraps correctly for any
    // delay < 2^31 ms. The corner case where the *previous* addchar
    // happened within `delay` ms of the wrap boundary is closed by
    // s_fifo_drained: once the host FIFO drains empty (no chars
    // pending), the next arriving char bypasses the delay entirely,
    // so an idle wraparound never matters.
    if (++s_rx_poll_div >= KL11_RX_POLL_DIV)
    {
        s_rx_poll_div = 0;

        bool inttytin_queued = false;
        for (uint8_t i = 0; i < ITABN; i++) {
            if (itab[i].vec == 0) break;
            if (itab[i].vec == INTTTYIN) { inttytin_queued = true; break; }
        }

        if (!(TKS & 0x80) && !inttytin_queued)
        {
            uint32_t now = millis();
            bool delay_ok = s_fifo_drained ||
                            (uint32_t)(now - s_last_addchar_ms) >= serial_in_delay_ms;
            if (delay_ok)
            {
                static bool prefer_telnet = false;
                uint8_t c;
                bool got;
                bool control_reply = g_control_reply.pop(&c);
                if (control_reply) {
                    got = true;
                } else if (prefer_telnet) {
                    got = telnet_in_pop(&c) || console_key_pop(&c);
                } else {
                    got = console_key_pop(&c) || telnet_in_pop(&c);
                }
                if (got)
                {
                    if (!control_reply) prefer_telnet = !prefer_telnet;
                    if (c == '\n' || c == '\r')
                    {
                        procNS::trapped |= VTRAP_ON_NL;
                    }
                    addchar(c & 0x7F);
                    s_last_addchar_ms = now;
                    s_fifo_drained    = false;
                }
                else
                {
                    // No host bytes pending. Mark idle so the next arriving
                    // char bypasses the delay (and the millis() wraparound
                    // corner case becomes a non-issue).
                    s_fifo_drained = true;
                }
            }
        }
    }

    // Write: when the guest puts a char in TPB, count up 32 polls (mimics
    // a baud-rate-limited UART) then fan the byte into all three host
    // FIFOs. Each sink owns its own 8 KB ring and drains independently,
    // so a slow USB host or a wedged telnet client can't stall the TFT
    // (or each other), and the KL11 itself never blocks.
    if ((TPS & 0x80) == 0)
    {
        if (++count > 32)
        {
            uint8_t out = TPB & 0x7f;  // strip parity bit
            process_console_output(out);
            TPS |= 0x80;
            update_tx_interrupt();
        }
    }
}

uint16_t read16(uint32_t a)
{
    switch (a)
    {
    case DEV_CONSOLE_TTY_IN_STATUS:
        return TKS;
    case DEV_CONSOLE_TTY_IN_DATA:
        if (TKS & 0x80)
        {
            // Clear only bit 7 (RX done); bit 0 is the reader-enable
            // flag which is set by software, not by reading TKB.
            TKS &= 0xff7f;
            update_rx_interrupt();
            trace_console_char("READ ", (uint8_t)TKB);
            return TKB;
        }
        return 0;
    case DEV_CONSOLE_TTY_OUT_STATUS:
        return TPS;
    case DEV_CONSOLE_TTY_OUT_DATA:
        return 0;
    default:
        if (PRINTSIMLINES)
        {
            if (!g_serial_silenced) Serial.println(F("%% kl11: read16 from invalid address"));  // " + ostr(a, 6))
            //panic();
        }
        return 0;
    }
}

void write16(uint32_t a, uint16_t v)
{
    switch (a)
    {
    case DEV_CONSOLE_TTY_IN_STATUS:
        // Real DL11: IE gates the request line. If software enables IE
        // while DONE is already set, request immediately; if it clears IE
        // while a request is queued, remove the stale pending vector.
        if (v & (1 << 6))
        {
            TKS |= 1 << 6;
        }
        else
        {
            TKS &= ~(1 << 6);
        }
        update_rx_interrupt();
        break;
    case DEV_CONSOLE_TTY_OUT_STATUS:
        // Real DL11: IE gates the request line. If software enables IE
        // while READY is already set, request immediately; if it clears IE
        // while a request is queued, remove the stale pending vector.
        if (v & (1 << 6))
        {
            TPS |= 1 << 6;
        }
        else
        {
            TPS &= ~(1 << 6);
        }
        update_tx_interrupt();
        break;
    case DEV_CONSOLE_TTY_OUT_DATA:
        TPB = v & 0xff;
        trace_console_char("WRITE", (uint8_t)TPB);
        TPS &= 0xff7f;
        update_tx_interrupt();
        count = 0;
        break;
    case DEV_CONSOLE_TTY_IN_DATA:
        break;
    default:
        if (PRINTSIMLINES)
        {
            if (!g_serial_silenced) Serial.println(F("%% kl11: write16 to invalid address"));  // " + ostr(a, 6))
            //panic();
        }
    }
}

};  // namespace kl11
