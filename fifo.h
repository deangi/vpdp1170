#pragma once
#include <stdint.h>
#include <stddef.h>

// Single-producer single-consumer byte ring with externally-provided
// power-of-two storage. Lockless: one thread calls push(), another (or
// the same) thread calls pop()/peek()/consume(). vpdp1140 uses these to
// decouple the KL11 console from its host sinks (Serial / Telnet / TFT)
// and its host sources (Serial / Telnet). All five instances live on
// core 1 today, but the head/tail indices are volatile so the same code
// would also be safe across cores on ESP32-S3 for byte-sized payloads.
class Fifo {
public:
  // Storage must be a power-of-two size. One slot is reserved to
  // distinguish full from empty, so usable capacity is size_pow2 - 1.
  void init(uint8_t* storage, size_t size_pow2) {
    buf  = storage;
    mask = size_pow2 - 1;
    head = 0;
    tail = 0;
  }

  bool push(uint8_t b) {
    uint32_t h = head;
    uint32_t next = (h + 1) & mask;
    if (next == tail) return false;          // full - drop new byte
    buf[h] = b;
    head = next;
    return true;
  }

  bool pop(uint8_t* b) {
    uint32_t t = tail;
    if (head == t) return false;
    *b = buf[t];
    tail = (t + 1) & mask;
    return true;
  }

  // Zero-copy peek: returns the longest contiguous run at the tail
  // (may be less than count() when the data wraps the buffer). Caller
  // is expected to send/consume <= the returned length before calling
  // peek again. Pair with consume().
  size_t peek(const uint8_t** out_ptr) const {
    uint32_t t = tail, h = head;
    if (t == h) { *out_ptr = nullptr; return 0; }
    *out_ptr = buf + t;
    uint32_t cap    = mask + 1;
    uint32_t to_end = cap - t;
    uint32_t avail  = (h - t) & mask;
    return avail < to_end ? avail : to_end;
  }

  void consume(size_t n) {
    tail = (uint32_t)((tail + n) & mask);
  }

  bool   empty()    const { return head == tail; }
  size_t count()    const { return (head - tail) & mask; }
  size_t capacity() const { return mask; }
  void   clear()          { tail = head; }

private:
  uint8_t*          buf  = nullptr;
  uint32_t          mask = 0;
  volatile uint32_t head = 0;
  volatile uint32_t tail = 0;
};
