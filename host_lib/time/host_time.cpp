#include "host_time.h"
#include "platform.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

// 2020-01-01 00:00:00 UTC — anything earlier is still epoch / unset.
static constexpr time_t kSyncedAfter = 1577836800;

static bool g_enabled = false;
static bool g_started = false;
static bool g_logged_sync = false;
static bool g_logged_disabled = false;
static char g_server[64] = "pool.ntp.org";

bool host_time_synced() {
  time_t now = time(nullptr);
  return now >= kSyncedAfter;
}

void host_time_begin(bool enabled, const char* server) {
  g_enabled = enabled;
  if (server && server[0]) {
    strncpy(g_server, server, sizeof(g_server) - 1);
    g_server[sizeof(g_server) - 1] = 0;
  }
  if (!g_enabled) {
    if (!g_logged_disabled) {
      LOG("NTP: disabled in /wificonfig.ini");
      g_logged_disabled = true;
    }
    return;
  }
  if (host_time_synced())
    return;

  configTime(0, 0, g_server);
  g_started = true;
  g_logged_sync = false;
  LOG("NTP: starting SNTP (UTC) server=%s", g_server);
}

void host_time_poll() {
  if (!g_enabled || !g_started || g_logged_sync)
    return;
  if (!host_time_synced())
    return;

  time_t now = time(nullptr);
  struct tm tm {};
  gmtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  LOG("NTP: synced UTC %s", buf);
  g_logged_sync = true;
}
