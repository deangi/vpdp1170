#pragma once
#include <stddef.h>
#include <stdint.h>
#include "boot_input.h"

struct HostBootScriptStep {
  static constexpr size_t EXPECT_MAX = 96;
  static constexpr size_t REPLY_MAX = 64;
  uint8_t expect[EXPECT_MAX];
  uint8_t reply[REPLY_MAX];
  uint8_t expect_len = 0;
  uint8_t reply_len = 0;
};

static constexpr size_t HOST_BOOT_SCRIPT_MAX_STEPS = 8;

void host_boot_script_set_inject(HostKeyInjectFn fn);
void host_boot_script_arm(const HostBootScriptStep* steps, uint8_t count);
void host_boot_script_disarm();
void host_boot_script_observe(uint8_t c);
void host_boot_script_poll();
bool host_boot_script_active();
