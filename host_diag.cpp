#include "host_diag.h"

#include "platform.h"
#include "telnet.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

// Defined here (was previously in the sam11 cpu_pdp11.cpp unit). Set true
// after a panic dump so further LOG noise cannot overwrite the trace.
volatile bool g_serial_silenced = false;

void host_diag_write(const char* text) {
  if (!text || !*text) return;
  if (!g_serial_silenced) Serial.print(text);
  telnet_diag_text(text);
}

void host_diag_printf(const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (written <= 0) return;
  host_diag_write(buffer);
}
