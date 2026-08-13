#pragma once
#include <stdint.h>

struct HostWifiConnectResult {
  bool ok = false;
  uint32_t sta_ip_host_order = 0;
};

HostWifiConnectResult host_wifi_connect(const char* ssid, const char* pass,
                                        const char* hostname,
                                        uint32_t timeout_ms);

bool host_wifi_connected();
uint32_t host_wifi_sta_ip();

// Called from the net task when STA IP becomes valid (or 0 on drop).
void host_wifi_set_sta_ip_hook(void (*fn)(uint32_t ip_host_order));
// Called when WiFi transitions to connected (e.g. re-arm NTP).
void host_wifi_set_up_hook(void (*fn)());

// Periodic reconnect + hook dispatch. Call from net_task (~10 s cadence
// is applied internally).
void host_wifi_service_link();
