#include "storage_guard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t g_storage_mutex = nullptr;
static StaticSemaphore_t g_storage_mutex_buffer;
static portMUX_TYPE g_storage_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;

void host_sd_lock() {
  if (!g_storage_mutex) {
    portENTER_CRITICAL(&g_storage_mutex_init_mux);
    if (!g_storage_mutex)
      g_storage_mutex = xSemaphoreCreateRecursiveMutexStatic(&g_storage_mutex_buffer);
    portEXIT_CRITICAL(&g_storage_mutex_init_mux);
  }
  if (g_storage_mutex) xSemaphoreTakeRecursive(g_storage_mutex, portMAX_DELAY);
}

void host_sd_unlock() {
  if (g_storage_mutex) xSemaphoreGiveRecursive(g_storage_mutex);
}
