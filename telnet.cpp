#include "telnet.h"
#include "platform.h"
#include "telnet_shell.h"
#include "host_lib/telnet/telnet_pipe.h"

#include <Arduino.h>

#define VPDP_TELNET_OUT_FIFO_BYTES 8192
#define VPDP_TELNET_DIAG_FIFO_BYTES 2048
#define VPDP_TELNET_IN_FIFO_BYTES 8192

static uint8_t telnet_out_storage[VPDP_TELNET_OUT_FIFO_BYTES];
static uint8_t telnet_diag_storage[VPDP_TELNET_DIAG_FIFO_BYTES];
static uint8_t telnet_in_storage[VPDP_TELNET_IN_FIFO_BYTES];
static TelnetPipe g_pipe;
static bool g_inited = false;

static uint8_t g_shell_escape_pos = 0;
static uint32_t g_shell_escape_ms = 0;
static constexpr uint32_t SHELL_ESCAPE_TIMEOUT_MS = 5000;

static void ensure_pipe() {
  if (g_inited) return;
  g_pipe.init(telnet_out_storage, sizeof(telnet_out_storage),
              telnet_in_storage, sizeof(telnet_in_storage),
              telnet_diag_storage, sizeof(telnet_diag_storage));
  g_inited = true;
}

static void send_shell_banner() {
  g_pipe.socket().print(
      "\r\nvpdp1170 management shell\r\n"
      "PDP-11 emulation continues; Telnet I/O is temporarily detached.\r\n"
      "Type help for commands, exit to return to the PDP console.\r\n"
      "vpdp:/> ");
}

static void enter_shell() {
  g_pipe.in_clear();
  g_pipe.out_clear();
  telnet_shell_enter();
  send_shell_banner();
}

static void route_console_input(uint8_t c, void*);

static void expire_shell_escape(void*) {
  if (!g_shell_escape_pos ||
      (uint32_t)(millis() - g_shell_escape_ms) < SHELL_ESCAPE_TIMEOUT_MS)
    return;
  g_pipe.in_push(0x1b);
  if (g_shell_escape_pos == 2) g_pipe.in_push('>');
  g_shell_escape_pos = 0;
  g_shell_escape_ms = 0;
}

static void route_console_input(uint8_t c, void*) {
  if (telnet_shell_active()) {
    if (c == 0x08 || c == 0x7f) {
      if (telnet_shell_backspace()) g_pipe.socket().print("\b \b");
      return;
    }
    if (c == '\r' || c == '\n') {
      telnet_shell_input('\r');
      g_pipe.socket().print("\r\n");
      return;
    }
    if (telnet_shell_input(c)) g_pipe.socket().write(&c, 1);
    return;
  }

  if (g_shell_escape_pos == 0) {
    if (c == 0x1b) {
      g_shell_escape_pos = 1;
      g_shell_escape_ms = millis();
    } else {
      g_pipe.in_push(c);
    }
    return;
  }
  if (g_shell_escape_pos == 1) {
    if (c == '>') {
      g_shell_escape_pos = 2;
      g_shell_escape_ms = millis();
      return;
    }
    g_pipe.in_push(0x1b);
    g_shell_escape_pos = 0;
    route_console_input(c, nullptr);
    return;
  }
  if (c == '>') {
    g_shell_escape_pos = 0;
    enter_shell();
    return;
  }
  g_pipe.in_push(0x1b);
  g_pipe.in_push('>');
  g_shell_escape_pos = 0;
  route_console_input(c, nullptr);
}

static size_t shell_aux_peek(const uint8_t** data, void*) {
  return telnet_shell_output_peek(data);
}

static void shell_aux_consume(size_t n, void*) {
  telnet_shell_output_consume(n);
}

static bool shell_is_active(void*) { return telnet_shell_active(); }

static void on_disconnect(void*) {
  telnet_shell_disconnect();
  g_shell_escape_pos = 0;
  g_shell_escape_ms = 0;
}

static void install_hooks() {
  TelnetPipe::Hooks h;
  h.on_rx = route_console_input;
  h.after_rx = expire_shell_escape;
  h.on_disconnect = on_disconnect;
  h.aux_peek = shell_aux_peek;
  h.aux_consume = shell_aux_consume;
  h.shell_active = shell_is_active;
  h.busy_msg = "\r\nvpdp1170: console already in use\r\n";
  h.log_name = "telnet";
  g_pipe.set_hooks(h);
}

void telnet_begin(uint16_t port, bool enabled) {
  ensure_pipe();
  install_hooks();
  telnet_shell_init();
  g_pipe.begin(port, enabled);
}

void telnet_poll() { g_pipe.poll(); }

bool telnet_in_pop(uint8_t* out) { return g_pipe.in_pop(out); }

void telnet_reset_guest_io() {
  ensure_pipe();
  g_pipe.reset_guest_io();
  g_shell_escape_pos = 0;
  g_shell_escape_ms = 0;
}

void telnet_write(uint8_t c) { g_pipe.write(c); }
void telnet_diag_write(uint8_t c) { g_pipe.diag_write(c); }

void telnet_output_stats(uint32_t* pending, uint32_t* dropped) {
  ensure_pipe();
  if (pending) *pending = (uint32_t)g_pipe.out_count();
  if (dropped) *dropped = g_pipe.dropped();
}

bool telnet_output_has_space(size_t bytes) {
  ensure_pipe();
  return g_pipe.out_free() >= bytes;
}

void telnet_diag_text(const char* text) {
  if (!text) return;
  while (*text) telnet_diag_write((uint8_t)*text++);
}

bool        telnet_connected() { return g_pipe.connected(); }
bool        telnet_listening() { return g_pipe.listening(); }
const char* telnet_client_ip() { return g_pipe.client_ip(); }
uint16_t    telnet_port()      { return g_pipe.port(); }
bool        telnet_enabled()   { return g_pipe.enabled(); }
bool        telnet_shell_connected() { return telnet_shell_active(); }
