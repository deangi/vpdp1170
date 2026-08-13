#pragma once
#ifndef HOST_LIB_FIFO_H
#define HOST_LIB_FIFO_H
#include <atomic>
#include <stddef.h>
#include <stdint.h>

// Single-producer single-consumer byte ring with externally-provided
// power-of-two storage. Lockless: one producer calls push() and one
// consumer calls pop()/peek()/consume(). Release/acquire cursor publication
// makes the payload visible across ESP32-S3 cores without a
// mutex or critical section. Storage may live in PSRAM; cursors should live
// in internal RAM with the Fifo object.
class Fifo {
public:
  // Storage must be a power-of-two size. One slot is reserved to
  // distinguish full from empty, so usable capacity is size_pow2 - 1.
  void init(uint8_t* storage, size_t size_pow2) {
    buf  = storage;
    mask = size_pow2 - 1;
    head.store(0, std::memory_order_relaxed);
    tail.store(0, std::memory_order_relaxed);
  }

  bool push(uint8_t b, bool* was_empty = nullptr) {
    uint32_t h = head.load(std::memory_order_relaxed);
    uint32_t next = (h + 1) & mask;
    uint32_t t = tail.load(std::memory_order_acquire);
    if (next == t) {
      if (was_empty) *was_empty = false;
      return false;                          // full - drop new byte
    }
    if (was_empty) *was_empty = (h == t);
    buf[h] = b;
    head.store(next, std::memory_order_release);
    return true;
  }

  bool pop(uint8_t* b) {
    uint32_t t = tail.load(std::memory_order_relaxed);
    if (head.load(std::memory_order_acquire) == t) return false;
    *b = buf[t];
    tail.store((t + 1) & mask, std::memory_order_release);
    return true;
  }

  // Zero-copy peek: returns the longest contiguous run at the tail
  // (may be less than count() when the data wraps the buffer). Caller
  // is expected to send/consume <= the returned length before calling
  // peek again. Pair with consume().
  size_t peek(const uint8_t** out_ptr) const {
    uint32_t t = tail.load(std::memory_order_relaxed);
    uint32_t h = head.load(std::memory_order_acquire);
    if (t == h) { *out_ptr = nullptr; return 0; }
    *out_ptr = buf + t;
    uint32_t cap    = mask + 1;
    uint32_t to_end = cap - t;
    uint32_t avail  = (h - t) & mask;
    return avail < to_end ? avail : to_end;
  }

  void consume(size_t n) {
    uint32_t t = tail.load(std::memory_order_relaxed);
    tail.store((uint32_t)((t + n) & mask), std::memory_order_release);
  }

  bool empty() const {
    return head.load(std::memory_order_acquire) ==
           tail.load(std::memory_order_acquire);
  }
  size_t count() const {
    uint32_t h = head.load(std::memory_order_acquire);
    uint32_t t = tail.load(std::memory_order_acquire);
    return (h - t) & mask;
  }
  size_t capacity() const { return mask; }
  size_t free_space() const { return capacity() - count(); }
  void clear() {
    tail.store(head.load(std::memory_order_acquire),
               std::memory_order_release);
  }

private:
  uint8_t*              buf  = nullptr;
  uint32_t              mask = 0;
  std::atomic<uint32_t> head { 0 };
  std::atomic<uint32_t> tail { 0 };
};

#endif  // HOST_LIB_FIFO_H
