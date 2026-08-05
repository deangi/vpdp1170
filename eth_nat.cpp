#include "eth_nat.h"

#include "platform.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/ip.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "ping/ping_sock.h"
#include <cerrno>
#include <esp_random.h>

#ifndef LOCK_TCPIP_CORE
#define LOCK_TCPIP_CORE()
#define UNLOCK_TCPIP_CORE()
#endif

// Guest ICMP echo is forwarded with esp_ping (SOCK_RAW recv is unreliable
// on ESP32 — replies are eaten by the stack and never surface to recvfrom).

namespace eth_nat {

namespace {

constexpr uint16_t ETHERTYPE_ARP = 0x0806;
constexpr uint16_t ETHERTYPE_IP  = 0x0800;
constexpr uint16_t ARP_REQUEST   = 1;
constexpr uint16_t ARP_REPLY     = 2;

constexpr uint8_t PROTO_ICMP = 1;
constexpr uint8_t PROTO_TCP  = 6;
constexpr uint8_t PROTO_UDP  = 17;

constexpr size_t RX_QUEUE   = 16;
constexpr size_t NAT_SLOTS  = 32;
constexpr uint32_t UDP_IDLE_MS  = 60000;
constexpr uint32_t TCP_IDLE_MS  = 300000;
constexpr uint32_t ICMP_IDLE_MS = 30000;

uint8_t  guest_mac[6]   = { 0x08, 0x00, 0x2B, 0x11, 0x70, 0x01 };
uint8_t  gateway_mac[6] = { 0x02, 0x00, 0x2B, 0x11, 0x70, 0x00 };
uint32_t guest_ip   = 0x0A0B0002u;
uint32_t guest_mask = 0xFFFFFF00u;
uint32_t gateway_ip = 0x0A0B0001u;
uint32_t sta_ip     = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

struct RxSlot {
  uint16_t len;
  uint8_t  data[MAX_FRAME];
};

RxSlot rxq[RX_QUEUE];
uint8_t rx_head = 0;
uint8_t rx_tail = 0;
uint8_t rx_count = 0;
uint32_t rx_drops = 0;

enum TcpPhase : uint8_t {
  TP_UNUSED = 0,
  TP_CONNECTING,
  TP_ESTABLISHED,
  TP_CLOSING,
};

struct NatEntry {
  uint8_t  proto;
  uint8_t  tcp_phase;
  uint32_t guest_ip;
  uint32_t remote_ip;
  uint16_t guest_port;   // UDP/TCP port or ICMP identifier
  uint16_t remote_port;  // UDP/TCP port; ICMP seq
  uint32_t last_ms;
  union {
    struct udp_pcb* udp;
    struct tcp_pcb* tcp;
  } pcb;
  esp_ping_handle_t ping;
  // Full original guest IP datagram (echo request) for reply rewrite.
  // 2.11BSD ping keeps id/seq in host order; rewriting the saved packet
  // preserves those bytes exactly (same path as gateway local echo).
  uint8_t  saved_ihl;
  uint16_t saved_ip_len;
  uint8_t  saved_ip[20 + 8 + 56];
  uint8_t  icmp_queued;   // already sitting in icmp_out_q
  uint8_t  icmp_replied;  // success callback already delivered to guest
  // Guest-facing TCP sequence state (host byte order).
  uint32_t g_isn;
  uint32_t h_isn;
  uint32_t g_seq;
  uint32_t h_seq;
  uint32_t g_ack;
};

NatEntry nat[NAT_SLOTS];

// Serialize esp_ping: one in-flight session (concurrent sessions were
// delivering callbacks the guest did not accept).
esp_ping_handle_t icmp_active_ping = nullptr;
NatEntry* icmp_active_entry = nullptr;

// ICMP work queue: guest TX enqueues; net_task/host_poll starts esp_ping.
struct IcmpOut {
  uint32_t src;
  uint32_t dst;
  uint16_t id;
  uint16_t seq;
};
constexpr size_t ICMP_OUT_Q = 8;
IcmpOut icmp_out_q[ICMP_OUT_Q];
uint8_t icmp_out_head = 0;
uint8_t icmp_out_tail = 0;
uint8_t icmp_out_count = 0;

// UDP/TCP work queues: guest TX only enqueues; lwIP runs on net_task.
// Keep these small — full 1400B × 16 × 2 blew ~45KB BSS and stopped the
// emulator from ever retiring boot-stub instructions (0.00 MIPS).
constexpr size_t L4_OUT_Q = 8;
constexpr size_t L4_PAYLOAD_MAX = 512;

struct UdpOut {
  uint32_t src;
  uint32_t dst;
  uint16_t sport;
  uint16_t dport;
  uint16_t plen;
  uint8_t  payload[L4_PAYLOAD_MAX];
};
UdpOut udp_out_q[L4_OUT_Q];
uint8_t udp_out_head = 0;
uint8_t udp_out_tail = 0;
uint8_t udp_out_count = 0;

struct TcpOut {
  uint32_t src;
  uint32_t dst;
  uint16_t sport;
  uint16_t dport;
  uint32_t seq;
  uint32_t ack;
  uint8_t  flags;
  uint16_t plen;
  uint8_t  payload[L4_PAYLOAD_MAX];
};
TcpOut tcp_out_q[L4_OUT_Q];
uint8_t tcp_out_head = 0;
uint8_t tcp_out_tail = 0;
uint8_t tcp_out_count = 0;
bool nat_flush_requested = false;

uint16_t rd16be(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void wr16be(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xff);
}

void wr32be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xff);
}

uint32_t rd32be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint16_t inet_checksum(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < len; i += 2)
    sum += rd16be(data + i);
  if (len & 1)
    sum += (uint16_t)data[len - 1] << 8;
  while (sum >> 16)
    sum = (sum & 0xffffu) + (sum >> 16);
  return (uint16_t)~sum;
}

uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t* tcp,
                      size_t tcp_len) {
  uint32_t sum = 0;
  sum += (src >> 16) & 0xffffu;
  sum += src & 0xffffu;
  sum += (dst >> 16) & 0xffffu;
  sum += dst & 0xffffu;
  sum += (uint32_t)PROTO_TCP;
  sum += (uint32_t)tcp_len;
  for (size_t i = 0; i + 1 < tcp_len; i += 2)
    sum += rd16be(tcp + i);
  if (tcp_len & 1)
    sum += (uint16_t)tcp[tcp_len - 1] << 8;
  while (sum >> 16)
    sum = (sum & 0xffffu) + (sum >> 16);
  return (uint16_t)~sum;
}

bool enqueue_rx_locked(const uint8_t* frame, size_t len) {
  if (!frame || len < 14) return false;
  if (len > MAX_FRAME) len = MAX_FRAME;
  if (rx_count >= RX_QUEUE) {
    rx_drops++;
    return false;
  }
  RxSlot& s = rxq[rx_tail];
  memcpy(s.data, frame, len);
  if (len < MIN_FRAME) {
    memset(s.data + len, 0, MIN_FRAME - len);
    len = MIN_FRAME;
  }
  s.len = (uint16_t)len;
  rx_tail = (uint8_t)((rx_tail + 1) % RX_QUEUE);
  rx_count++;
  return true;
}

bool enqueue_rx(const uint8_t* frame, size_t len) {
  portENTER_CRITICAL(&mux);
  bool ok = enqueue_rx_locked(frame, len);
  portEXIT_CRITICAL(&mux);
  return ok;
}

bool on_link(uint32_t ip) {
  return (ip & guest_mask) == (guest_ip & guest_mask);
}

bool deliver_ipv4_to_guest(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                           const uint8_t* l4, size_t l4_len) {
  if (l4_len > MAX_FRAME - 14 - 20) l4_len = MAX_FRAME - 14 - 20;
  uint8_t frame[MAX_FRAME];
  memset(frame, 0, sizeof(frame));
  memcpy(frame + 0, guest_mac, 6);
  memcpy(frame + 6, gateway_mac, 6);
  wr16be(frame + 12, ETHERTYPE_IP);

  uint8_t* ip = frame + 14;
  ip[0] = 0x45;
  ip[1] = 0;
  wr16be(ip + 2, (uint16_t)(20 + l4_len));
  wr16be(ip + 4, 0);
  wr16be(ip + 6, 0);  // no DF — match typical guest IP headers
  ip[8] = 64;
  ip[9] = proto;
  wr16be(ip + 10, 0);
  wr32be(ip + 12, src_ip);
  wr32be(ip + 16, dst_ip);
  wr16be(ip + 10, inet_checksum(ip, 20));
  memcpy(ip + 20, l4, l4_len);

  if (proto == PROTO_TCP) {
    wr16be(ip + 20 + 16, 0);
    wr16be(ip + 20 + 16, tcp_checksum(src_ip, dst_ip, ip + 20, l4_len));
  }

  return enqueue_rx(frame, 14 + 20 + l4_len);
}

// Forward decl — defined with ARP/gateway helpers below.
void local_icmp_echo(const uint8_t* ip, size_t ihl, size_t ip_len,
                     const uint8_t* frame, uint32_t reply_src,
                     const char* why);
void drain_icmp_out_queue();
void drain_udp_out_queue();
void drain_tcp_out_queue();

void free_pcb(uint8_t proto, void* pcb) {
  if (!pcb) return;
  LOCK_TCPIP_CORE();
  if (proto == PROTO_UDP)
    udp_remove((struct udp_pcb*)pcb);
  else if (proto == PROTO_TCP) {
    struct tcp_pcb* t = (struct tcp_pcb*)pcb;
    tcp_arg(t, nullptr);
    tcp_recv(t, nullptr);
    tcp_err(t, nullptr);
    tcp_sent(t, nullptr);
    tcp_abort(t);
  }
  UNLOCK_TCPIP_CORE();
}

void stop_ping(esp_ping_handle_t ping) {
  // esp_ping_stop leads to on_ping_end, which deletes the session.
  if (ping) esp_ping_stop(ping);
}

void free_entry_locked(NatEntry* e) {
  if (!e || e->proto == 0) return;
  uint8_t proto = e->proto;
  void* pcb = e->pcb.udp;
  esp_ping_handle_t ping = e->ping;
  memset(e, 0, sizeof(*e));
  portEXIT_CRITICAL(&mux);
  free_pcb(proto, pcb);
  stop_ping(ping);
  portENTER_CRITICAL(&mux);
}

static inline uint32_t ip4_addr_host(const ip_addr_t* addr) {
  return lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));
}

NatEntry* alloc_entry_locked() {
  // Prefer free slot; else oldest idle (never steal an in-flight ping).
  NatEntry* oldest = nullptr;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == 0) return &nat[i];
    if (nat[i].proto == PROTO_ICMP && nat[i].ping) continue;
    if (!oldest || (int32_t)(nat[i].last_ms - oldest->last_ms) < 0)
      oldest = &nat[i];
  }
  if (oldest) free_entry_locked(oldest);
  return oldest;
}

// ---- UDP (outbound via queue → net_task / lwIP) ----

void udp_recv_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                 const ip_addr_t* addr, u16_t port) {
  (void)pcb;
  NatEntry* e = (NatEntry*)arg;
  if (!e || !p) {
    if (p) pbuf_free(p);
    return;
  }

  size_t payload_len = p->tot_len;
  if (payload_len > L4_PAYLOAD_MAX) payload_len = L4_PAYLOAD_MAX;
  // Heap — tcpip thread stack is too small for a 512B+ frame local.
  uint8_t* udp = (uint8_t*)malloc(8 + payload_len);
  if (!udp) {
    pbuf_free(p);
    return;
  }
  wr16be(udp + 0, port);
  wr16be(udp + 2, e->guest_port);
  wr16be(udp + 4, (uint16_t)(8 + payload_len));
  wr16be(udp + 6, 0);
  pbuf_copy_partial(p, udp + 8, (u16_t)payload_len, 0);
  pbuf_free(p);

  uint32_t rip = ip4_addr_host(addr);
  e->last_ms = millis();
  deliver_ipv4_to_guest(rip, e->guest_ip, PROTO_UDP, udp, 8 + payload_len);
  free(udp);
}

bool nat_udp_out(uint32_t src, uint32_t dst, const uint8_t* udp,
                 size_t udp_len) {
  if (sta_ip == 0 || udp_len < 8) return false;
  const uint16_t sport = rd16be(udp + 0);
  const uint16_t dport = rd16be(udp + 2);
  size_t payload_len = udp_len - 8;
  if (payload_len > L4_PAYLOAD_MAX) payload_len = L4_PAYLOAD_MAX;

  portENTER_CRITICAL(&mux);
  NatEntry* e = nullptr;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == PROTO_UDP && nat[i].guest_ip == src &&
        nat[i].guest_port == sport && nat[i].remote_ip == dst &&
        nat[i].remote_port == dport) {
      e = &nat[i];
      break;
    }
  }
  if (!e) {
    e = alloc_entry_locked();
    if (!e) {
      portEXIT_CRITICAL(&mux);
      return false;
    }
    e->proto = PROTO_UDP;
    e->guest_ip = src;
    e->remote_ip = dst;
    e->guest_port = sport;
    e->remote_port = dport;
    e->pcb.udp = nullptr;
  }
  e->last_ms = millis();

  if (udp_out_count >= L4_OUT_Q) {
    portEXIT_CRITICAL(&mux);
    LOGE("eth_nat: UDP out queue full");
    return false;
  }
  UdpOut& slot = udp_out_q[udp_out_tail];
  slot.src = src;
  slot.dst = dst;
  slot.sport = sport;
  slot.dport = dport;
  slot.plen = (uint16_t)payload_len;
  if (payload_len) memcpy(slot.payload, udp + 8, payload_len);
  udp_out_tail = (uint8_t)((udp_out_tail + 1) % L4_OUT_Q);
  udp_out_count++;
  portEXIT_CRITICAL(&mux);
  return true;
}

void drain_udp_out_queue() {
  for (;;) {
    // Copy header fields only — do not put a full UdpOut on the stack.
    uint32_t src, dst;
    uint16_t sport, dport, plen;
    uint8_t payload[L4_PAYLOAD_MAX];
    NatEntry* e = nullptr;

    portENTER_CRITICAL(&mux);
    if (udp_out_count == 0) {
      portEXIT_CRITICAL(&mux);
      return;
    }
    UdpOut& slot = udp_out_q[udp_out_head];
    src = slot.src;
    dst = slot.dst;
    sport = slot.sport;
    dport = slot.dport;
    plen = slot.plen;
    if (plen > L4_PAYLOAD_MAX) plen = L4_PAYLOAD_MAX;
    if (plen) memcpy(payload, slot.payload, plen);
    udp_out_head = (uint8_t)((udp_out_head + 1) % L4_OUT_Q);
    udp_out_count--;

    for (size_t i = 0; i < NAT_SLOTS; i++) {
      if (nat[i].proto == PROTO_UDP && nat[i].guest_ip == src &&
          nat[i].guest_port == sport && nat[i].remote_ip == dst &&
          nat[i].remote_port == dport) {
        e = &nat[i];
        break;
      }
    }
    portEXIT_CRITICAL(&mux);
    if (!e) continue;

    LOCK_TCPIP_CORE();
    if (!e->pcb.udp) {
      e->pcb.udp = udp_new();
      if (!e->pcb.udp) {
        UNLOCK_TCPIP_CORE();
        LOGE("eth_nat: udp_new failed");
        continue;
      }
      if (udp_bind(e->pcb.udp, IP_ANY_TYPE, 0) != ERR_OK) {
        udp_remove(e->pcb.udp);
        e->pcb.udp = nullptr;
        UNLOCK_TCPIP_CORE();
        LOGE("eth_nat: udp_bind failed");
        continue;
      }
      udp_recv(e->pcb.udp, udp_recv_cb, e);
    }
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)plen, PBUF_RAM);
    if (!p) {
      UNLOCK_TCPIP_CORE();
      continue;
    }
    if (plen) memcpy(p->payload, payload, plen);
    ip_addr_t dip;
    IP_ADDR4(&dip, (dst >> 24) & 0xff, (dst >> 16) & 0xff, (dst >> 8) & 0xff,
             dst & 0xff);
    err_t err = udp_sendto(e->pcb.udp, p, &dip, dport);
    pbuf_free(p);
    UNLOCK_TCPIP_CORE();
    if (err == ERR_OK) {
      LOG("eth_nat: UDP %u -> %u.%u.%u.%u:%u (%u B)", (unsigned)sport,
          (unsigned)((dst >> 24) & 0xff), (unsigned)((dst >> 16) & 0xff),
          (unsigned)((dst >> 8) & 0xff), (unsigned)(dst & 0xff),
          (unsigned)dport, (unsigned)plen);
    } else {
      LOGE("eth_nat: udp_sendto failed (%d)", (int)err);
    }
  }
}

// ---- ICMP (outbound echo via esp_ping, one-at-a-time) ----

void on_ping_success(esp_ping_handle_t hdl, void* args) {
  (void)hdl;
  NatEntry* e = (NatEntry*)args;
  if (!e || e->proto != PROTO_ICMP) return;

  // Snapshot under lock — entry must not be freed mid-callback.
  uint8_t saved_ip[sizeof(e->saved_ip)];
  size_t ihl = 0;
  size_t ip_len = 0;
  uint32_t remote = 0;
  uint16_t id = 0, seq = 0;
  uint8_t fake_eth[14];
  portENTER_CRITICAL(&mux);
  if (e->proto != PROTO_ICMP || e->saved_ip_len < 28) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  // One forged reply per esp_ping session (esp_ping may theoretically
  // surface more than one recv for count=1 in edge cases).
  if (e->icmp_replied) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  e->icmp_replied = 1;
  ihl = e->saved_ihl ? e->saved_ihl : 20;
  ip_len = e->saved_ip_len;
  if (ip_len > sizeof(saved_ip)) ip_len = sizeof(saved_ip);
  memcpy(saved_ip, e->saved_ip, ip_len);
  remote = e->remote_ip;
  id = e->guest_port;
  seq = e->remote_port;
  e->last_ms = millis();
  portEXIT_CRITICAL(&mux);

  memset(fake_eth, 0, sizeof(fake_eth));
  memcpy(fake_eth + 6, guest_mac, 6);
  local_icmp_echo(saved_ip, ihl, ip_len, fake_eth, remote, "nat");
  // 2.11BSD stores id/seq in host order; log host values (byte-swap of BE read).
  const uint16_t id_h = (uint16_t)((id >> 8) | (id << 8));
  const uint16_t seq_h = (uint16_t)((seq >> 8) | (seq << 8));
  LOG("eth_nat: ICMP reply id=%u seq=%u from %u.%u.%u.%u -> guest",
      (unsigned)id_h, (unsigned)seq_h,
      (unsigned)((remote >> 24) & 0xff),
      (unsigned)((remote >> 16) & 0xff),
      (unsigned)((remote >> 8) & 0xff),
      (unsigned)(remote & 0xff));
}

void on_ping_timeout(esp_ping_handle_t hdl, void* args) {
  (void)hdl;
  NatEntry* e = (NatEntry*)args;
  if (!e) return;
  LOG("eth_nat: ICMP timeout id=%u seq=%u %u.%u.%u.%u",
      (unsigned)e->guest_port, (unsigned)e->remote_port,
      (unsigned)((e->remote_ip >> 24) & 0xff),
      (unsigned)((e->remote_ip >> 16) & 0xff),
      (unsigned)((e->remote_ip >> 8) & 0xff),
      (unsigned)(e->remote_ip & 0xff));
}

void on_ping_end(esp_ping_handle_t hdl, void* args) {
  NatEntry* e = (NatEntry*)args;
  esp_ping_delete_session(hdl);
  portENTER_CRITICAL(&mux);
  if (e && e->ping == hdl) e->ping = nullptr;
  if (icmp_active_ping == hdl) {
    icmp_active_ping = nullptr;
    icmp_active_entry = nullptr;
  }
  portEXIT_CRITICAL(&mux);
  // Kick the next queued echo immediately (don't wait for net_task).
  drain_icmp_out_queue();
}

bool start_esp_ping(NatEntry* e) {
  if (!e || e->ping || icmp_active_ping) return false;

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&cfg.target_addr, (e->remote_ip >> 24) & 0xff,
           (e->remote_ip >> 16) & 0xff, (e->remote_ip >> 8) & 0xff,
           e->remote_ip & 0xff);
  cfg.count = 1;
  cfg.interval_ms = 100;
  cfg.timeout_ms = 1500;
  // Payload size from saved guest echo (ihl + 8 ICMP hdr).
  size_t plen = 0;
  if (e->saved_ip_len > (size_t)e->saved_ihl + 8u)
    plen = e->saved_ip_len - (size_t)e->saved_ihl - 8u;
  cfg.data_size = plen ? (uint32_t)plen : 32;
  if (cfg.data_size > 56) cfg.data_size = 56;
  cfg.task_stack_size = 4096;
  cfg.task_prio = 2;

  esp_ping_callbacks_t cbs = {};
  cbs.cb_args = e;
  cbs.on_ping_success = on_ping_success;
  cbs.on_ping_timeout = on_ping_timeout;
  cbs.on_ping_end = on_ping_end;

  esp_ping_handle_t hdl = nullptr;
  esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
  if (err != ESP_OK || !hdl) {
    LOGE("eth_nat: esp_ping_new_session failed (%d)", (int)err);
    return false;
  }
  e->ping = hdl;
  icmp_active_ping = hdl;
  icmp_active_entry = e;
  e->icmp_replied = 0;
  err = esp_ping_start(hdl);
  if (err != ESP_OK) {
    LOGE("eth_nat: esp_ping_start failed (%d)", (int)err);
    esp_ping_delete_session(hdl);
    e->ping = nullptr;
    icmp_active_ping = nullptr;
    icmp_active_entry = nullptr;
    return false;
  }
  return true;
}

bool nat_icmp_out(uint32_t src, uint32_t dst, const uint8_t* ip, size_t ihl,
                  size_t ip_len) {
  if (sta_ip == 0) {
    LOGE("eth_nat: ICMP fwd dropped (no STA IP)");
    return false;
  }
  if (!ip || ihl < 20 || ip_len < ihl + 8) return false;
  const uint8_t* icmp = ip + ihl;
  if (icmp[0] != 8) return false;

  // Key by wire bytes of id/seq (2.11BSD stores host order — not htons).
  const uint16_t id = rd16be(icmp + 4);
  const uint16_t seq = rd16be(icmp + 6);
  size_t save_len = ip_len;
  if (save_len > sizeof(nat[0].saved_ip)) save_len = sizeof(nat[0].saved_ip);

  portENTER_CRITICAL(&mux);
  NatEntry* e = nullptr;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == PROTO_ICMP && nat[i].guest_ip == src &&
        nat[i].guest_port == id && nat[i].remote_port == seq &&
        nat[i].remote_ip == dst) {
      e = &nat[i];
      break;
    }
  }
  if (!e) {
    e = alloc_entry_locked();
    if (!e) {
      portEXIT_CRITICAL(&mux);
      LOGE("eth_nat: ICMP NAT table full");
      return false;
    }
    e->proto = PROTO_ICMP;
    e->guest_ip = src;
    e->remote_ip = dst;
    e->guest_port = id;
    e->remote_port = seq;
    e->pcb.udp = nullptr;
    e->ping = nullptr;
    e->icmp_queued = 0;
    e->icmp_replied = 0;
  }
  e->saved_ihl = (uint8_t)ihl;
  e->saved_ip_len = (uint16_t)save_len;
  memcpy(e->saved_ip, ip, save_len);
  e->last_ms = millis();

  // Already in-flight or waiting in the out queue — do not double-schedule
  // (that produced guest DUP! replies and lost later seq numbers).
  if (e->ping || e->icmp_queued) {
    portEXIT_CRITICAL(&mux);
    return true;
  }

  if (icmp_out_count >= ICMP_OUT_Q) {
    portEXIT_CRITICAL(&mux);
    LOGE("eth_nat: ICMP out queue full");
    return false;
  }
  IcmpOut& slot = icmp_out_q[icmp_out_tail];
  slot.src = src;
  slot.dst = dst;
  slot.id = id;
  slot.seq = seq;
  icmp_out_tail = (uint8_t)((icmp_out_tail + 1) % ICMP_OUT_Q);
  icmp_out_count++;
  e->icmp_queued = 1;
  e->icmp_replied = 0;
  portEXIT_CRITICAL(&mux);
  return true;
}

void drain_icmp_out_queue() {
  // Only one esp_ping at a time; host_poll retries while queue non-empty.
  if (icmp_active_ping) return;

  IcmpOut slot;
  portENTER_CRITICAL(&mux);
  if (icmp_out_count == 0) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  slot = icmp_out_q[icmp_out_head];
  icmp_out_head = (uint8_t)((icmp_out_head + 1) % ICMP_OUT_Q);
  icmp_out_count--;

  NatEntry* e = nullptr;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == PROTO_ICMP && nat[i].guest_ip == slot.src &&
        nat[i].guest_port == slot.id && nat[i].remote_port == slot.seq &&
        nat[i].remote_ip == slot.dst) {
      e = &nat[i];
      break;
    }
  }
  if (e) e->icmp_queued = 0;
  portEXIT_CRITICAL(&mux);
  if (!e) return;
  if (!start_esp_ping(e))
    LOGE("eth_nat: ping start failed for %u.%u.%u.%u seq=%u",
         (unsigned)((slot.dst >> 24) & 0xff),
         (unsigned)((slot.dst >> 16) & 0xff),
         (unsigned)((slot.dst >> 8) & 0xff),
         (unsigned)(slot.dst & 0xff), (unsigned)slot.seq);
}

// ---- TCP proxy (outbound via queue → net_task / lwIP) ----

constexpr uint8_t TH_FIN = 0x01;
constexpr uint8_t TH_SYN = 0x02;
constexpr uint8_t TH_RST = 0x04;
constexpr uint8_t TH_PSH = 0x08;
constexpr uint8_t TH_ACK = 0x10;

void tcp_send_to_guest(NatEntry* e, uint8_t flags, const uint8_t* data,
                       size_t data_len, uint32_t seq, uint32_t ack) {
  if (data_len > L4_PAYLOAD_MAX) data_len = L4_PAYLOAD_MAX;
  uint8_t* seg = (uint8_t*)malloc(20 + data_len);
  if (!seg) return;
  wr16be(seg + 0, e->remote_port);
  wr16be(seg + 2, e->guest_port);
  wr32be(seg + 4, seq);
  wr32be(seg + 8, ack);
  seg[12] = (5 << 4);  // data offset
  seg[13] = flags;
  wr16be(seg + 14, 4096);
  wr16be(seg + 16, 0);
  wr16be(seg + 18, 0);
  if (data_len) memcpy(seg + 20, data, data_len);
  size_t seg_len = 20 + data_len;
  wr16be(seg + 16, tcp_checksum(e->remote_ip, e->guest_ip, seg, seg_len));
  deliver_ipv4_to_guest(e->remote_ip, e->guest_ip, PROTO_TCP, seg, seg_len);
  free(seg);
}

err_t tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
  NatEntry* e = (NatEntry*)arg;
  if (!e) {
    if (p) pbuf_free(p);
    return ERR_OK;
  }
  if (!p) {
    tcp_send_to_guest(e, TH_FIN | TH_ACK, nullptr, 0, e->h_seq, e->g_seq);
    e->h_seq++;
    e->tcp_phase = TP_CLOSING;
    return ERR_OK;
  }
  if (err != ERR_OK) {
    pbuf_free(p);
    return err;
  }

  size_t left = p->tot_len;
  size_t off = 0;
  uint8_t* buf = (uint8_t*)malloc(L4_PAYLOAD_MAX);
  if (!buf) {
    pbuf_free(p);
    return ERR_MEM;
  }
  while (left > 0) {
    size_t chunk = left > L4_PAYLOAD_MAX ? L4_PAYLOAD_MAX : left;
    pbuf_copy_partial(p, buf, (u16_t)chunk, (u16_t)off);
    tcp_send_to_guest(e, TH_PSH | TH_ACK, buf, chunk, e->h_seq, e->g_seq);
    e->h_seq += (uint32_t)chunk;
    off += chunk;
    left -= chunk;
  }
  free(buf);
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  e->last_ms = millis();
  return ERR_OK;
}

err_t tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err) {
  (void)tpcb;
  NatEntry* e = (NatEntry*)arg;
  if (!e) return ERR_ARG;
  if (err != ERR_OK) {
    tcp_send_to_guest(e, TH_RST | TH_ACK, nullptr, 0, 0, e->g_isn + 1);
    portENTER_CRITICAL(&mux);
    free_entry_locked(e);
    portEXIT_CRITICAL(&mux);
    return err;
  }
  e->h_isn = (uint32_t)esp_random();
  e->h_seq = e->h_isn + 1;
  e->g_seq = e->g_isn + 1;
  e->tcp_phase = TP_ESTABLISHED;
  e->last_ms = millis();
  tcp_send_to_guest(e, TH_SYN | TH_ACK, nullptr, 0, e->h_isn, e->g_seq);
  LOG("eth_nat: TCP connected %u.%u.%u.%u:%u",
      (unsigned)((e->remote_ip >> 24) & 0xff),
      (unsigned)((e->remote_ip >> 16) & 0xff),
      (unsigned)((e->remote_ip >> 8) & 0xff),
      (unsigned)(e->remote_ip & 0xff), (unsigned)e->remote_port);
  return ERR_OK;
}

void tcp_err_cb(void* arg, err_t err) {
  (void)err;
  NatEntry* e = (NatEntry*)arg;
  if (!e) return;
  e->pcb.tcp = nullptr;
  tcp_send_to_guest(e, TH_RST | TH_ACK, nullptr, 0, e->h_seq, e->g_seq);
  portENTER_CRITICAL(&mux);
  memset(e, 0, sizeof(*e));
  portEXIT_CRITICAL(&mux);
}

bool nat_tcp_out(uint32_t src, uint32_t dst, const uint8_t* tcp,
                 size_t tcp_len) {
  if (sta_ip == 0 || tcp_len < 20) return false;
  const uint16_t sport = rd16be(tcp + 0);
  const uint16_t dport = rd16be(tcp + 2);
  const uint32_t seq = rd32be(tcp + 4);
  const uint32_t ack = rd32be(tcp + 8);
  const uint8_t data_off = (uint8_t)((tcp[12] >> 4) * 4u);
  if (data_off < 20 || tcp_len < data_off) return false;
  const uint8_t flags = tcp[13];
  size_t data_len = tcp_len - data_off;
  if (data_len > L4_PAYLOAD_MAX) data_len = L4_PAYLOAD_MAX;
  const uint8_t* data = tcp + data_off;

  portENTER_CRITICAL(&mux);
  NatEntry* e = nullptr;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == PROTO_TCP && nat[i].guest_ip == src &&
        nat[i].guest_port == sport && nat[i].remote_ip == dst &&
        nat[i].remote_port == dport) {
      e = &nat[i];
      break;
    }
  }

  if (flags & TH_RST) {
    if (e) {
      // Queue a RST work item so net_task can abort the PCB safely.
      if (tcp_out_count < L4_OUT_Q) {
        TcpOut& slot = tcp_out_q[tcp_out_tail];
        slot.src = src;
        slot.dst = dst;
        slot.sport = sport;
        slot.dport = dport;
        slot.seq = seq;
        slot.ack = ack;
        slot.flags = TH_RST;
        slot.plen = 0;
        tcp_out_tail = (uint8_t)((tcp_out_tail + 1) % L4_OUT_Q);
        tcp_out_count++;
      } else {
        free_entry_locked(e);
      }
    }
    portEXIT_CRITICAL(&mux);
    return true;
  }

  if (!e) {
    if (!(flags & TH_SYN) || (flags & TH_ACK)) {
      portEXIT_CRITICAL(&mux);
      return false;
    }
    e = alloc_entry_locked();
    if (!e) {
      portEXIT_CRITICAL(&mux);
      return false;
    }
    e->proto = PROTO_TCP;
    e->tcp_phase = TP_CONNECTING;
    e->guest_ip = src;
    e->remote_ip = dst;
    e->guest_port = sport;
    e->remote_port = dport;
    e->g_isn = seq;
    e->g_seq = seq + 1;
    e->pcb.tcp = nullptr;
  }

  e->last_ms = millis();
  if (flags & TH_ACK) e->g_ack = ack;
  if (data_len > 0) e->g_seq = seq + (uint32_t)data_len;
  else if (flags & (TH_SYN | TH_FIN)) e->g_seq = seq + 1;
  else e->g_seq = seq;

  if (tcp_out_count >= L4_OUT_Q) {
    portEXIT_CRITICAL(&mux);
    LOGE("eth_nat: TCP out queue full");
    return false;
  }
  TcpOut& slot = tcp_out_q[tcp_out_tail];
  slot.src = src;
  slot.dst = dst;
  slot.sport = sport;
  slot.dport = dport;
  slot.seq = seq;
  slot.ack = ack;
  slot.flags = flags;
  slot.plen = (uint16_t)data_len;
  if (data_len) memcpy(slot.payload, data, data_len);
  tcp_out_tail = (uint8_t)((tcp_out_tail + 1) % L4_OUT_Q);
  tcp_out_count++;
  portEXIT_CRITICAL(&mux);
  return true;
}

void drain_tcp_out_queue() {
  for (;;) {
    uint32_t src, dst, seq, ack;
    uint16_t sport, dport, plen;
    uint8_t flags;
    uint8_t payload[L4_PAYLOAD_MAX];
    NatEntry* e = nullptr;

    portENTER_CRITICAL(&mux);
    if (tcp_out_count == 0) {
      portEXIT_CRITICAL(&mux);
      return;
    }
    TcpOut& slot = tcp_out_q[tcp_out_head];
    src = slot.src;
    dst = slot.dst;
    sport = slot.sport;
    dport = slot.dport;
    seq = slot.seq;
    ack = slot.ack;
    flags = slot.flags;
    plen = slot.plen;
    if (plen > L4_PAYLOAD_MAX) plen = L4_PAYLOAD_MAX;
    if (plen) memcpy(payload, slot.payload, plen);
    tcp_out_head = (uint8_t)((tcp_out_head + 1) % L4_OUT_Q);
    tcp_out_count--;

    for (size_t i = 0; i < NAT_SLOTS; i++) {
      if (nat[i].proto == PROTO_TCP && nat[i].guest_ip == src &&
          nat[i].guest_port == sport && nat[i].remote_ip == dst &&
          nat[i].remote_port == dport) {
        e = &nat[i];
        break;
      }
    }
    portEXIT_CRITICAL(&mux);
    if (!e) continue;

    if (flags & TH_RST) {
      LOCK_TCPIP_CORE();
      if (e->pcb.tcp) {
        tcp_arg(e->pcb.tcp, nullptr);
        tcp_recv(e->pcb.tcp, nullptr);
        tcp_err(e->pcb.tcp, nullptr);
        tcp_abort(e->pcb.tcp);
        e->pcb.tcp = nullptr;
      }
      UNLOCK_TCPIP_CORE();
      portENTER_CRITICAL(&mux);
      memset(e, 0, sizeof(*e));
      portEXIT_CRITICAL(&mux);
      continue;
    }

    if ((flags & TH_SYN) && !(flags & TH_ACK)) {
      if (e->pcb.tcp) continue;
      LOCK_TCPIP_CORE();
      e->pcb.tcp = tcp_new();
      if (!e->pcb.tcp) {
        UNLOCK_TCPIP_CORE();
        portENTER_CRITICAL(&mux);
        memset(e, 0, sizeof(*e));
        portEXIT_CRITICAL(&mux);
        LOGE("eth_nat: tcp_new failed");
        continue;
      }
      tcp_arg(e->pcb.tcp, e);
      tcp_recv(e->pcb.tcp, tcp_recv_cb);
      tcp_err(e->pcb.tcp, tcp_err_cb);
      ip_addr_t dip;
      IP_ADDR4(&dip, (dst >> 24) & 0xff, (dst >> 16) & 0xff, (dst >> 8) & 0xff,
               dst & 0xff);
      err_t err = tcp_connect(e->pcb.tcp, &dip, dport, tcp_connected_cb);
      UNLOCK_TCPIP_CORE();
      if (err != ERR_OK && err != ERR_INPROGRESS) {
        LOGE("eth_nat: tcp_connect failed (%d)", (int)err);
        portENTER_CRITICAL(&mux);
        free_entry_locked(e);
        portEXIT_CRITICAL(&mux);
      } else {
        LOG("eth_nat: TCP SYN %u -> %u.%u.%u.%u:%u", (unsigned)sport,
            (unsigned)((dst >> 24) & 0xff), (unsigned)((dst >> 16) & 0xff),
            (unsigned)((dst >> 8) & 0xff), (unsigned)(dst & 0xff),
            (unsigned)dport);
      }
      continue;
    }

    if (!e->pcb.tcp || e->tcp_phase == TP_CONNECTING) continue;

    LOCK_TCPIP_CORE();
    if (plen > 0) {
      err_t err = tcp_write(e->pcb.tcp, payload, plen, TCP_WRITE_FLAG_COPY);
      if (err == ERR_OK) tcp_output(e->pcb.tcp);
      else LOGE("eth_nat: tcp_write failed (%d)", (int)err);
    }
    if (flags & TH_FIN) {
      tcp_shutdown(e->pcb.tcp, 0, 1);
    }
    UNLOCK_TCPIP_CORE();

    if (plen > 0)
      tcp_send_to_guest(e, TH_ACK, nullptr, 0, e->h_seq, e->g_seq);
    if (flags & TH_FIN) {
      tcp_send_to_guest(e, TH_FIN | TH_ACK, nullptr, 0, e->h_seq, e->g_seq);
      e->h_seq++;
      e->tcp_phase = TP_CLOSING;
    }
  }
}

// ---- ARP / local ICMP / dispatch ----

void handle_arp(const uint8_t* frame, size_t len) {
  if (len < 42) return;
  const uint8_t* arp = frame + 14;
  if (rd16be(arp + 0) != 1) return;
  if (rd16be(arp + 2) != ETHERTYPE_IP) return;
  if (arp[4] != 6 || arp[5] != 4) return;
  if (rd16be(arp + 6) != ARP_REQUEST) return;
  if (rd32be(arp + 24) != gateway_ip) return;

  uint8_t reply[MIN_FRAME];
  memset(reply, 0, sizeof(reply));
  memcpy(reply + 0, frame + 6, 6);
  memcpy(reply + 6, gateway_mac, 6);
  wr16be(reply + 12, ETHERTYPE_ARP);
  uint8_t* a = reply + 14;
  wr16be(a + 0, 1);
  wr16be(a + 2, ETHERTYPE_IP);
  a[4] = 6;
  a[5] = 4;
  wr16be(a + 6, ARP_REPLY);
  memcpy(a + 8, gateway_mac, 6);
  wr32be(a + 14, gateway_ip);
  memcpy(a + 18, frame + 6, 6);
  memcpy(a + 24, arp + 14, 4);
  if (enqueue_rx(reply, MIN_FRAME))
    LOG("eth_nat: ARP reply gateway");
}

// Synthesize ICMP echo reply as if from reply_src (gateway or STA hairpin).
void local_icmp_echo(const uint8_t* ip, size_t ihl, size_t ip_len,
                     const uint8_t* frame, uint32_t reply_src,
                     const char* why) {
  const uint8_t* icmp = ip + ihl;
  if (icmp[0] != 8) return;

  uint8_t reply[MAX_FRAME];
  memset(reply, 0, sizeof(reply));
  memcpy(reply + 0, frame + 6, 6);
  memcpy(reply + 6, gateway_mac, 6);
  wr16be(reply + 12, ETHERTYPE_IP);

  uint8_t* rip = reply + 14;
  memcpy(rip, ip, ip_len);
  wr32be(rip + 12, reply_src);
  wr32be(rip + 16, rd32be(ip + 12));
  rip[8] = 64;
  wr16be(rip + 10, 0);
  wr16be(rip + 10, inet_checksum(rip, ihl));

  uint8_t* ricmp = rip + ihl;
  ricmp[0] = 0;
  wr16be(ricmp + 2, 0);
  wr16be(ricmp + 2, inet_checksum(ricmp, ip_len - ihl));

  if (enqueue_rx(reply, 14 + ip_len))
    LOG("eth_nat: ICMP echo reply (%s) -> guest", why);
}

void handle_ip(const uint8_t* frame, size_t len) {
  if (len < 14 + 20) return;
  const uint8_t* ip = frame + 14;
  if ((ip[0] >> 4) != 4) return;
  const size_t ihl = (size_t)(ip[0] & 0x0f) * 4u;
  if (ihl < 20 || len < 14 + ihl) return;
  if (rd16be(ip + 6) & 0x3fffu) return;

  const uint16_t total = rd16be(ip + 2);
  if (total < ihl) return;
  size_t ip_len = total;
  if (14 + ip_len > len) ip_len = len - 14;
  if (ip_len > MAX_FRAME - 14) ip_len = MAX_FRAME - 14;

  const uint32_t src = rd32be(ip + 12);
  const uint32_t dst = rd32be(ip + 16);
  const uint8_t proto = ip[9];
  const uint8_t* l4 = ip + ihl;
  const size_t l4_len = ip_len - ihl;

  // Local gateway services.
  if (dst == gateway_ip) {
    if (proto == PROTO_ICMP && l4_len >= 8)
      local_icmp_echo(ip, ihl, ip_len, frame, gateway_ip, "gateway");
    return;
  }

  // Hairpin to the ESP STA address: do not inject onto WiFi (lwIP will not
  // reliably bounce ICMP echo back into our raw PCB).
  if (sta_ip != 0 && dst == sta_ip) {
    if (proto == PROTO_ICMP && l4_len >= 8)
      local_icmp_echo(ip, ihl, ip_len, frame, sta_ip, "sta");
    return;
  }

  // Other on-link addresses: no peer (guest is alone on 10.11.0/24).
  if (on_link(dst)) return;

  // Off-link: NAPT via STA (queue only — lwIP on net_task).
  if (proto == PROTO_UDP)
    nat_udp_out(src, dst, l4, l4_len);
  else if (proto == PROTO_TCP)
    nat_tcp_out(src, dst, l4, l4_len);
  else if (proto == PROTO_ICMP) {
    if (nat_icmp_out(src, dst, ip, ihl, ip_len))
      LOG("eth_nat: ICMP echo fwd %u.%u.%u.%u",
          (unsigned)((dst >> 24) & 0xff), (unsigned)((dst >> 16) & 0xff),
          (unsigned)((dst >> 8) & 0xff), (unsigned)(dst & 0xff));
  }
}

}  // namespace

void reset() {
  // PDP-core path: never call lwIP here. Snapshot PCBs/pings, clear tables,
  // and let net_task free them (avoids LOCK_TCPIP_CORE during cold_boot).
  portENTER_CRITICAL(&mux);
  rx_head = rx_tail = rx_count = 0;
  rx_drops = 0;
  icmp_out_head = icmp_out_tail = icmp_out_count = 0;
  udp_out_head = udp_out_tail = udp_out_count = 0;
  tcp_out_head = tcp_out_tail = tcp_out_count = 0;
  icmp_active_ping = nullptr;
  icmp_active_entry = nullptr;

  // Stash live PCBs into the entry slots' union while clearing proto so
  // flush_nat_pcbs can walk and free them. Use a side table instead.
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    NatEntry& e = nat[i];
    if (e.proto == PROTO_UDP && e.pcb.udp) {
      // Keep pointer; mark for flush via proto==0xff sentinel in tcp_phase.
      e.tcp_phase = 0xff;
      e.proto = 0;  // hidden from lookups
      // pcb.udp still set
      e.ping = nullptr;
      continue;
    }
    if (e.proto == PROTO_TCP && e.pcb.tcp) {
      e.tcp_phase = 0xfe;
      e.proto = 0;
      e.ping = nullptr;
      continue;
    }
    if (e.proto == PROTO_ICMP && e.ping) {
      e.tcp_phase = 0xfd;
      // keep ping handle in e.ping
      e.proto = 0;
      e.pcb.udp = nullptr;
      continue;
    }
    memset(&e, 0, sizeof(e));
  }
  nat_flush_requested = true;
  portEXIT_CRITICAL(&mux);
}

void flush_nat_pcbs() {
  struct udp_pcb* udps[NAT_SLOTS];
  struct tcp_pcb* tcps[NAT_SLOTS];
  esp_ping_handle_t pings[NAT_SLOTS];
  size_t nu = 0, nt = 0, np = 0;

  portENTER_CRITICAL(&mux);
  if (!nat_flush_requested) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  nat_flush_requested = false;
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    NatEntry& e = nat[i];
    if (e.tcp_phase == 0xff && e.pcb.udp) {
      udps[nu++] = e.pcb.udp;
    } else if (e.tcp_phase == 0xfe && e.pcb.tcp) {
      tcps[nt++] = e.pcb.tcp;
    } else if (e.tcp_phase == 0xfd && e.ping) {
      pings[np++] = e.ping;
    }
    memset(&e, 0, sizeof(e));
  }
  portEXIT_CRITICAL(&mux);

  for (size_t i = 0; i < nu; i++) free_pcb(PROTO_UDP, udps[i]);
  for (size_t i = 0; i < nt; i++) free_pcb(PROTO_TCP, tcps[i]);
  for (size_t i = 0; i < np; i++) stop_ping(pings[i]);
}

void set_guest_mac(const uint8_t mac[6]) {
  if (mac) memcpy(guest_mac, mac, 6);
}

void set_gateway_mac(const uint8_t mac[6]) {
  if (mac) memcpy(gateway_mac, mac, 6);
}

void set_addresses(uint32_t guest_ip_host, uint32_t guest_mask_host,
                   uint32_t gateway_ip_host) {
  guest_ip = guest_ip_host;
  guest_mask = guest_mask_host;
  gateway_ip = gateway_ip_host;
}

void set_sta_ip(uint32_t sta_ip_host) {
  if (sta_ip != sta_ip_host) {
    sta_ip = sta_ip_host;
    if (sta_ip) {
      LOG("eth_nat: STA IP %u.%u.%u.%u",
          (unsigned)((sta_ip >> 24) & 0xff),
          (unsigned)((sta_ip >> 16) & 0xff),
          (unsigned)((sta_ip >> 8) & 0xff),
          (unsigned)(sta_ip & 0xff));
    }
  }
}

void on_guest_tx(const uint8_t* frame, size_t len) {
  if (!frame || len < 14) return;
  // Keep L2 dest for replies aligned with the MAC the guest actually uses.
  memcpy(guest_mac, frame + 6, 6);
  const uint16_t etype = rd16be(frame + 12);
  if (etype == ETHERTYPE_ARP) {
    handle_arp(frame, len);
    return;
  }
  if (etype == ETHERTYPE_IP) {
    handle_ip(frame, len);
    return;
  }
}

void expire_nat_entries() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  for (size_t i = 0; i < NAT_SLOTS; i++) {
    if (nat[i].proto == 0) continue;
    // Never expire an in-flight esp_ping — callback still holds e*.
    if (nat[i].proto == PROTO_ICMP && nat[i].ping) continue;
    uint32_t idle = UDP_IDLE_MS;
    if (nat[i].proto == PROTO_TCP) idle = TCP_IDLE_MS;
    else if (nat[i].proto == PROTO_ICMP) idle = ICMP_IDLE_MS;
    if ((uint32_t)(now - nat[i].last_ms) > idle)
      free_entry_locked(&nat[i]);
  }
  portEXIT_CRITICAL(&mux);
}

void tick() {
  // PDP path: no lwIP here (PCB free must run on net_task).
}

void host_poll() {
  flush_nat_pcbs();
  drain_icmp_out_queue();
  drain_udp_out_queue();
  drain_tcp_out_queue();
  expire_nat_entries();
}

bool pop_rx(uint8_t* out, size_t* out_len, size_t out_cap) {
  if (!out || !out_len) return false;
  portENTER_CRITICAL(&mux);
  if (rx_count == 0) {
    portEXIT_CRITICAL(&mux);
    return false;
  }
  const RxSlot& s = rxq[rx_head];
  size_t n = s.len;
  if (n > out_cap) n = out_cap;
  memcpy(out, s.data, n);
  *out_len = n;
  rx_head = (uint8_t)((rx_head + 1) % RX_QUEUE);
  rx_count--;
  portEXIT_CRITICAL(&mux);
  return true;
}

size_t rx_pending() {
  portENTER_CRITICAL(&mux);
  size_t n = rx_count;
  portEXIT_CRITICAL(&mux);
  return n;
}

}  // namespace eth_nat
