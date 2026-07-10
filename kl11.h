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
#include <stddef.h>
#include "pdp1140.h"

namespace kl11 {

enum
{
    BAUD_100 = 100,    // REV E
    BAUD_110 = 110,    // REV A
    BAUD_150 = 150,    // REV B
    BAUD_300 = 300,    // REV C
    BAUD_600 = 600,    // REV D
    BAUD_1200 = 1200,  // REV E
    BAUD_2400 = 2400,  // REV F
    BAUD_DEFAULT = BAUD_2400,
};

void write16(uint32_t a, uint16_t v);
uint16_t read16(uint32_t a);
void reset();
void poll();

// Minimum host-side ms between successive TKB loads. 0 disables the gate
// (back to instruction-rate delivery, fine for slow human typing but not
// for line-buffered terminal bursts). Set from [diag] serialdelay.
extern uint32_t serial_in_delay_ms;

void set_console_trace(uint32_t count);
uint32_t console_trace_remaining();

// Drain queued KL11->USB-Serial bytes (called from loop() on core 1).
// The KL11 push path is non-blocking; this turns the 8 KB FIFO into
// actual Serial.write() output at whatever pace the host can take.
void drain_serial_out();

// Non-blocking enqueue of a guest console output byte (kek TTY path).
void queue_serial_out(uint8_t out);

// Queue a framed emulator-control response into the KL11 receive stream.
// The payload is wrapped as ESC ] VPDP ; payload ETX and is delivered before
// interactive USB/Telnet input so response frames cannot be interleaved.
bool queue_control_reply(const char* payload);

// Queue unframed bytes into the TTY0 KL11 receive stream. Used by direct
// INASCII/INHEX commands, which return one CR/LF-terminated data line.
bool queue_input_bytes(const uint8_t* data, size_t bytes);

};  // namespace kl11
