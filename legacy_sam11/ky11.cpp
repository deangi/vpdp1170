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

// sam11 software emulation of DEC PDP-11/40 KY11 Front Panel

#include "ky11.h"

#include "dd11.h"
#include "kb11.h"  // 11/45
#include "kd11.h"  // 11/40
#include "sam11_platform.h"
#include "sam11.h"

namespace ky11 {

uint32_t SR;  // data switches (not address or option switches!)
uint16_t DR;  // display register (separate to address/data displays)

uint16_t SLR;  // Register of status LEDs separate to addr/data/display
uint16_t CSR;  // Resgister of control switches separate to addr/data switches

uint16_t prevCSR;
uint32_t workingADR;
uint16_t workingDTR;

bool showDR = false;

void step()
{
    SR = platform::readSwitches();
    CSR = platform::readControlSwitches();

#if KY_PANEL
    if (CSR & sw_load && !(prevCSR & sw_load))
    {
        // Load the address from switches, load the data from the bus
        workingADR = SR;
        workingDTR = dd11::read16(workingADR);

        // write to front panel
        platform::writeAddr(workingADR);
        platform::writeData(workingDTR);
        prevCSR = CSR;
        return;
    }

    if (CSR & sw_load && !(prevCSR & sw_load))
    {
        workingADR = SR;
        prevCSR = CSR;
        platform::writeAddr(workingADR);
        return;
    }

    // If the deposit switch came on, push the working data into the bus
    if (CSR & sw_deposit && !(prevCSR & sw_deposit))
    {
        // simply write working data to the bus
        dd11::write16(workingADR, workingDTR);
        prevCSR = CSR;
        return;
    }
#endif
}

void reset()
{
    SR = 0000000;  // INST_UNIX_SINGLEUSER;
    DR = 0000000;

    SR = platform::readSwitches();
    CSR = platform::readControlSwitches();
}

uint16_t read16(uint32_t addr)
{
    // read front panel switches here
    // SR = platform::readSwitches();

    if (addr == DEV_CONSOLE_SR)
        return SR;  // SR;
    return 0;
}
void write16(uint32_t a, uint16_t v)
{
    if (a == DEV_CONSOLE_DR)  // if it is the display register, copy it into the display register
        DR = v;

    // write to a front panel LEDs here
    platform::writeAddr(a);
    if (!showDR)
        platform::writeData(v);
    if (showDR)
        platform::writeDispReg(DR);
}
};  // namespace ky11
