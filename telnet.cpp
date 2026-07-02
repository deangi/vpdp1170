#include "telnet.h"
#include "console.h"
#include "platform.h"
#include "fifo.h"
#include "telnet_shell.h"
#include <WiFi.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// Telnet protocol bytes
#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define OPT_BINARY    0
#define OPT_ECHO      1
#define OPT_SGA       3
#define OPT_LINEMODE  34

static WiFiServer  g_server(23);
static WiFiClient  g_client;
static bool        g_enabled = false;
static bool        g_started = false;
static uint16_t    g_port = 23;
static char        g_client_ip[20] = {0};

// 8 KB output FIFO (KL11/core 1 push, telnet task/core 0 drain) and 8 KB
// input FIFO (telnet task/core 0 push, KL11/core 1 pop). Both storages live
// in PSRAM via EXT_RAM_BSS_ATTR and each ring has one producer and consumer.
#define VPDP_TELNET_FIFO_BYTES 8192   // must be power of two
EXT_RAM_BSS_ATTR static uint8_t telnet_out_storage[VPDP_TELNET_FIFO_BYTES];
EXT_RAM_BSS_ATTR static uint8_t telnet_in_storage[VPDP_TELNET_FIFO_BYTES];
static Fifo g_telnet_out;
static Fifo g_telnet_in;
static bool g_fifos_inited = false;

enum TelnetRxState : uint8_t {
  RX_DATA,
  RX_IAC,
  RX_IAC_OPTION,
  RX_SUBNEG,
  RX_SUBNEG_IAC
};
static TelnetRxState g_rx_state = RX_DATA;
static bool g_rx_after_cr = false;
static uint8_t g_shell_escape_pos = 0;
static uint32_t g_shell_escape_ms = 0;
static constexpr uint32_t SHELL_ESCAPE_TIMEOUT_MS = 5000;

static void reset_rx_parser() {
  g_rx_state = RX_DATA;
  g_rx_after_cr = false;
  g_shell_escape_pos = 0;
  g_shell_escape_ms = 0;
}

static void ensure_fifos_inited() {
  if (g_fifos_inited) return;
  g_telnet_out.init(telnet_out_storage, VPDP_TELNET_FIFO_BYTES);
  g_telnet_in.init(telnet_in_storage,  VPDP_TELNET_FIFO_BYTES);
  g_fifos_inited = true;
}

void telnet_begin(uint16_t port, bool enabled) {
  ensure_fifos_inited();
  telnet_shell_init();
  g_enabled = enabled;
  g_port    = port;
  if (!enabled) { LOG("telnet: disabled in config"); return; }
  g_server = WiFiServer(port);
  g_server.begin();
  g_server.setNoDelay(true);
  g_started = true;
  LOG("telnet: listening on port %u", port);
}

static void send_iac(uint8_t verb, uint8_t opt) {
  uint8_t b[3] = { T_IAC, verb, opt };
  g_client.write(b, 3);
}

static void on_connect() {
  reset_rx_parser();
  IPAddress ip = g_client.remoteIP();
  strncpy(g_client_ip, ip.toString().c_str(), sizeof(g_client_ip) - 1);
  g_client_ip[sizeof(g_client_ip) - 1] = 0;
  LOG("telnet: client connected from %s", g_client_ip);
  // Put the client in character-at-a-time mode.
  send_iac(T_WILL, OPT_ECHO);
  send_iac(T_WILL, OPT_SGA);
  send_iac(T_WONT, OPT_LINEMODE);
  send_iac(T_DO,   OPT_BINARY);
  g_telnet_out.clear();     // drop any stale output queued before connect
}

static void send_shell_banner() {
  g_client.print(
      "\r\nvpdp1170 management shell\r\n"
      "PDP-11 emulation continues; Telnet I/O is temporarily detached.\r\n"
      "Type help for commands, exit to return to the PDP console.\r\n"
      "vpdp:/> ");
}

static void enter_shell() {
  g_telnet_in.clear();
  g_telnet_out.clear();
  telnet_shell_enter();
  send_shell_banner();
}

static void route_console_input(uint8_t c) {
  if (telnet_shell_active()) {
    if (c == 0x08 || c == 0x7f) {
      if (telnet_shell_backspace()) g_client.print("\b \b");
      return;
    }
    if (c == '\r' || c == '\n') {
      telnet_shell_input('\r');
      g_client.print("\r\n");
      return;
    }
    if (telnet_shell_input(c)) g_client.write(&c, 1);
    return;
  }

  // ESC >> detaches this Telnet session from the PDP console. Prefix bytes
  // are delayed until the sequence either matches or fails; failures are
  // replayed unchanged so normal terminal escape sequences still work.
  if (g_shell_escape_pos == 0) {
    if (c == 0x1b) {
      g_shell_escape_pos = 1;
      g_shell_escape_ms = millis();
    }
    else g_telnet_in.push(c);
    return;
  }
  if (g_shell_escape_pos == 1) {
    if (c == '>') {
      g_shell_escape_pos = 2;
      g_shell_escape_ms = millis();
      return;
    }
    g_telnet_in.push(0x1b);
    g_shell_escape_pos = 0;
    route_console_input(c);
    return;
  }
  if (c == '>') {
    g_shell_escape_pos = 0;
    enter_shell();
    return;
  }
  g_telnet_in.push(0x1b);
  g_telnet_in.push('>');
  g_shell_escape_pos = 0;
  route_console_input(c);
}

static void expire_shell_escape() {
  if (!g_shell_escape_pos ||
      (uint32_t)(millis() - g_shell_escape_ms) <
          SHELL_ESCAPE_TIMEOUT_MS) return;
  g_telnet_in.push(0x1b);
  if (g_shell_escape_pos == 2) g_telnet_in.push('>');
  g_shell_escape_pos = 0;
  g_shell_escape_ms = 0;
}

static void drain_rx() {
  while (g_client.available()) {
    int ch = g_client.read();
    if (ch < 0) break;
    uint8_t c = (uint8_t)ch;

    switch (g_rx_state) {
      case RX_DATA:
        if (c == T_IAC) {
          g_rx_state = RX_IAC;
          break;
        }
        if (g_rx_after_cr && (c == 0x00 || c == 0x0A)) {
          g_rx_after_cr = false;
          break;
        }
        g_rx_after_cr = false;
        route_console_input(c);
        if (c == 0x0D) g_rx_after_cr = true;
        break;

      case RX_IAC:
        if (c == T_IAC) {
          route_console_input(T_IAC);
          g_rx_state = RX_DATA;
        } else if (c == T_SB) {
          g_rx_state = RX_SUBNEG;
        } else if (c == T_WILL || c == T_WONT ||
                   c == T_DO   || c == T_DONT) {
          g_rx_state = RX_IAC_OPTION;
        } else {
          g_rx_state = RX_DATA;
        }
        break;

      case RX_IAC_OPTION:
        g_rx_state = RX_DATA;
        break;

      case RX_SUBNEG:
        if (c == T_IAC) g_rx_state = RX_SUBNEG_IAC;
        break;

      case RX_SUBNEG_IAC:
        if (c == T_SE) g_rx_state = RX_DATA;
        else           g_rx_state = RX_SUBNEG;
        break;
      }
    }
}

bool telnet_in_pop(uint8_t* out) {
  return g_telnet_in.pop(out);
}

void telnet_poll() {
  if (!g_started) return;

  // Accept a new connection.
  if (g_server.hasClient()) {
    WiFiClient nc = g_server.available();
    if (g_client && g_client.connected()) {
      nc.print("\r\nvpdp1170: console already in use\r\n");
      nc.stop();
    } else {
      g_client = nc;
      g_client.setNoDelay(true);
      on_connect();
    }
  }

  if (g_client && g_client.connected()) {
    // Drain and parse the socket before considering a partial shell escape
    // expired. This parser runs entirely on the core-0 network task and is
    // independent of how quickly the PDP-11 consumes its input FIFO.
    drain_rx();
    expire_shell_escape();
    const uint8_t* shell_data;
    size_t shell_bytes;
    while ((shell_bytes = telnet_shell_output_peek(&shell_data)) > 0) {
      size_t written = g_client.write(shell_data, shell_bytes);
      if (written == 0) break;
      telnet_shell_output_consume(written);
      if (written < shell_bytes) break;
    }
    // Flush queued console output in contiguous chunks from the FIFO.
    // peek() returns the largest run that doesn't wrap, so at most two
    // calls are needed to drain the ring. We stop early if write() can't
    // take everything we offered (socket buffer full); the rest stays
    // queued for the next telnet_poll().
    const uint8_t* p;
    size_t n;
    while (!telnet_shell_active() && (n = g_telnet_out.peek(&p)) > 0) {
      size_t w = g_client.write(p, n);
      if (w == 0) break;
      g_telnet_out.consume(w);
      if (w < n) break;
    }
  } else if (g_client) {                    // client went away
    g_client.stop();
    telnet_shell_disconnect();
    reset_rx_parser();
    g_client_ip[0] = 0;
    g_telnet_out.clear();
    LOG("telnet: client disconnected");
  } else {
    // No client connected at all: drop any queued output so it doesn't
    // accumulate stale bytes that a future client would see on connect.
    g_telnet_out.clear();
  }
}

void telnet_write(uint8_t c) {
  // If telnet is disabled in config the FIFO would never drain, so
  // drop bytes at the source. When enabled but no client is connected
  // we still buffer; telnet_poll() then clears the FIFO each iteration
  // so it never accumulates stale bytes a future client would see.
  // IAC bytes in the data stream are escaped by emitting them twice
  // (RFC 854). A full FIFO drops new bytes silently.
  if (!g_started || telnet_shell_active()) return;
  g_telnet_out.push(c);
  if (c == T_IAC) g_telnet_out.push(T_IAC);
}

bool        telnet_connected() { return g_client && g_client.connected(); }
bool        telnet_listening() { return g_started && WiFi.status() == WL_CONNECTED; }
const char* telnet_client_ip() { return g_client_ip; }
uint16_t    telnet_port()      { return g_port; }
bool        telnet_enabled()   { return g_enabled; }
bool        telnet_shell_connected() { return telnet_shell_active(); }
