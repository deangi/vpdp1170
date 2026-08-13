#include "telnet_pipe.h"
#include "platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void TelnetPipe::init(uint8_t* out_storage, size_t out_size,
                      uint8_t* in_storage, size_t in_size,
                      uint8_t* diag_storage, size_t diag_size) {
  out_storage_ = out_storage;
  out_size_ = out_size;
  in_storage_ = in_storage;
  in_size_ = in_size;
  diag_storage_ = diag_storage;
  diag_size_ = diag_size;
  fifos_inited_ = false;
}

void TelnetPipe::set_hooks(const Hooks& h) {
  hooks_ = h;
}

void TelnetPipe::ensure_fifos() {
  if (fifos_inited_) return;
  if (out_storage_ && out_size_) out_.init(out_storage_, out_size_);
  if (in_storage_ && in_size_) in_.init(in_storage_, in_size_);
  if (has_diag()) diag_.init(diag_storage_, diag_size_);
  fifos_inited_ = true;
  LOG("%s FIFO: %u KB out",
      hooks_.log_name ? hooks_.log_name : "telnet",
      (unsigned)(out_size_ / 1024));
}

void TelnetPipe::reset_rx_parser() {
  rx_state_ = RX_DATA;
  rx_after_cr_ = false;
}

void TelnetPipe::begin(uint16_t port, bool enabled) {
  ensure_fifos();
  enabled_ = enabled;
  port_ = port;
  const char* name = hooks_.log_name ? hooks_.log_name : "telnet";
  if (!enabled) {
    LOG("%s: disabled in config", name);
    return;
  }
  if (port == 0) {
    LOG("%s: port 0 — not starting", name);
    enabled_ = false;
    return;
  }
  server_ = WiFiServer(port);
  server_.begin();
  server_.setNoDelay(true);
  started_ = true;
  LOG("%s: listening on port %u", name, port);
}

void TelnetPipe::send_iac(uint8_t verb, uint8_t opt) {
  uint8_t b[3] = { T_IAC, verb, opt };
  client_.write(b, 3);
}

void TelnetPipe::on_connect() {
  reset_rx_parser();
  IPAddress ip = client_.remoteIP();
  strncpy(client_ip_, ip.toString().c_str(), sizeof(client_ip_) - 1);
  client_ip_[sizeof(client_ip_) - 1] = 0;
  const char* name = hooks_.log_name ? hooks_.log_name : "telnet";
  LOG("%s: client connected from %s", name, client_ip_);
  send_iac(T_WILL, OPT_ECHO);
  send_iac(T_WILL, OPT_SGA);
  send_iac(T_WONT, OPT_LINEMODE);
  send_iac(T_DO,   OPT_BINARY);
  out_.clear();
  if (has_diag()) diag_.clear();
  dropped_.store(0, std::memory_order_relaxed);
}

void TelnetPipe::deliver_rx(uint8_t c) {
  if (hooks_.on_rx) {
    hooks_.on_rx(c, hooks_.rx_ctx);
    return;
  }
  in_push(c);
}

bool TelnetPipe::in_push(uint8_t c) {
  return in_.push(c);
}

void TelnetPipe::drain_rx() {
  while (client_.available()) {
    int ch = client_.read();
    if (ch < 0) break;
    uint8_t c = (uint8_t)ch;
    switch (rx_state_) {
      case RX_DATA:
        if (c == T_IAC) {
          rx_state_ = RX_IAC;
          break;
        }
        if (rx_after_cr_ && (c == 0x00 || c == 0x0A)) {
          rx_after_cr_ = false;
          break;
        }
        rx_after_cr_ = false;
        deliver_rx(c);
        if (c == 0x0D) rx_after_cr_ = true;
        break;
      case RX_IAC:
        if (c == T_IAC) {
          deliver_rx(T_IAC);
          rx_state_ = RX_DATA;
        } else if (c == T_SB) {
          rx_state_ = RX_SUBNEG;
        } else if (c == T_WILL || c == T_WONT ||
                   c == T_DO   || c == T_DONT) {
          rx_state_ = RX_IAC_OPTION;
        } else {
          rx_state_ = RX_DATA;
        }
        break;
      case RX_IAC_OPTION:
        rx_state_ = RX_DATA;
        break;
      case RX_SUBNEG:
        if (c == T_IAC) rx_state_ = RX_SUBNEG_IAC;
        break;
      case RX_SUBNEG_IAC:
        rx_state_ = (c == T_SE) ? RX_DATA : RX_SUBNEG;
        break;
    }
  }
}

void TelnetPipe::drain_fifo(Fifo& fifo) {
  const uint8_t* p;
  size_t n;
  while ((n = fifo.peek(&p)) > 0) {
    size_t w = client_.write(p, n);
    if (w == 0) break;
    fifo.consume(w);
    if (w < n) break;
  }
}

void TelnetPipe::poll() {
  ensure_fifos();
  poll_seen_.store(true, std::memory_order_release);
  if (guest_reset_requested_.load(std::memory_order_acquire)) {
    in_.clear();
    out_.clear();
    const bool shell = hooks_.shell_active && hooks_.shell_active(hooks_.shell_ctx);
    if (!shell) {
      while (client_ && client_.available() > 0)
        client_.read();
    }
    reset_rx_parser();
    dropped_.store(0, std::memory_order_relaxed);
    guest_reset_requested_.store(false, std::memory_order_release);
  }
  if (!started_) {
    out_.clear();
    if (has_diag()) diag_.clear();
    return;
  }

  if (server_.hasClient()) {
    WiFiClient nc = server_.available();
    if (client_ && client_.connected()) {
      if (hooks_.busy_msg) nc.print(hooks_.busy_msg);
      nc.stop();
    } else {
      client_ = nc;
      client_.setNoDelay(true);
      on_connect();
    }
  }

  if (client_ && client_.connected()) {
    drain_rx();
    if (hooks_.after_rx) hooks_.after_rx(hooks_.after_rx_ctx);
    if (hooks_.aux_peek && hooks_.aux_consume) {
      const uint8_t* data;
      size_t bytes;
      while ((bytes = hooks_.aux_peek(&data, hooks_.aux_ctx)) > 0) {
        size_t written = client_.write(data, bytes);
        if (written == 0) break;
        hooks_.aux_consume(written, hooks_.aux_ctx);
        if (written < bytes) break;
      }
    }
    if (has_diag()) drain_fifo(diag_);
    if (hooks_.shell_active && hooks_.shell_active(hooks_.shell_ctx))
      out_.clear();
    else
      drain_fifo(out_);
  } else if (client_) {
    client_.stop();
    if (hooks_.on_disconnect) hooks_.on_disconnect(hooks_.disc_ctx);
    reset_rx_parser();
    client_ip_[0] = 0;
    out_.clear();
    if (has_diag()) diag_.clear();
    const char* name = hooks_.log_name ? hooks_.log_name : "telnet";
    LOG("%s: client disconnected", name);
  } else {
    out_.clear();
    if (has_diag()) diag_.clear();
  }
}

void TelnetPipe::reset_guest_io() {
  ensure_fifos();
  if (poll_seen_.load(std::memory_order_acquire)) {
    guest_reset_requested_.store(true, std::memory_order_release);
    while (guest_reset_requested_.load(std::memory_order_acquire))
      vTaskDelay(1);
  } else {
    in_.clear();
    out_.clear();
    const bool shell = hooks_.shell_active && hooks_.shell_active(hooks_.shell_ctx);
    if (!shell) {
      while (client_ && client_.available() > 0)
        client_.read();
    }
    reset_rx_parser();
    dropped_.store(0, std::memory_order_relaxed);
  }
}

void TelnetPipe::write(uint8_t c) {
  if (!out_.push(c)) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (c == T_IAC && !out_.push(T_IAC))
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

void TelnetPipe::diag_write(uint8_t c) {
  if (!has_diag()) return;
  if (!diag_.push(c)) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (c == T_IAC && !diag_.push(T_IAC))
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

bool TelnetPipe::in_pop(uint8_t* out) {
  return in_.pop(out);
}

bool TelnetPipe::in_available() const {
  return !in_.empty();
}

void TelnetPipe::in_clear() { in_.clear(); }
void TelnetPipe::out_clear() { out_.clear(); }
void TelnetPipe::diag_clear() { if (has_diag()) diag_.clear(); }

bool TelnetPipe::connected() {
  return (bool)client_ && client_.connected();
}

bool TelnetPipe::listening() const {
  return started_ && WiFi.status() == WL_CONNECTED;
}

size_t TelnetPipe::out_count() const { return out_.count(); }
size_t TelnetPipe::out_free() const { return out_.free_space(); }
