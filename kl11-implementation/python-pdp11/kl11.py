# MIT License
#
# Copyright (c) 2023 Neil Webber
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# simulation of a KL-11 console interface
#
# There are two modes of operation: socket or stdin.
#
# socket: In socket mode, this starts a simple TCP server and accepts
#         connections on port 1170 (cute). Characters are proxied back
#         and forth from this socket to the emulated console
#
# stdin:  In stdin mode, python's stdin is usurped and put into raw mode.
#         The python console is the emulated console. This mode
#         requires termios ("unix only" but may exist on other systems)
#
#         In this mode the normal ^C/SIGINT does nothing because in raw
#         mode there are no special characters; everything is passed to
#         the emulated environment. The only way to exit/stop the emulator is
#         if the running code halts. Alternatively, the input byte sequence:
#                   0xC3 0xA7
#         which is Unicode U+00E7, c-cedilla, will cause the emulation to
#         halt as if the physical console HALT toggle had been selected.
#         This sequence was chosen because it is option-C on a mac; see
#                 HALT_SEQUENCE
#         to override this choice. NOTE: the first N-1 HALT_SEQUENCE bytes
#         will still be transmitted to the emulation, the HALT will only occur
#         once the full sequence has been received.
#

import socket
import threading
import queue
from contextlib import contextmanager

# termios is only on unix-like/POSIX systems.
# If there is no termios module then use_stdin mode is not available.
try:
    import termios
except ModuleNotFoundError:
    termios = None
else:
    # these are only needed for use_stdin, which also requires termios
    import tty
    import sys
    import os

from pdptraps import PDPTraps
from unibus import BusCycle


class KL11:

    KL11_DEFAULT = 0o17560           # offset within I/O page
    BUSY = 0o400                     # probably no reason to ever set this
    RCDONE = 0o200                   # means character available in buf
    TXRDY = 0o200                    # same bit on tx side is called "RDY"
    IENABLE = 0o100
    RDRENA = 0o001

    _SHUTDOWN_SENTINEL = object()
    SERVERHOST = ''
    SERVERPORT = 1170

    HALT_SEQUENCE = bytes((0xc3, 0xa7))
#   HALT_SEQUENCE = bytes((0x05,))      # this will do ^E to exit, like SIMH

    def __init__(self, ub, /, *, baseaddr=KL11_DEFAULT,
                 send_telnet=False, use_stdin=False):
        """Initialize the emulated console. Listens on port 1170.

        Argument use_stdin (True/False, default False) determines whether
        a remote socket is used or stdin (python console). This only works
        on python hosts with termios (generally any POSIX-like system)

        If use_stdin is False, argument send_telnet (True/False, dflt False)
        controls whether RFC854 sequences to turn off echo, etc will be sent.
        """

        self.addr = baseaddr
        self.ub = ub
        self.ub.register(ub.autobyte(self.klregs), baseaddr, 4)
        self.send_telnet = send_telnet
        self.start_this = None

        # output characters are just queued (via tq) to the output thread
        # input characters have to undergo a more careful 1-by-1
        # dance to properly match interrupts to their arrival
        self.tq = queue.Queue()
        self.rxc = threading.Condition()

        # bits broken out of virtualized KL11 Reader Status Register
        self.rcdone = False
        self.r_ienable = False

        # reader buffer register (address: baseaddr + 2)
        self.rdrbuf = 0

        # transmit buffer status (address: baseaddr + 4)
        self.t_ienable = False

        if use_stdin:
            if not termios:
                raise ValueError("Cannot use_stdin without termios module")
            serverloop = self._stdindeferred
        else:
            serverloop = self._connectionserver

        # The socket server connection/listener
        self._t = threading.Thread(target=serverloop, daemon=True)
        self._t.start()

    def _telnetsequences(self, s):
        """If telnet is being used to connect, turn off local echo etc."""

        dont_auth = bytes((0xff, 0xf4, 0x25))
        s.sendall(dont_auth)

        suppress_goahead = bytes((0xff, 0xfb, 0x03))
        s.sendall(suppress_goahead)

        dont_linemode = bytes((0xff, 0xfe, 0x22))
        s.sendall(dont_linemode)

        dont_new_env = bytes((0xff, 0xfe, 0x27))
        s.sendall(dont_new_env)

        will_echo = bytes((0xff, 0xfb, 0x01))
        s.sendall(will_echo)

        dont_echo = bytes((0xff, 0xfe, 0x01))
        s.sendall(dont_echo)

        noecho = bytes((0xff, 0xfd, 0x2d))
        s.sendall(noecho)

    def klregs(self, addr, cycle, /, *, value=None):
        if self.start_this:
            t, self.start_this = self.start_this, None
            t.start()

        if cycle == BusCycle.RESET:
            self.rcdone = False
            self.r_ienable = False
            self.r_tenable = False
            return

        match addr - self.addr:
            case 0:              # rcsr
                if cycle == BusCycle.READ16:
                    value = 0

                    if self.r_ienable:
                        value |= self.IENABLE

                    if self.rcdone:
                        value |= self.RCDONE

                elif cycle == BusCycle.WRITE16:
                    if value & self.RDRENA:
                        with self.rxc:
                            # a request to get one character, which only
                            # has to clear the rcdone bit here.
                            self.rcdone = False
                            self.rdrbuf = 0
                            self.r_ienable = (value & self.IENABLE)
                            self.rxc.notify()

            case 2 if cycle == BusCycle.READ16:         # rbuf
                with self.rxc:
                    value = self.rdrbuf
                    self.rcdone = False
                    self.rxc.notify()

            # transmit buffer status (sometimes called tcsr)
            case 4:
                if cycle == BusCycle.READ16:
                    value = self.TXRDY      # always ready to send chars
                    if self.t_ienable:
                        value |= self.IENABLE
                elif cycle == BusCycle.WRITE16:
                    prev = self.t_ienable
                    self.t_ienable = (value & self.IENABLE)
                    if self.t_ienable and not prev:
                        self.ub.intmgr.simple_irq(pri=4, vector=0o64)

            # transmit buffer
            case 6 if cycle == BusCycle.WRITE16:              # tbuf
                value &= 0o177
                if (value != 0o177):
                    s = chr(value)
                    self.tq.put(s)
                if self.t_ienable:
                    self.ub.intmgr.simple_irq(pri=4, vector=0o64)
            case _:
                raise PDPTraps.AddressError

        return value

    def _stdindeferred(self):
        # because the stdin method steals the console I/O, which hoses
        # interactive use (if a KL11 has been set up), the start of the
        # stdin thread is deferred until any first KL11 access.
        self.start_this = threading.Thread(
            target=self._stdinserver, daemon=True)

    def _stdinserver(self):
        """Server loop daemon thread for console I/O via stdin."""

        @contextmanager
        def _rawmode(fd):
            saved = termios.tcgetattr(fd)
            try:
                tty.setraw(fd)
                yield
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, saved)

        def _outloop(q, outf):
            while True:
                try:
                    c = q.get()
                    if c is self._SHUTDOWN_SENTINEL:
                        break
                except queue.Empty:
                    pass
                else:
                    outf.write(c.encode())

        def _inloop(inf):
            in_halt_sequence = None
            while len(c := inf.read(1)) != 0:
                b = ord(c)
                with self.rxc:
                    if in_halt_sequence is None:
                        if b == self.HALT_SEQUENCE[0]:
                            in_halt_sequence = 1
                            if len(self.HALT_SEQUENCE) == 1:
                                return
                    elif b == self.HALT_SEQUENCE[in_halt_sequence]:
                        in_halt_sequence += 1
                        if len(self.HALT_SEQUENCE) == in_halt_sequence:
                            return
                    else:
                        in_halt_sequence = None

                    self.rxc.wait_for(lambda: not self.rcdone)
                    self.rdrbuf = b
                    self.rcdone = True
                    if self.r_ienable:
                        self.ub.intmgr.simple_irq(pri=4, vector=0o60)

        with _rawmode(sys.stdin.fileno()):
            inf = os.fdopen(sys.stdin.fileno(), 'rb',
                            buffering=0, closefd=False)
            outf = os.fdopen(sys.stdout.fileno(), 'wb',
                             buffering=0, closefd=False)

            outthread = threading.Thread(target=_outloop, args=(self.tq, outf))
            inthread = threading.Thread(target=_inloop, args=(inf,))

            outthread.start()
            inthread.start()

            inthread.join()
            self.tq.put(self._SHUTDOWN_SENTINEL)
            outthread.join()

        self.ub.intmgr.halt_toggle(f"CONSOLE HALT ({self.HALT_SEQUENCE})")

    def _connectionserver(self):
        """Server loop daemon thread for console I/O via telnet/nc socket."""
        serversocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        serversocket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        serversocket.bind((self.SERVERHOST, self.SERVERPORT))
        serversocket.listen(1)

        def _outloop(q, s):
            while True:
                try:
                    c = q.get()
                    if c is self._SHUTDOWN_SENTINEL:
                        break
                except queue.Empty:
                    pass
                else:
                    s.sendall(c.encode())

        def _inloop(s):
            while len(b := s.recv(1)) != 0:
                with self.rxc:
                    self.rxc.wait_for(lambda: not self.rcdone)
                    self.rdrbuf = ord(b)
                    self.rcdone = True
                    if self.r_ienable:
                        self.ub.intmgr.simple_irq(pri=4, vector=0o60)

        while True:
            s, addr = serversocket.accept()

            if self.send_telnet:
                self._telnetsequences(s)

            outthread = threading.Thread(target=_outloop, args=(self.tq, s))
            inthread = threading.Thread(target=_inloop, args=(s,))

            outthread.start()
            inthread.start()

            inthread.join()
            self.tq.put(self._SHUTDOWN_SENTINEL)
            outthread.join()

    # debugging tool
    def statestring(self):
        s = ""
        if self.r_ienable:
            s += " RINT"
        if self.t_ienable:
            s += " TINT"
        if self.rdrbuf:
            s += f" LC={self.rdrbuf}"
        if self.rcdone:
            s += f" RCDONE"
        return s
