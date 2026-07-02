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

#if !H_CPU_IRQ
#define H_CPU_IRQ 1

void trapat(uint16_t vec)
{
    if (vec & 1)
    {
        if (PRINTSIMLINES)
        {
            Serial.println(F("%% Thou darst calling trapat() with an odd vector number?"));
        }
        panic();
    }
    trapped = true;
    cont_with = false;
    if (PRINTSIMLINES)
    {
        Serial.print(F("%% trap: "));
        Serial.println(vec, OCT);

        if (DEBUG_TRAP)
        {
            printstate();
        }
    }
    /*var prev uint16
       defer func() {
           t = recover()
           switch t = t.(type) {
           case trap:
               writedebug("red stack trap!\n")
               memory[0] = uint16(k.R[7])
               memory[1] = prev
               vec = 4
               panic("fatal")
           case nil:
               break
           default:
               panic(t)
           }
   */
    uint16_t prev = PS;
    switchmode(0);
    push(prev);
    push(R[7]);

    R[7] = dd11::read16(vec);
    PS = dd11::read16(vec + 2);
    PS |= (curuser << 14);
    PS |= (prevuser << 12);
    waiting = false;
}

void interrupt(uint8_t vec, uint8_t pri)
{
    if (vec & 1)
    {
        if (PRINTSIMLINES)
        {
            Serial.println(F("%% Thou darst calling interrupt() with an odd vector number?"));
        }
        panic();
    }
    uint8_t count = 0;
    while (count < ITABN && itab[count].vec != 0) {
        // A level-sensitive device has only one pending request per vector.
        if (itab[count].vec == vec)
            return;
        count++;
    }

    if (count >= ITABN)
    {
        if (PRINTSIMLINES)
        {
            _printf("%%%% interrupt table full (%i of %i)", count, ITABN);
        }
        panic();
        return;
    }

    // Highest BR level wins. At the same level, the lower vector has
    // higher UNIBUS arbitration priority.
    uint8_t pos = 0;
    while (pos < count &&
           (itab[pos].pri > pri ||
            (itab[pos].pri == pri && itab[pos].vec < vec))) {
        pos++;
    }

    // Shift from the tail toward the insertion point. The old forward
    // shift repeatedly copied one entry and corrupted every queued IRQ
    // after it.
    for (uint8_t j = count; j > pos; j--)
        itab[j] = itab[j - 1];

    itab[pos].vec = vec;
    itab[pos].pri = pri;
}

void cancelinterrupt(uint8_t vec)
{
    uint8_t out = 0;
    for (uint8_t in = 0; in < ITABN; in++) {
        if (itab[in].vec != 0 && itab[in].vec != vec)
            itab[out++] = itab[in];
    }
    while (out < ITABN) {
        itab[out].vec = 0;
        itab[out].pri = 0;
        out++;
    }
}

void handleinterrupt()
{
    uint8_t vec = itab[0].vec;
    if (vec == INTCLOCK)
        kw11::trace_interrupt("KW11-L", "deliver", vec, itab[0].pri);
    else if (vec == INTRTC)
        kw11::trace_interrupt("KW11-P", "deliver", vec, itab[0].pri);
    if (DEBUG_INTER)
    {
        if (PRINTSIMLINES)
        {
            Serial.print("%% IRQ: ");
            Serial.println(vec, OCT);
        }
    }

    // The UNIBUS interrupt grant acknowledges this device before the CPU
    // begins stacking state and fetching the vector. A device can have only
    // one outstanding request for a vector, so discard every stale duplicate
    // rather than merely shifting one entry. This also prevents a corrupted
    // or previously duplicated request from starving lower-priority devices.
    // Do not install a nested setjmp in the shared global trapbuf: after this
    // function returned, trapbuf pointed into a dead stack frame.
    cancelinterrupt(vec);

    uint16_t prev = PS;
    switchmode(0);
    push(prev);
    push(R[7]);

    R[7] = dd11::read16(vec);
    PS = dd11::read16(vec + 2);
    PS |= (curuser << 14);
    PS |= (prevuser << 12);
    waiting = false;
}

#endif
