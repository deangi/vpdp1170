#include "boot_script.h"

#include "appconfig.h"
#include "console.h"

#include <Arduino.h>
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
// Snapshot of the step committed for the deferred reply (index advances on
// match so that clause cannot fire again while the delay is outstanding).
uint8_t g_pending_reply[AppConfig::BootScriptStep::REPLY_MAX];
uint8_t g_pending_reply_len = 0;

// Case-fold letters only so \r \n space and other controls match exactly.
uint8_t fold_ascii(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return (uint8_t)(c + ('a' - 'A'));
  return c;
}

void schedule_reply() {
  // Each clause is applied at most once: ignore if already deferred or done.
  if (g_reply_pending || !g_armed || g_index >= g_count) return;

  const AppConfig::BootScriptStep& step = g_steps[g_index];
  g_pending_reply_len = step.reply_len;
  if (g_pending_reply_len > sizeof(g_pending_reply))
    g_pending_reply_len = sizeof(g_pending_reply);
  memcpy(g_pending_reply, step.reply, g_pending_reply_len);

  g_reply_pending = true;
  g_reply_due_ms = millis() + kReplyDelayMs;

  // Consume this step immediately so a reprinted prompt cannot re-match it.
  g_index++;
  g_match = 0;
  if (g_index >= g_count) g_armed = false;
}

void fire_pending_reply() {
  if (!g_reply_pending) return;

  for (uint8_t i = 0; i < g_pending_reply_len; i++)
    console_key_push(g_pending_reply[i]);

  g_reply_pending = false;
  g_pending_reply_len = 0;
}

}  // namespace

void boot_script_disarm() {
  g_armed = false;
  g_count = 0;
  g_index = 0;
  g_match = 0;
  g_reply_pending = false;
  g_reply_due_ms = 0;
  g_pending_reply_len = 0;
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
  // Reply may still be deferred after the last step was consumed.
  return g_reply_pending || (g_armed && g_index < g_count);
}

void boot_script_observe(uint8_t c) {
  // Isolate matcher from guest NUL padding (e.g. CR/NUL on KL11).
  if (c == 0) return;
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
