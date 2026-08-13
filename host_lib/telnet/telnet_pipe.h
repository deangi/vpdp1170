#pragma once
#include <stddef.h>
#include <stdint.h>
#include <WiFi.h>
#include <atomic>
#include "../util/fifo.h"

// Single-client Telnet listener: IAC negotiation, RX parser, guest I/O FIFOs.
// Guest adapters (KL11, DZ11, ADM-3A UART) stay outside this class.

class TelnetPipe {
public:
  struct Hooks {
    // After IAC/CR-NUL stripping. If null, bytes go to the input FIFO.
    void (*on_rx)(uint8_t c, void* ctx) = nullptr;
    void* rx_ctx = nullptr;

    void (*after_rx)(void* ctx) = nullptr;
    void* after_rx_ctx = nullptr;

    void (*on_disconnect)(void* ctx) = nullptr;
    void* disc_ctx = nullptr;

    size_t (*aux_peek)(const uint8_t** out, void* ctx) = nullptr;
    void (*aux_consume)(size_t n, void* ctx) = nullptr;
    void* aux_ctx = nullptr;

    // When true, guest TX is discarded (management shell owns the socket)
    // and guest reset must not drain pending socket RX.
    bool (*shell_active)(void* ctx) = nullptr;
    void* shell_ctx = nullptr;

    const char* busy_msg = "console already in use\r\n";
    const char* log_name = "telnet";
  };

  void init(uint8_t* out_storage, size_t out_size,
            uint8_t* in_storage, size_t in_size,
            uint8_t* diag_storage = nullptr, size_t diag_size = 0);
  void set_hooks(const Hooks& h);

  void begin(uint16_t port, bool enabled);
  void poll();

  void write(uint8_t c);
  void diag_write(uint8_t c);
  bool in_push(uint8_t c);
  bool in_pop(uint8_t* out);
  bool in_available() const;

  void in_clear();
  void out_clear();
  void diag_clear();
  void reset_rx_parser();
  void reset_guest_io();

  bool connected();
  bool started() const { return started_; }
  bool listening() const;
  const char* client_ip() const { return client_ip_; }
  uint16_t port() const { return port_; }
  bool enabled() const { return enabled_; }

  size_t out_count() const;
  size_t out_free() const;
  uint32_t dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  WiFiClient& socket() { return client_; }

private:
  enum RxState : uint8_t {
    RX_DATA,
    RX_IAC,
    RX_IAC_OPTION,
    RX_SUBNEG,
    RX_SUBNEG_IAC
  };

  void ensure_fifos();
  void send_iac(uint8_t verb, uint8_t opt);
  void on_connect();
  void deliver_rx(uint8_t c);
  void drain_rx();
  void drain_fifo(Fifo& fifo);
  bool has_diag() const { return diag_storage_ != nullptr && diag_size_ > 0; }

  Hooks hooks_{};
  WiFiServer server_{23};
  WiFiClient client_;
  bool enabled_ = false;
  bool started_ = false;
  uint16_t port_ = 23;
  char client_ip_[20] = {0};

  uint8_t* out_storage_ = nullptr;
  size_t out_size_ = 0;
  uint8_t* in_storage_ = nullptr;
  size_t in_size_ = 0;
  uint8_t* diag_storage_ = nullptr;
  size_t diag_size_ = 0;
  Fifo out_{};
  Fifo in_{};
  Fifo diag_{};
  bool fifos_inited_ = false;

  std::atomic<uint32_t> dropped_{0};
  std::atomic<bool> poll_seen_{false};
  std::atomic<bool> guest_reset_requested_{false};

  RxState rx_state_ = RX_DATA;
  bool rx_after_cr_ = false;
};
