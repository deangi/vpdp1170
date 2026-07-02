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

#if !H_CPU_DEBUG
#define H_CPU_DEBUG 1

void debug_step()
{
    if (PRINTSIMLINES)
    {
        if (BREAK_ON_TRAP && trapped)
        {
            //Serial.print("!");
            //printstate();

            Serial.print("\r\n%%!");
            while (!Serial.available())
                delay(1);
            char c;
            while (1)
            {
                c = Serial.read();
                if (c == '`')  // step individually
                {
                    trapped = true;
                    cont_with = false;
                    break;
                }
                if (c == '>')  // continue to the next trap
                {
                    trapped = false;
                    cont_with = false;
                    break;
                }
                if (c == '~')  // continue to the next trap, but keep printing
                {
                    trapped = false;
                    cont_with = true;
                    break;
                }
                if (c == 'd' && ALLOW_DISASM)
                {
                    trapped = false;
                    cont_with = false;
                    disasm(kt11::decode_instr(curPC, false, curuser));
                    break;
                }
                if (c == 'a')
                {
                    printstate();
                }
            }
            Serial.print(c);
            Serial.println();
        }

        if ((BREAK_ON_TRAP || PRINTINSTR || PRINTSTATE) && (cont_with || trapped))
        {
            delayMicroseconds(100);
        }
    }
}

void debug_print()
{
    if (PRINTSIMLINES)
    {
        if (PRINTINSTR && (trapped || cont_with))
        {
            _printf("%%%% instr 0%06o: 0%06o\r\n", curPC, dd11::read16(kt11::decode_instr(curPC, false, curuser)));
        }

        if ((BREAK_ON_TRAP || PRINTSTATE) && (trapped || cont_with))
            printstate();
    }
}
#endif