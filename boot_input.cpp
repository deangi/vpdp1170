#include "boot_input.h"

#include "appconfig.h"
#include "console.h"
#include "platform.h"

#include <Arduino.h>
#include <string.h>

namespace {

AppConfig::BootInputSegment g_segments[AppConfig::BOOT_INPUT_MAX_SEGMENTS];
uint8_t g_count = 0;
uint8_t g_index = 0;
bool g_armed = false;
bool g_waiting = false;
uint32_t g_due_ms = 0;

void schedule_current() {
  if (!g_armed || g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }
  g_waiting = true;
  g_due_ms = millis() + g_segments[g_index].delay_ms;
}

void fire_current() {
  if (!g_armed || g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }

  const AppConfig::BootInputSegment& seg = g_segments[g_index];
  for (uint8_t i = 0; i < seg.data_len; i++)
    console_key_push(seg.data[i]);

  g_index++;
  if (g_index >= g_count) {
    g_armed = false;
    g_waiting = false;
    return;
  }
  schedule_current();
}

}  // namespace

void boot_input_disarm() {
  g_armed = false;
  g_count = 0;
  g_index = 0;
  g_waiting = false;
  g_due_ms = 0;
}

void boot_input_arm(const AppConfig& cfg) {
  boot_input_disarm();
  if (cfg.boot_input_segment_count == 0) return;

  g_count = cfg.boot_input_segment_count;
  if (g_count > AppConfig::BOOT_INPUT_MAX_SEGMENTS)
    g_count = AppConfig::BOOT_INPUT_MAX_SEGMENTS;
  memcpy(g_segments, cfg.boot_input_segments, sizeof(g_segments[0]) * g_count);
  g_armed = true;
  g_index = 0;
  schedule_current();

  uint32_t total_delay = 0;
  size_t total_bytes = 0;
  for (uint8_t i = 0; i < g_count; i++) {
    total_delay += g_segments[i].delay_ms;
    total_bytes += g_segments[i].data_len;
  }
  LOG("console: armed boot_input (%u segment%s, %u bytes, %lu ms delays)",
      (unsigned)g_count,
      g_count == 1 ? "" : "s",
      (unsigned)total_bytes,
      (unsigned long)total_delay);
}

bool boot_input_active() {
  return g_armed || g_waiting;
}

void boot_input_poll() {
  if (!g_armed || !g_waiting) return;
  if ((int32_t)(millis() - g_due_ms) < 0) return;
  fire_current();
}
