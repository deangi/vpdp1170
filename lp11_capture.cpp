#include "lp11_capture.h"

#include "fifo.h"
#include "platform.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

#include <Arduino.h>
#include "sd_fs.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace lp11_capture {

static constexpr size_t FIFO_SIZE = 4096;
static constexpr size_t CHUNK_BYTES = 1024;
static constexpr uint32_t IDLE_FLUSH_MS = 2000;
static constexpr uint32_t PERIOD_MS = 500;

EXT_RAM_BSS_ATTR static uint8_t fifo_storage[FIFO_SIZE];
static Fifo fifo;
static bool fifo_ready = false;

static char current_name[32] = {0};
static std::atomic<uint32_t> last_push_ms{0};
static std::atomic<bool> rotate_requested{false};
static TaskHandle_t consumer_task = nullptr;
static SemaphoreHandle_t session_mu = nullptr;

static void ensure_fifo() {
  if (fifo_ready) return;
  fifo.init(fifo_storage, FIFO_SIZE);
  fifo_ready = true;
}

static void ensure_mutex() {
  if (session_mu) return;
  session_mu = xSemaphoreCreateMutex();
}

static bool parse_lp_index(const char* name, int* out_n) {
  if (!name || !out_n) return false;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (tolower((unsigned char)base[0]) != 'l') return false;
  if (tolower((unsigned char)base[1]) != 'p') return false;
  const char* p = base + 2;
  if (!isdigit((unsigned char)*p)) return false;
  int n = 0;
  while (isdigit((unsigned char)*p)) {
    n = n * 10 + (*p - '0');
    if (n > 9999) return false;
    ++p;
  }
  if (*p != '.') return false;
  ++p;
  if (tolower((unsigned char)p[0]) != 't') return false;
  if (tolower((unsigned char)p[1]) != 'x') return false;
  if (tolower((unsigned char)p[2]) != 't') return false;
  if (p[3] != 0) return false;
  *out_n = n;
  return true;
}

static void set_current_path(int n) {
  snprintf(current_name, sizeof(current_name), "/LP%d.TXT", n);
}

static void pick_lp_path() {
  int best_empty = -1;
  int max_n = -1;

  File root = SD_FS.open("/");
  if (root) {
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) continue;
      int n = -1;
      if (!parse_lp_index(f.name(), &n)) continue;
      if (n > max_n) max_n = n;
      if (f.size() == 0 && (best_empty < 0 || n < best_empty))
        best_empty = n;
    }
    root.close();
  }

  int chosen = (best_empty >= 0) ? best_empty : (max_n + 1);
  if (chosen < 0) chosen = 0;
  set_current_path(chosen);
  LOG("LP11 capture session file %s (empty_reuse=%d max=%d)",
      current_name, best_empty, max_n);
}

static bool write_chunk(const uint8_t* data, size_t len) {
  if (!current_name[0] || !data || len == 0) return false;

  File f = SD_FS.open(current_name, "a");
  if (!f) {
    LOGE("LP11 capture OPEN failed path=%s", current_name);
    return false;
  }
  size_t written = f.write(data, len);
  f.flush();
  f.close();
  if (written != len) {
    LOGE("LP11 capture WRITE short path=%s wrote=%u want=%u",
         current_name, (unsigned)written, (unsigned)len);
    return false;
  }
  LOG("LP11 capture WRITE path=%s bytes=%u", current_name, (unsigned)len);
  return true;
}

static size_t pop_bytes(uint8_t* dest, size_t want) {
  size_t got = 0;
  while (got < want) {
    const uint8_t* ptr = nullptr;
    size_t n = fifo.peek(&ptr);
    if (n == 0) break;
    size_t take = want - got;
    if (take > n) take = n;
    memcpy(dest + got, ptr, take);
    fifo.consume(take);
    got += take;
  }
  return got;
}

static void requeue(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (!fifo.push(data[i])) break;
  }
}

static void drain_policy() {
  ensure_fifo();
  if (!current_name[0]) return;

  uint8_t chunk[CHUNK_BYTES];

  while (fifo.count() >= CHUNK_BYTES) {
    size_t n = pop_bytes(chunk, CHUNK_BYTES);
    if (n == 0) break;
    if (!write_chunk(chunk, n)) {
      requeue(chunk, n);
      return;
    }
  }

  const size_t pending = fifo.count();
  if (pending == 0) return;

  const uint32_t last = last_push_ms.load(std::memory_order_relaxed);
  const uint32_t now = millis();
  if (pending < CHUNK_BYTES && last != 0 && (now - last) >= IDLE_FLUSH_MS) {
    size_t n = pop_bytes(chunk, pending);
    if (n == 0) return;
    if (!write_chunk(chunk, n))
      requeue(chunk, n);
  }
}

static void open_session_file() {
  if (SD_FS.cardType() == CARD_NONE) {
    current_name[0] = 0;
    LOG("LP11 capture session deferred (SD not mounted)");
    return;
  }
  pick_lp_path();
  if (!current_name[0]) return;
  if (!SD_FS.exists(current_name)) {
    File f = SD_FS.open(current_name, "w");
    if (f) {
      f.close();
      LOG("LP11 capture CREATE path=%s", current_name);
    } else {
      LOGE("LP11 capture CREATE failed path=%s", current_name);
    }
  }
}

static void consumer_loop(void*) {
  for (;;) {
    {
      SD_FTP_StorageGuard guard;
      ensure_mutex();
      if (session_mu &&
          xSemaphoreTake(session_mu, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (rotate_requested.load(std::memory_order_acquire)) {
          // Prior FIFO contents were dropped on reset; open next LPn.TXT.
          open_session_file();
          if (current_name[0])
            rotate_requested.store(false, std::memory_order_release);
        }
        drain_policy();
        xSemaphoreGive(session_mu);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
  }
}

void init() {
  ensure_fifo();
  ensure_mutex();
  if (consumer_task) return;
  xTaskCreatePinnedToCore(consumer_loop, "lp_consumer", 6144, nullptr,
                          1, &consumer_task, 0);
  LOG("LP11 capture consumer task started (period %u ms)", (unsigned)PERIOD_MS);
}

void begin_session() {
  // Emulator reset must not block on SD/FTP. Drop any pending print bytes and
  // ask the lp_consumer task to pick the next /LPn.TXT on its next wake.
  ensure_fifo();
  ensure_mutex();
  fifo.clear();
  last_push_ms.store(0, std::memory_order_relaxed);
  rotate_requested.store(true, std::memory_order_release);
}

bool push(uint8_t c) {
  ensure_fifo();
  // NUL makes Windows (and other tools) mis-detect the file as UTF-16BE
  // when it leads the capture; real LP listings are 7-bit text — skip NUL.
  if (c == 0)
    return true;
  if (!fifo.push(c)) return false;
  last_push_ms.store(millis(), std::memory_order_relaxed);
  return true;
}

size_t free_space() {
  ensure_fifo();
  return fifo.free_space();
}

size_t count() {
  ensure_fifo();
  return fifo.count();
}

const char* current_path() {
  return current_name;
}

}  // namespace lp11_capture
