#include "boot_script.h"

#include "appconfig.h"
#include "console.h"
#include "host_lib/boot/boot_script.h"

#include <string.h>

void boot_script_disarm() {
  host_boot_script_disarm();
}

void boot_script_arm(const AppConfig& cfg) {
  host_boot_script_set_inject(console_key_push);
  if (cfg.boot_script_count == 0) {
    host_boot_script_disarm();
    return;
  }
  HostBootScriptStep steps[HOST_BOOT_SCRIPT_MAX_STEPS];
  uint8_t n = cfg.boot_script_count;
  if (n > HOST_BOOT_SCRIPT_MAX_STEPS) n = HOST_BOOT_SCRIPT_MAX_STEPS;
  memset(steps, 0, sizeof(steps));
  for (uint8_t i = 0; i < n; i++) {
    steps[i].expect_len = cfg.boot_script[i].expect_len;
    steps[i].reply_len = cfg.boot_script[i].reply_len;
    if (steps[i].expect_len > HostBootScriptStep::EXPECT_MAX)
      steps[i].expect_len = HostBootScriptStep::EXPECT_MAX;
    if (steps[i].reply_len > HostBootScriptStep::REPLY_MAX)
      steps[i].reply_len = HostBootScriptStep::REPLY_MAX;
    memcpy(steps[i].expect, cfg.boot_script[i].expect, steps[i].expect_len);
    memcpy(steps[i].reply, cfg.boot_script[i].reply, steps[i].reply_len);
  }
  host_boot_script_arm(steps, n);
}

bool boot_script_active() { return host_boot_script_active(); }
void boot_script_observe(uint8_t c) { host_boot_script_observe(c); }
void boot_script_poll() { host_boot_script_poll(); }
