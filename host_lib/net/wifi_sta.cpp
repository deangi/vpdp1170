#include "wifi_sta.h"
#include "platform.h"

#include <WiFi.h>

static void (*g_sta_ip_hook)(uint32_t) = nullptr;
static void (*g_up_hook)() = nullptr;
static uint32_t g_wifi_ms = 0;
static bool g_wifi_was_up = false;
static constexpr uint32_t kLinkCheckMs = 10000;

static uint32_t ip_to_host_order(IPAddress ip) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

void host_wifi_set_sta_ip_hook(void (*fn)(uint32_t)) { g_sta_ip_hook = fn; }
void host_wifi_set_up_hook(void (*fn)()) { g_up_hook = fn; }

bool host_wifi_connected() { return WiFi.status() == WL_CONNECTED; }

uint32_t host_wifi_sta_ip() {
  if (!host_wifi_connected()) return 0;
  return ip_to_host_order(WiFi.localIP());
}

HostWifiConnectResult host_wifi_connect(const char* ssid, const char* pass,
                                        const char* hostname,
                                        uint32_t timeout_ms) {
  HostWifiConnectResult r;
  if (!ssid || !ssid[0]) {
    LOGE("WiFi SSID is empty - set [wifi] ssid= in wificonfig.ini");
    return r;
  }
  if (!hostname) hostname = "esp32";
  if (!pass) pass = "";

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);

  LOG("WiFi connecting to \"%s\" (hostname=%s) ...", ssid, hostname);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    r.ok = true;
    r.sta_ip_host_order = ip_to_host_order(WiFi.localIP());
    g_wifi_was_up = true;
    LOG("WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
    LOG("WiFi gateway=%s", WiFi.gatewayIP().toString().c_str());
    if (g_sta_ip_hook) g_sta_ip_hook(r.sta_ip_host_order);
    if (g_up_hook) g_up_hook();
  } else {
    LOGE("WiFi connect timed out");
    g_wifi_was_up = false;
    if (g_sta_ip_hook) g_sta_ip_hook(0);
  }
  return r;
}

void host_wifi_service_link() {
  const wl_status_t st = WiFi.status();
  const bool wifi_up = (st == WL_CONNECTED);
  if (wifi_up && !g_wifi_was_up && g_up_hook)
    g_up_hook();
  g_wifi_was_up = wifi_up;

  uint32_t now = millis();
  if (now - g_wifi_ms < kLinkCheckMs) return;
  g_wifi_ms = now;

  if (st == WL_DISCONNECTED || st == WL_CONNECTION_LOST ||
      st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
    LOGE("WiFi link down (status=%d) - reconnecting", (int)st);
    WiFi.reconnect();
    if (g_sta_ip_hook) g_sta_ip_hook(0);
  } else if (st == WL_CONNECTED) {
    if (g_sta_ip_hook) g_sta_ip_hook(ip_to_host_order(WiFi.localIP()));
  }
}
