#include "telnet_dz.h"
#include "platform.h"

#if VPDP_ENABLE_DZ11

#include "host_lib/telnet/telnet_pipe.h"

static constexpr size_t kDzFifo = 128;
static uint8_t g_in_storage[kDzFifo];
static uint8_t g_out_storage[kDzFifo];
static TelnetPipe g_pipe;
static bool g_inited = false;

static void ensure_pipe() {
  if (g_inited) return;
  g_pipe.init(g_out_storage, sizeof(g_out_storage),
              g_in_storage, sizeof(g_in_storage));
  TelnetPipe::Hooks h;
  h.busy_msg = "\r\nvpdp1170: DZ11 line already in use\r\n";
  h.log_name = "DZ telnet";
  g_pipe.set_hooks(h);
  g_inited = true;
}

void telnet_dz_begin(uint16_t port, bool enabled) {
  ensure_pipe();
  g_pipe.begin(port, enabled);
}

void telnet_dz_poll() { g_pipe.poll(); }
void telnet_dz_write(uint8_t c) { g_pipe.write(c); }
bool telnet_dz_in_pop(uint8_t* out) { return g_pipe.in_pop(out); }
bool telnet_dz_in_available() { return g_pipe.in_available(); }
bool telnet_dz_connected() { return g_pipe.connected(); }
bool telnet_dz_listening() { return g_pipe.started(); }
const char* telnet_dz_client_ip() { return g_pipe.client_ip(); }
uint16_t telnet_dz_port() { return g_pipe.port(); }
bool telnet_dz_enabled() { return g_pipe.enabled(); }

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
