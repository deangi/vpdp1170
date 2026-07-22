#pragma once

// Host diagnostics that go to USB serial and Telnet (unlike LOG/LOGE, which
// are serial-only). Telnet is not gated by g_serial_silenced.

void host_diag_write(const char* text);
void host_diag_printf(const char* fmt, ...);
