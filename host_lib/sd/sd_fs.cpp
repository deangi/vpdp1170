// CrowPanel Advance 7" SD — ESP-IDF SDSPI, CS=GPIO_NUM_NC (card CS hard-tied).
// Proven in CrowPanelBringup: DIP S1=1 S0=1, stable clock 20 MHz (40 MHz fails).
#include "config.h"

#if VPDP_SD_BACKEND == VPDP_SD_SPI_IDF

#include "sd_fs.h"
#include "platform.h"

#include "vfs_api.h"

#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include <string.h>

static sdmmc_card_t* g_card = nullptr;
static fs::FS* g_fs = nullptr;
static bool g_spi_bus_live = false;

CrowSdFs SD_FS;

static void crow_sd_release_bus(spi_host_device_t host_id) {
  if (g_spi_bus_live) {
    spi_bus_free(host_id);
    g_spi_bus_live = false;
  }
}

static bool crow_sd_mount_at(int max_freq_khz) {
  LOG("SD: IDF SDSPI CS=NC host=SPI2 max=%d kHz (DIP S1=1 S0=1)", max_freq_khz);

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = max_freq_khz;

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = CROW_SD_MOSI;
  bus_cfg.miso_io_num = CROW_SD_MISO;
  bus_cfg.sclk_io_num = CROW_SD_SCK;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 16 * 1024;

  esp_err_t err =
      spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (err != ESP_OK) {
    LOGE("SD: spi_bus_initialize %s", esp_err_to_name(err));
    return false;
  }
  g_spi_bus_live = true;

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = GPIO_NUM_NC;
  slot.host_id = (spi_host_device_t)host.slot;

  esp_vfs_fat_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = false;
  mount_cfg.max_files = SD_MAX_OPEN_FILES;
  mount_cfg.allocation_unit_size = 16 * 1024;

  sdmmc_card_t* card = nullptr;
  err = esp_vfs_fat_sdspi_mount(CROW_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
  if (err != ESP_OK) {
    LOGE("SD: idf mount %s", esp_err_to_name(err));
    crow_sd_release_bus((spi_host_device_t)host.slot);
    return false;
  }

  g_card = card;

  // Arduino FS layer over the same VFS mount so SD_FS.open("/x") → /sdcard/x.
  auto* impl = new VFSImpl();
  impl->mountpoint(CROW_SD_MOUNT_POINT);
  g_fs = new fs::FS(fs::FSImplPtr(impl));

  LOG("SD: IDF mount OK at %s  freq=%u kHz  size=%llu MB",
      CROW_SD_MOUNT_POINT, (unsigned)card->max_freq_khz,
      (unsigned long long)(SD_FS.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool crow_sd_mount() {
  if (g_fs) return true;  // already mounted

  // 40 MHz HS fails through the CrowPanel mux; 20 MHz is the bring-up sweet spot.
  const int khz_try[] = {CROW_SD_SPI_KHZ, 10000, 4000, 400};
  for (int khz : khz_try) {
    if (crow_sd_mount_at(khz)) return true;
  }
  LOGE("SD: CrowPanel mount FAILED — DIP S1/S0=TF? card FAT32+MBR?");
  return false;
}

fs::File CrowSdFs::open(const char* path, const char* mode) {
  if (!g_fs) return fs::File();
  return g_fs->open(path, mode);
}

bool CrowSdFs::exists(const char* path) {
  if (!g_fs) return false;
  return g_fs->exists(path);
}

bool CrowSdFs::remove(const char* path) {
  if (!g_fs) return false;
  return g_fs->remove(path);
}

bool CrowSdFs::rename(const char* pathFrom, const char* pathTo) {
  if (!g_fs) return false;
  return g_fs->rename(pathFrom, pathTo);
}

uint64_t CrowSdFs::cardSize() const {
  if (!g_card) return 0;
  return (uint64_t)g_card->csd.capacity * (uint64_t)g_card->csd.sector_size;
}

uint8_t CrowSdFs::cardType() const {
  if (!g_card) return CARD_NONE;
  return (g_card->ocr & (1U << 30)) ? CARD_SDHC : CARD_SD;  // SD_OCR_SDHC_CAP
}

#endif  // VPDP_SD_BACKEND == VPDP_SD_SPI_IDF
