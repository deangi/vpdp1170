#include "net_task.h"
#include "wifi_sta.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int kMaxPollers = 8;
static HostNetPollFn g_pollers[kMaxPollers];
static int g_npollers = 0;
static uint32_t g_period_ms = 2;
static TaskHandle_t g_task = nullptr;

bool host_net_task_add(HostNetPollFn fn) {
  if (!fn || g_npollers >= kMaxPollers) return false;
  g_pollers[g_npollers++] = fn;
  return true;
}

void host_net_task_set_period_ms(uint32_t ms) {
  if (ms == 0) ms = 1;
  g_period_ms = ms;
}

void host_net_task_poll_once() {
  for (int i = 0; i < g_npollers; i++)
    g_pollers[i]();
  host_wifi_service_link();
}

static void net_task_loop(void*) {
  for (;;) {
    host_net_task_poll_once();
    vTaskDelay(pdMS_TO_TICKS(g_period_ms));
  }
}

bool host_net_task_start(const char* name, uint32_t stack, int prio, int core) {
  if (g_task) return true;
  if (!name) name = "net";
  return xTaskCreatePinnedToCore(net_task_loop, name, stack, nullptr, prio,
                                 &g_task, core) == pdPASS;
}
