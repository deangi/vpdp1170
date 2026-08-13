#pragma once
#include <stddef.h>
#include <stdint.h>

struct HostBootInputSegment {
  static constexpr size_t DATA_MAX = 64;
  uint32_t delay_ms = 0;
  uint8_t data[DATA_MAX];
  uint8_t data_len = 0;
};

static constexpr size_t HOST_BOOT_INPUT_MAX_SEGMENTS = 16;

using HostKeyInjectFn = void (*)(uint8_t c);

void host_boot_input_set_inject(HostKeyInjectFn fn);
void host_boot_input_arm(const HostBootInputSegment* segs, uint8_t count);
void host_boot_input_disarm();
void host_boot_input_poll();
bool host_boot_input_active();
