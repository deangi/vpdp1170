#include "telnet_dz.h"
#include "platform.h"

#if VPDP_ENABLE_DZ11

#include <WiFi.h>
#include <atomic>
#include <string.h>

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

static constexpr size_t kDzFifo = 64;

struct ByteRing64 {
  uint8_t  buf[kDzFifo] {};
  uint8_t  head = 0;
  uint8_t  tail = 0;
  uint8_t  count = 0;

  void clear() { head = tail = count = 0; }
  bool empty() const { return count == 0; }
  bool full() const { return count >= kDzFifo; }

  bool push(uint8_t c) {
    if (full()) return false;
    buf[head] = c;
    head = (uint8_t)((head + 1) % kDzFifo);
    count++;
    return true;
  }

  bool pop(uint8_t* c) {
    if (empty()) return false;
    *c = buf[tail];
    tail = (uint8_t)((tail + 1) % kDzFifo);
    count--;
    return true;
  }

  // Contiguous peek from tail for socket write.
  size_t peek(const uint8_t** out) const {
    if (empty()) { *out = nullptr; return 0; }
    *out = buf + tail;
    if (tail < head) return (size_t)(head - tail);
    return (size_t)(kDzFifo - tail);
  }

  void consume(size_t n) {
    while (n-- && count) {
      tail = (uint8_t)((tail + 1) % kDzFifo);
      count--;
    }
  }
};

static WiFiServer g_server(0);
static WiFiClient g_client;
static bool g_started = false;
static bool g_enabled = false;
static uint16_t g_port = 0;
static char g_client_ip[16] = "";
static ByteRing64 g_in;
static ByteRing64 g_out;
static std::atomic<uint32_t> g_dropped{0};

enum DzRxState : uint8_t {
  RX_DATA = 0,
  RX_IAC,
  RX_IAC_OPTION,
  RX_SUBNEG,
  RX_SUBNEG_IAC,
};
static DzRxState g_rx_state = RX_DATA;
static bool g_rx_after_cr = false;

static void reset_rx_parser() {
  g_rx_state = RX_DATA;
  g_rx_after_cr = false;
}

void telnet_dz_begin(uint16_t port, bool enabled) {
  g_enabled = enabled;
  g_port = port;
  if (!enabled) {
    LOG("DZ telnet: disabled in config");
    return;
  }
  if (port == 0) {
    LOG("DZ telnet: port 0 — not starting");
    g_enabled = false;
    return;
  }
  g_server = WiFiServer(port);
  g_server.begin();
  g_server.setNoDelay(true);
  g_started = true;
  LOG("DZ telnet: listening on port %u (line 0, %u-byte FIFOs)",
      port, (unsigned)kDzFifo);
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
  LOG("DZ telnet: client connected from %s", g_client_ip);
  send_iac(T_WILL, OPT_ECHO);
  send_iac(T_WILL, OPT_SGA);
  send_iac(T_WONT, OPT_LINEMODE);
  send_iac(T_DO,   OPT_BINARY);
  g_out.clear();
  g_in.clear();
  g_dropped.store(0, std::memory_order_relaxed);
}

static void route_input(uint8_t c) {
  if (!g_in.push(c))
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

static void drain_rx() {
  while (g_client.available()) {
    int ch = g_client.read();
    if (ch < 0) break;
    uint8_t c = (uint8_t)ch;
    switch (g_rx_state) {
      case RX_DATA:
        if (c == T_IAC) { g_rx_state = RX_IAC; break; }
        if (g_rx_after_cr && (c == 0x00 || c == 0x0A)) {
          g_rx_after_cr = false;
          break;
        }
        g_rx_after_cr = false;
        route_input(c);
        if (c == 0x0D) g_rx_after_cr = true;
        break;
      case RX_IAC:
        if (c == T_IAC) {
          route_input(T_IAC);
          g_rx_state = RX_DATA;
        } else if (c == T_SB) {
          g_rx_state = RX_SUBNEG;
        } else if (c == T_WILL || c == T_WONT || c == T_DO || c == T_DONT) {
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
        g_rx_state = (c == T_SE) ? RX_DATA : RX_SUBNEG;
        break;
    }
  }
}

static void drain_out() {
  const uint8_t* p;
  size_t n;
  while ((n = g_out.peek(&p)) > 0) {
    size_t w = g_client.write(p, n);
    if (w == 0) break;
    g_out.consume(w);
    if (w < n) break;
  }
}

void telnet_dz_poll() {
  if (!g_started) {
    g_out.clear();
    return;
  }

  if (g_server.hasClient()) {
    WiFiClient nc = g_server.available();
    if (g_client && g_client.connected()) {
      nc.print("\r\nvpdp1170: DZ11 line already in use\r\n");
      nc.stop();
    } else {
      g_client = nc;
      g_client.setNoDelay(true);
      on_connect();
    }
  }

  if (g_client && g_client.connected()) {
    drain_rx();
    drain_out();
  } else if (g_client) {
    g_client.stop();
    reset_rx_parser();
    g_client_ip[0] = 0;
    g_out.clear();
    g_in.clear();
    LOG("DZ telnet: client disconnected");
  } else {
    g_out.clear();
  }
}

void telnet_dz_write(uint8_t c) {
  if (c == T_IAC) {
    if (!g_out.push(c)) g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  if (!g_out.push(c))
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

bool telnet_dz_in_pop(uint8_t* out) {
  return g_in.pop(out);
}

bool telnet_dz_in_available() {
  return !g_in.empty();
}

bool telnet_dz_connected() {
  return g_client && g_client.connected();
}

bool telnet_dz_listening() { return g_started; }
const char* telnet_dz_client_ip() { return g_client_ip; }
uint16_t telnet_dz_port() { return g_port; }
bool telnet_dz_enabled() { return g_enabled; }

#else  // !VPDP_ENABLE_DZ11

void telnet_dz_begin(uint16_t, bool) {}
void telnet_dz_poll() {}
void telnet_dz_write(uint8_t) {}
bool telnet_dz_in_pop(uint8_t*) { return false; }
bool telnet_dz_in_available() { return false; }
bool telnet_dz_connected() { return false; }
bool telnet_dz_listening() { return false; }
const char* telnet_dz_client_ip() { return ""; }
uint16_t telnet_dz_port() { return 0; }
bool telnet_dz_enabled() { return false; }

#endif  // VPDP_ENABLE_DZ11
