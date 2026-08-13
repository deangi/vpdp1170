#include "boot_script.h"

#include <Arduino.h>
#include <string.h>

static constexpr uint32_t kReplyDelayMs = 500;

static HostBootScriptStep g_steps[HOST_BOOT_SCRIPT_MAX_STEPS];
static uint8_t g_count = 0;
static uint8_t g_index = 0;
static uint8_t g_match = 0;
static bool g_armed = false;
static bool g_reply_pending = false;
static uint32_t g_reply_due_ms = 0;
static uint8_t g_pending_reply[HostBootScriptStep::REPLY_MAX];
static uint8_t g_pending_reply_len = 0;
static HostKeyInjectFn g_inject = nullptr;

static uint8_t fold_ascii(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return (uint8_t)(c + ('a' - 'A'));
  return c;
}

void host_boot_script_set_inject(HostKeyInjectFn fn) {
  g_inject = fn;
}

static void schedule_reply() {
  if (g_reply_pending || !g_armed || g_index >= g_count) return;

  const HostBootScriptStep& step = g_steps[g_index];
  g_pending_reply_len = step.reply_len;
  if (g_pending_reply_len > sizeof(g_pending_reply))
    g_pending_reply_len = sizeof(g_pending_reply);
  memcpy(g_pending_reply, step.reply, g_pending_reply_len);

  g_reply_pending = true;
  g_reply_due_ms = millis() + kReplyDelayMs;

  g_index++;
  g_match = 0;
  if (g_index >= g_count) g_armed = false;
}

static void fire_pending_reply() {
  if (!g_reply_pending) return;
  if (g_inject) {
    for (uint8_t i = 0; i < g_pending_reply_len; i++)
      g_inject(g_pending_reply[i]);
  }
  g_reply_pending = false;
  g_pending_reply_len = 0;
}

void host_boot_script_disarm() {
  g_armed = false;
  g_count = 0;
  g_index = 0;
  g_match = 0;
  g_reply_pending = false;
  g_reply_due_ms = 0;
  g_pending_reply_len = 0;
}

void host_boot_script_arm(const HostBootScriptStep* steps, uint8_t count) {
  host_boot_script_disarm();
  if (!steps || count == 0) return;
  g_count = count;
  if (g_count > HOST_BOOT_SCRIPT_MAX_STEPS)
    g_count = HOST_BOOT_SCRIPT_MAX_STEPS;
  memcpy(g_steps, steps, sizeof(g_steps[0]) * g_count);
  g_armed = true;
}

bool host_boot_script_active() {
  return g_reply_pending || (g_armed && g_index < g_count);
}

void host_boot_script_observe(uint8_t c) {
  if (c == 0) return;
  if (!g_armed || g_index >= g_count) return;
  if (g_reply_pending) return;

  const HostBootScriptStep& step = g_steps[g_index];
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

void host_boot_script_poll() {
  if (!g_reply_pending) return;
  if ((int32_t)(millis() - g_reply_due_ms) < 0) return;
  fire_pending_reply();
}
