#include "boot_script.h"

#include "appconfig.h"
#include "console.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

namespace {

// Wait after a prompt match before injecting the reply so the guest has
// finished printing and is ready to accept the answer.
static constexpr uint32_t kReplyDelayMs = 500;

AppConfig::BootScriptStep g_steps[AppConfig::BOOT_SCRIPT_MAX_STEPS];
uint8_t g_count = 0;
uint8_t g_index = 0;
uint8_t g_match = 0;
bool g_armed = false;
bool g_reply_pending = false;
uint32_t g_reply_due_ms = 0;

uint8_t fold_ascii(uint8_t c) {
  return (uint8_t)tolower((unsigned char)c);
}

void schedule_reply() {
  g_reply_pending = true;
  g_reply_due_ms = millis() + kReplyDelayMs;
}

void fire_pending_reply() {
  if (!g_reply_pending || g_index >= g_count) {
    g_reply_pending = false;
    return;
  }

  const AppConfig::BootScriptStep& step = g_steps[g_index];
  for (uint8_t i = 0; i < step.reply_len; i++)
    console_key_push(step.reply[i]);

  g_reply_pending = false;
  g_index++;
  g_match = 0;
  if (g_index >= g_count) {
    g_armed = false;
  }
}

}  // namespace

void boot_script_disarm() {
  g_armed = false;
  g_count = 0;
  g_index = 0;
  g_match = 0;
  g_reply_pending = false;
  g_reply_due_ms = 0;
}

void boot_script_arm(const AppConfig& cfg) {
  boot_script_disarm();
  if (cfg.boot_script_count == 0) return;

  g_count = cfg.boot_script_count;
  if (g_count > AppConfig::BOOT_SCRIPT_MAX_STEPS)
    g_count = AppConfig::BOOT_SCRIPT_MAX_STEPS;
  memcpy(g_steps, cfg.boot_script, sizeof(g_steps[0]) * g_count);
  g_armed = true;
}

bool boot_script_active() {
  return g_armed && (g_index < g_count || g_reply_pending);
}

void boot_script_observe(uint8_t c) {
  if (!g_armed || g_index >= g_count) return;
  // Do not resume matching until the deferred reply has been sent.
  if (g_reply_pending) return;

  const AppConfig::BootScriptStep& step = g_steps[g_index];
  if (step.expect_len == 0) {
    schedule_reply();
    return;
  }

  const uint8_t want = fold_ascii(step.expect[g_match]);
  const uint8_t got = fold_ascii(c);
  if (got == want) {
    g_match++;
  } else if (g_match > 0) {
    g_match = 0;
    if (fold_ascii(step.expect[0]) == got) g_match = 1;
  }

  if (g_match >= step.expect_len)
    schedule_reply();
}

void boot_script_poll() {
  if (!g_reply_pending) return;
  if ((int32_t)(millis() - g_reply_due_ms) < 0) return;
  fire_pending_reply();
}
