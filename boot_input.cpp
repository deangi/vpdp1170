#include "boot_input.h"

#include "appconfig.h"
#include "console.h"
#include "host_lib/boot/boot_input.h"

#include <string.h>

void boot_input_disarm() {
  host_boot_input_disarm();
}

void boot_input_arm(const AppConfig& cfg) {
  host_boot_input_set_inject(console_key_push);
  if (cfg.boot_input_segment_count == 0) {
    host_boot_input_disarm();
    return;
  }
  HostBootInputSegment segs[HOST_BOOT_INPUT_MAX_SEGMENTS];
  uint8_t n = cfg.boot_input_segment_count;
  if (n > HOST_BOOT_INPUT_MAX_SEGMENTS) n = HOST_BOOT_INPUT_MAX_SEGMENTS;
  memset(segs, 0, sizeof(segs));
  for (uint8_t i = 0; i < n; i++) {
    segs[i].delay_ms = cfg.boot_input_segments[i].delay_ms;
    segs[i].data_len = cfg.boot_input_segments[i].data_len;
    if (segs[i].data_len > HostBootInputSegment::DATA_MAX)
      segs[i].data_len = HostBootInputSegment::DATA_MAX;
    memcpy(segs[i].data, cfg.boot_input_segments[i].data, segs[i].data_len);
  }
  host_boot_input_arm(segs, n);
}

bool boot_input_active() { return host_boot_input_active(); }
void boot_input_poll() { host_boot_input_poll(); }
