#include "kek_deuna.h"

#include "eth_nat.h"
#include "platform.h"
#include "_upstream_kek/bus.h"

#include <string.h>

namespace kek_deuna {

namespace {

constexpr uint16_t PCSR0_SERI = 0100000;
constexpr uint16_t PCSR0_PCEI = 0040000;
constexpr uint16_t PCSR0_RXI  = 0020000;
constexpr uint16_t PCSR0_TXI  = 0010000;
constexpr uint16_t PCSR0_DNI  = 0004000;
constexpr uint16_t PCSR0_RCBI = 0002000;
constexpr uint16_t PCSR0_FATL = 0001000;
constexpr uint16_t PCSR0_USCI = 0000400;
constexpr uint16_t PCSR0_INTR = 0000200;
constexpr uint16_t PCSR0_INTE = 0000100;
constexpr uint16_t PCSR0_RSET = 0000040;
constexpr uint16_t PCSR0_PCMD = 0000017;

constexpr uint16_t CMD_NOOP     = 000;
constexpr uint16_t CMD_GETPCBB  = 001;
constexpr uint16_t CMD_GETCMD   = 002;
constexpr uint16_t CMD_SELFTEST = 003;
constexpr uint16_t CMD_START    = 004;
constexpr uint16_t CMD_BOOT     = 005;
constexpr uint16_t CMD_PDMD     = 010;
constexpr uint16_t CMD_HALT     = 016;
constexpr uint16_t CMD_STOP     = 017;

constexpr uint16_t PCSR1_STATE   = 0000017;
constexpr uint16_t TYPE_DEUNA    = 0000000;
constexpr uint16_t STATE_READY   = 002;
constexpr uint16_t STATE_RUNNING = 003;
constexpr uint16_t STATE_HALT    = 010;

constexpr uint16_t FC_NOOP    = 000;
constexpr uint16_t FC_RDPA    = 002;
constexpr uint16_t FC_RPA     = 004;
constexpr uint16_t FC_WPA     = 005;
constexpr uint16_t FC_RRF     = 010;
constexpr uint16_t FC_WRF     = 011;
constexpr uint16_t FC_RDCTR   = 012;
constexpr uint16_t FC_RDCLCTR = 013;
constexpr uint16_t FC_RMODE   = 014;
constexpr uint16_t FC_WMODE   = 015;
constexpr uint16_t FC_RSTAT   = 016;
constexpr uint16_t FC_RCSTAT  = 017;

// Descriptor word 2 flags (SIMH / hardware). Note RX vs TX reuse the same
// bit positions differently: 0020000 is TX MTCH but RX FRAM.
constexpr uint16_t DESC_OWN  = 0100000;
constexpr uint16_t DESC_ERRS = 0040000;
constexpr uint16_t DESC_FRAM = 0020000;  // RX framing error (not TX MTCH!)
constexpr uint16_t DESC_OFLO = 0010000;
constexpr uint16_t DESC_CRC  = 0004000;
constexpr uint16_t DESC_STF  = 0001000;
constexpr uint16_t DESC_ENF  = 0000400;
constexpr uint16_t RXR_MLEN  = 0007777;

constexpr size_t FRAME_MAX = eth_nat::MAX_FRAME;

bool     g_enabled = false;
bus*     g_bus     = nullptr;
uint8_t  g_mac[6]  = { 0x08, 0x00, 0x2B, 0x11, 0x70, 0x01 };

uint16_t pcsr0 = 0;
uint16_t pcsr1 = TYPE_DEUNA | STATE_READY;
uint16_t pcsr2 = 0;
uint16_t pcsr3 = 0;

uint32_t pcbb  = 0;
uint32_t mode  = 0;
uint16_t estat = 0000040;

uint32_t tdrb  = 0;
uint32_t telen = 4;
uint32_t trlen = 0;
uint32_t txnext = 0;
uint32_t rdrb  = 0;
uint32_t relen = 4;
uint32_t rrlen = 0;
uint32_t rxnext = 0;

bool irq_pending = false;

uint8_t tx_accum[FRAME_MAX];
size_t  tx_accum_len = 0;

void update_intr() {
  if (pcsr0 & 0xFF00)
    pcsr0 |= PCSR0_INTR;
  else
    pcsr0 &= (uint16_t)~PCSR0_INTR;
  irq_pending = (pcsr0 & PCSR0_INTE) && (pcsr0 & 0xFF00);
}

uint16_t state() { return (uint16_t)(pcsr1 & PCSR1_STATE); }

void set_state(uint16_t st) {
  pcsr1 = (uint16_t)(TYPE_DEUNA | (st & PCSR1_STATE));
}

uint32_t udb_from_pcb(const uint16_t pcb[4]) {
  return ((uint32_t)(pcb[2] & 3) << 16) | (uint32_t)pcb[1];
}

bool read_pcb(uint16_t out[4]) {
  if (!g_bus || !pcbb) return false;
  for (int i = 0; i < 4; i++)
    out[i] = g_bus->read_unibus_word(pcbb + (uint32_t)i * 2u);
  return true;
}

void write_bytes(uint32_t addr, const uint8_t* data, size_t n) {
  if (!g_bus) return;
  for (size_t i = 0; i < n; i++)
    g_bus->write_unibus_byte(addr + (uint32_t)i, data[i]);
}

void read_bytes(uint32_t addr, uint8_t* data, size_t n) {
  if (!g_bus) return;
  for (size_t i = 0; i < n; i++)
    data[i] = g_bus->read_unibus_byte(addr + (uint32_t)i);
}

void write_words(uint32_t addr, const uint16_t* w, int n) {
  if (!g_bus) return;
  for (int i = 0; i < n; i++)
    g_bus->write_unibus_word(addr + (uint32_t)i * 2u, w[i]);
}

void read_words(uint32_t addr, uint16_t* w, int n) {
  if (!g_bus) return;
  for (int i = 0; i < n; i++)
    w[i] = g_bus->read_unibus_word(addr + (uint32_t)i * 2u);
}

// Process owned TX descriptors: assemble frames, hand to eth_nat, clear OWN.
bool process_transmit() {
  if (!g_bus || trlen == 0 || telen < 4 || tdrb == 0) return false;
  bool any = false;

  for (;;) {
    const uint32_t ba = tdrb + (telen * 2u) * txnext;
    uint16_t hdr[4];
    read_words(ba, hdr, 4);
    if (!(hdr[2] & DESC_OWN)) break;

    const uint16_t slen = hdr[0];
    const uint32_t segb =
        (uint32_t)hdr[1] + (((uint32_t)hdr[2] & 3u) << 16);

    if (hdr[2] & DESC_STF) tx_accum_len = 0;

    if (slen > 0 && tx_accum_len < FRAME_MAX) {
      size_t n = slen;
      if (tx_accum_len + n > FRAME_MAX) n = FRAME_MAX - tx_accum_len;
      read_bytes(segb, tx_accum + tx_accum_len, n);
      tx_accum_len += n;
    }

    if (hdr[2] & DESC_ENF) {
      // Hardware inserts source MAC (DEUNA UG §4.7).
      if (tx_accum_len >= 12)
        memcpy(tx_accum + 6, g_mac, 6);
      if (tx_accum_len < eth_nat::MIN_FRAME && tx_accum_len >= 14) {
        memset(tx_accum + tx_accum_len, 0,
               eth_nat::MIN_FRAME - tx_accum_len);
        tx_accum_len = eth_nat::MIN_FRAME;
      }
      eth_nat::on_guest_tx(tx_accum, tx_accum_len);
      pcsr0 |= PCSR0_TXI;
      tx_accum_len = 0;
    }

    hdr[2] &= (uint16_t)~DESC_OWN;
    write_words(ba, hdr, 4);
    any = true;
    txnext++;
    if (txnext >= trlen) txnext = 0;
  }
  return any;
}

// Drain eth_nat RX queue into owned RX descriptors.
// Cap frames per call so we make progress on TCP without stuffing the
// whole queue before the guest runs deintr.
bool process_receive() {
  if (!g_bus || rrlen == 0 || relen < 4 || rdrb == 0) return false;
  if (state() != STATE_RUNNING) return false;
  if (pcsr0 & PCSR0_RCBI) return false;

  bool any = false;
  for (int n = 0; n < 4 && eth_nat::rx_pending() > 0; n++) {
    const uint32_t ba = rdrb + (relen * 2u) * rxnext;
    uint16_t hdr[4];
    read_words(ba, hdr, 4);
    if (!(hdr[2] & DESC_OWN)) break;

    uint8_t frame[FRAME_MAX];
    size_t flen = 0;
    if (!eth_nat::pop_rx(frame, &flen, sizeof(frame))) break;

    const uint16_t buflen = hdr[0];
    const uint32_t segb =
        (uint32_t)hdr[1] + (((uint32_t)hdr[2] & 3u) << 16);
    size_t wlen = flen;
    if (buflen > 0 && wlen > buflen) wlen = buflen;

    if (wlen >= 14 + 20) {
      const uint16_t etype =
          (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
      if (etype == 0x0800 && (frame[14] & 0xf0) == 0x40) {
        const size_t ihl = (size_t)(frame[14] & 0x0f) * 4u;
        if (ihl >= 20 && wlen >= 14 + ihl) {
          const uint16_t iptot =
              (uint16_t)(((uint16_t)frame[16] << 8) | frame[17]);
          const size_t max_ip = wlen - 14;
          if (iptot > max_ip) {
            frame[16] = (uint8_t)(max_ip >> 8);
            frame[17] = (uint8_t)(max_ip & 0xff);
            frame[24] = 0;
            frame[25] = 0;
            uint32_t sum = 0;
            for (size_t i = 0; i + 1 < ihl; i += 2)
              sum += (uint16_t)(((uint16_t)frame[14 + i] << 8) |
                                frame[15 + i]);
            if (ihl & 1) sum += (uint16_t)frame[14 + ihl - 1] << 8;
            while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
            const uint16_t csum = (uint16_t)~sum;
            frame[24] = (uint8_t)(csum >> 8);
            frame[25] = (uint8_t)(csum & 0xff);
          }
        }
      }
    }

    write_bytes(segb, frame, wlen);

    hdr[2] &= (uint16_t)~(DESC_OWN | DESC_ERRS | DESC_FRAM | DESC_OFLO |
                          DESC_CRC | DESC_STF | DESC_ENF);
    hdr[2] |= (uint16_t)(DESC_STF | DESC_ENF);
    const uint16_t mlen = (uint16_t)(wlen + 4u);
    hdr[3] = (uint16_t)(mlen & RXR_MLEN);
    write_words(ba, hdr, 4);

    pcsr0 |= PCSR0_RXI;
    any = true;
    rxnext++;
    if (rxnext >= rrlen) rxnext = 0;
  }
  return any;
}

uint16_t do_getcmd() {
  uint16_t pcb[4] = {};
  if (!read_pcb(pcb)) return PCSR0_PCEI;

  const uint16_t fc = pcb[0] & 037;
  switch (fc) {
    case FC_NOOP:
      return 0;
    case FC_RDPA:
    case FC_RPA:
      write_bytes(pcbb + 2, g_mac, 6);
      return 0;
    case FC_WPA:
      if (pcb[1] & 1) return PCSR0_PCEI;
      read_bytes(pcbb + 2, g_mac, 6);
      eth_nat::set_guest_mac(g_mac);
      return 0;
    case FC_RMODE: {
      uint16_t v = (uint16_t)(mode & 0177777);
      write_words(pcbb + 2, &v, 1);
      return 0;
    }
    case FC_WMODE:
      mode = pcb[1];
      return 0;
    case FC_RSTAT:
    case FC_RCSTAT: {
      uint16_t v0 = estat, v1 = 10, v2 = 32;
      write_words(pcbb + 2, &v0, 1);
      write_words(pcbb + 4, &v1, 1);
      write_words(pcbb + 6, &v2, 1);
      if (fc == FC_RCSTAT) estat &= 0377;
      return 0;
    }
    case FC_RRF: {
      if ((pcb[1] & 1) || (pcb[2] & 0374)) return PCSR0_PCEI;
      uint16_t udb[6];
      udb[0] = (uint16_t)(tdrb & 0177776);
      udb[1] = (uint16_t)(((telen & 0377) << 8) | ((tdrb >> 16) & 3));
      udb[2] = (uint16_t)(trlen & 0177777);
      udb[3] = (uint16_t)(rdrb & 0177776);
      udb[4] = (uint16_t)(((relen & 0377) << 8) | ((rdrb >> 16) & 3));
      udb[5] = (uint16_t)(rrlen & 0177777);
      write_words(udb_from_pcb(pcb), udb, 6);
      return 0;
    }
    case FC_WRF: {
      if ((pcb[1] & 1) || (pcb[2] & 0374)) return PCSR0_PCEI;
      if (state() == STATE_RUNNING) return PCSR0_PCEI;
      uint16_t udb[6] = {};
      read_words(udb_from_pcb(pcb), udb, 6);
      if ((udb[0] & 1) || (udb[1] & 0374) || (udb[3] & 1) || (udb[4] & 0374) ||
          (udb[5] < 2))
        return PCSR0_PCEI;
      tdrb  = ((uint32_t)(udb[1] & 3) << 16) | (uint32_t)(udb[0] & 0177776);
      telen = (udb[1] >> 8) & 0377;
      trlen = udb[2];
      rdrb  = ((uint32_t)(udb[4] & 3) << 16) | (uint32_t)(udb[3] & 0177776);
      relen = (udb[4] >> 8) & 0377;
      rrlen = udb[5];
      txnext = rxnext = 0;
      return 0;
    }
    case FC_RDCTR:
    case FC_RDCLCTR: {
      uint16_t zeros[34] = {};
      zeros[0] = 68;
      write_words(udb_from_pcb(pcb), zeros, 34);
      return 0;
    }
    default:
      LOG("DEUNA: ancillary fc=%o not implemented", (unsigned)fc);
      return PCSR0_PCEI;
  }
}

void port_command() {
  const uint16_t cmd = pcsr0 & PCSR0_PCMD;
  const uint16_t st  = state();

  switch (cmd) {
    case CMD_PDMD:
      if (process_transmit()) { /* TXI already set inside */ }
      process_receive();
      pcsr0 |= PCSR0_DNI;
      break;
    case CMD_GETCMD:
      pcsr0 |= do_getcmd();
      pcsr0 |= PCSR0_DNI;
      break;
    case CMD_GETPCBB:
      pcbb = ((uint32_t)(pcsr3 & 3) << 16) | (uint32_t)(pcsr2 & 0177776);
      pcsr0 |= PCSR0_DNI;
      break;
    case CMD_SELFTEST:
      pcsr0 |= PCSR0_DNI;
      pcsr0 &= (uint16_t)~(PCSR0_USCI | PCSR0_FATL);
      set_state(STATE_READY);
      break;
    case CMD_START:
      if (st == STATE_READY) {
        set_state(STATE_RUNNING);
        pcsr0 |= PCSR0_DNI;
        txnext = rxnext = 0;
        tx_accum_len = 0;
      } else {
        pcsr0 |= PCSR0_PCEI;
      }
      break;
    case CMD_HALT:
      if (st == STATE_READY || st == STATE_RUNNING) {
        set_state(STATE_HALT);
        pcsr0 |= PCSR0_DNI;
      } else {
        pcsr0 |= PCSR0_PCEI;
      }
      break;
    case CMD_STOP:
      if (st == STATE_RUNNING) {
        set_state(STATE_READY);
        pcsr0 |= PCSR0_DNI;
      } else {
        pcsr0 |= PCSR0_PCEI;
      }
      break;
    case CMD_BOOT:
      pcsr0 |= PCSR0_PCEI;
      break;
    case CMD_NOOP:
      break;
    default:
      pcsr0 |= PCSR0_DNI;
      break;
  }
  update_intr();
}

void write_pcsr0(uint16_t data, bool byte_hi_only) {
  if (byte_hi_only) {
    pcsr0 &= (uint16_t)~((data << 8) & 0177400);
    update_intr();
    return;
  }

  pcsr0 &= (uint16_t)~(data & 0xFF00);

  if (data & PCSR0_RSET) {
    reset();
    update_intr();
    return;
  }

  if ((pcsr0 ^ data) & PCSR0_INTE) {
    pcsr0 ^= PCSR0_INTE;
    pcsr0 |= PCSR0_DNI;
    update_intr();
    return;
  }

  pcsr0 = (uint16_t)((pcsr0 & (uint16_t)~PCSR0_PCMD) | (data & PCSR0_PCMD));
  port_command();
}

}  // namespace

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }
void set_bus(bus* b) { g_bus = b; }

void set_mac(const uint8_t mac[6]) {
  if (!mac) return;
  memcpy(g_mac, mac, 6);
  eth_nat::set_guest_mac(g_mac);
}

void get_mac(uint8_t mac[6]) {
  if (!mac) return;
  memcpy(mac, g_mac, 6);
}

void set_network(uint32_t guest_ip, uint32_t guest_mask, uint32_t gateway_ip) {
  eth_nat::set_addresses(guest_ip, guest_mask, gateway_ip);
}

bool contains(uint16_t addr) {
  if (!g_enabled) return false;
  addr &= 0177776;
  return addr >= BASE_ADDR && addr < END_ADDR;
}

void reset() {
  // SIMH xu_sw_reset: RSET completion is signaled with DNI (+ INTR mirror).
  // 2.11BSD deattach spins in dewait() on PCSR0_INTR after board reset.
  pcsr0 = PCSR0_DNI;
  set_state(STATE_READY);
  pcsr2 = 0;
  pcsr3 = 0;
  pcbb = 0;
  mode = 0;
  estat = 0000040;
  tdrb = rdrb = 0;
  telen = relen = 4;
  trlen = rrlen = 0;
  txnext = rxnext = 0;
  tx_accum_len = 0;
  irq_pending = false;
  eth_nat::reset();
  eth_nat::set_guest_mac(g_mac);
  update_intr();
}

void tick() {
  if (!g_enabled || state() != STATE_RUNNING) {
    eth_nat::tick();
    return;
  }
  // RX only here. TX is driven by guest PDMD (real DEUNA behaviour) —
  // polling TX every tick can re-enter the same descriptor path around
  // destart/PDMD and double-enqueue ICMP NAT work (guest sees DUP!).
  bool dirty = false;
  if (process_receive()) dirty = true;
  if (dirty) update_intr();
  eth_nat::tick();
}

bool take_interrupt() {
  if (!g_enabled) return false;
  // Recompute from CSR: irq_pending is edge-latched for the host poll, but
  // must refresh so uncleared RXI/TXI/DNI can re-assert after a missed take.
  update_intr();
  if (!irq_pending) return false;
  irq_pending = false;
  return true;
}

uint16_t read_word(uint16_t addr) {
  if (!g_enabled) return 0;
  switch (addr & 0177776) {
    case BASE_ADDR + 0: return pcsr0;
    case BASE_ADDR + 2: return pcsr1;
    case BASE_ADDR + 4: return pcsr2;
    case BASE_ADDR + 6: return pcsr3;
    default: return 0;
  }
}

uint8_t read_byte(uint16_t addr) {
  uint16_t w = read_word(addr & 0177776);
  return (addr & 1) ? (uint8_t)(w >> 8) : (uint8_t)(w & 0xff);
}

void write_word(uint16_t addr, uint16_t value) {
  if (!g_enabled) return;
  switch (addr & 0177776) {
    case BASE_ADDR + 0:
      write_pcsr0(value, false);
      break;
    case BASE_ADDR + 2:
      break;
    case BASE_ADDR + 4:
      pcsr2 = value & 0177776;
      break;
    case BASE_ADDR + 6:
      pcsr3 = value & 0000003;
      break;
    default:
      break;
  }
}

void write_byte(uint16_t addr, uint8_t value) {
  if (!g_enabled) return;
  if ((addr & 0177776) == BASE_ADDR) {
    if (addr & 1)
      write_pcsr0(value, true);
    else
      write_pcsr0(value, false);
    return;
  }
  uint16_t a = addr & 0177776;
  uint16_t w = read_word(a);
  if (addr & 1)
    w = (uint16_t)((w & 0x00ff) | ((uint16_t)value << 8));
  else
    w = (uint16_t)((w & 0xff00) | value);
  write_word(a, w);
}

}  // namespace kek_deuna
