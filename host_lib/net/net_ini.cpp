#include "net_ini.h"
#include "platform.h"
#include "sd_fs.h"
#include "../sd/storage_guard.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

static bool truthy(const char* s) {
  if (!s) return false;
  return strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
         strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0;
}

bool host_net_ini_is_section(const char* section) {
  if (!section) return false;
  return strcasecmp(section, "wifi") == 0 ||
         strcasecmp(section, "ntp") == 0 ||
         strcasecmp(section, "telnet") == 0 ||
         strcasecmp(section, "ftp") == 0 ||
         strcasecmp(section, "dz11") == 0;
}

bool host_net_ini_apply(HostNetConfig& cfg, const char* section,
                        const char* key, const char* val) {
  if (!section || !key) return false;
  if (!val) val = "";

  if (strcasecmp(section, "wifi") == 0) {
    if (strcasecmp(key, "ssid") == 0) { cfg.wifi_ssid = val; return true; }
    if (strcasecmp(key, "password") == 0) { cfg.wifi_password = val; return true; }
    if (strcasecmp(key, "hostname") == 0) { cfg.wifi_hostname = val; return true; }
    return false;
  }
  if (strcasecmp(section, "ntp") == 0) {
    if (strcasecmp(key, "enabled") == 0) { cfg.ntp_enabled = truthy(val); return true; }
    if (strcasecmp(key, "server") == 0) { cfg.ntp_server = val; return true; }
    return false;
  }
  if (strcasecmp(section, "telnet") == 0) {
    if (strcasecmp(key, "enabled") == 0) { cfg.telnet_enabled = truthy(val); return true; }
    if (strcasecmp(key, "port") == 0) { cfg.telnet_port = atoi(val); return true; }
    return false;
  }
  if (strcasecmp(section, "dz11") == 0) {
    if (strcasecmp(key, "enabled") == 0) { cfg.extra_telnet_enabled = truthy(val); return true; }
    if (strcasecmp(key, "port") == 0) { cfg.extra_telnet_port = atoi(val); return true; }
    return false;
  }
  if (strcasecmp(section, "ftp") == 0) {
    if (strcasecmp(key, "enabled") == 0) { cfg.ftp_enabled = truthy(val); return true; }
    if (strcasecmp(key, "port") == 0) { cfg.ftp_port = atoi(val); return true; }
    if (strcasecmp(key, "user") == 0) { cfg.ftp_user = val; return true; }
    if (strcasecmp(key, "password") == 0) { cfg.ftp_password = val; return true; }
    return false;
  }
  return false;
}

static int variant_name_cmp(const void* a, const void* b) {
  return strcasecmp((const char*)a, (const char*)b);
}

int host_ini_list_variants(const char* prefix, char names[][44], int max) {
  HostSdGuard guard;
  if (!prefix || max <= 0) return 0;
  int count = 0;

  fs::File root = SD_FS.open("/");
  if (!root) return 0;

  size_t plen = strlen(prefix);
  for (fs::File f = root.openNextFile(); f && count < max;
       f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char* fullname = f.name();
      const char* slash = strrchr(fullname, '/');
      const char* base = slash ? slash + 1 : fullname;
      size_t blen = strlen(base);
      if (strncmp(base, prefix, plen) == 0 &&
          blen > plen + 4 &&
          strcasecmp(base + blen - 4, ".ini") == 0) {
        size_t midlen = blen - plen - 4;
        if (midlen > 0 && midlen < 43) {
          memcpy(names[count], base + plen, midlen);
          names[count][midlen] = 0;
          count++;
        }
      }
    }
    f.close();
  }
  root.close();

  if (count > 1)
    qsort(names, (size_t)count, 44, variant_name_cmp);
  return count;
}

bool host_ini_copy_file(const char* src, const char* dst) {
  HostSdGuard guard;
  char temp[192];
  char backup[192];
  if (snprintf(temp, sizeof(temp), "%s.tmp", dst) >= (int)sizeof(temp) ||
      snprintf(backup, sizeof(backup), "%s.bak", dst) >= (int)sizeof(backup)) {
    LOGE("config_copy_file: destination path too long: %s", dst);
    return false;
  }

  File s = SD_FS.open(src, FILE_READ);
  if (!s) { LOGE("config_copy_file: can't open %s for read", src); return false; }
  uint32_t srcSize = (uint32_t)s.size();

  if (SD_FS.exists(temp)) SD_FS.remove(temp);
  File d = SD_FS.open(temp, FILE_WRITE);
  if (!d) {
    LOGE("config_copy_file: can't open %s for write", temp);
    s.close();
    return false;
  }

  uint8_t buf[512];
  size_t total = 0;
  bool copy_ok = true;
  while (s.available()) {
    int n = s.read(buf, sizeof(buf));
    if (n <= 0) { copy_ok = false; break; }
    int w = d.write(buf, n);
    if (w != n) {
      LOGE("config_copy_file: short write (%d/%d) at %u into %s",
           w, n, (unsigned)total, temp);
      copy_ok = false;
      break;
    }
    total += n;
  }
  s.close();
  d.flush();
  d.close();

  File v = SD_FS.open(temp, FILE_READ);
  uint32_t verifySize = v ? (uint32_t)v.size() : 0;
  if (v) v.close();
  if (!copy_ok || total != srcSize || verifySize != srcSize) {
    LOGE("config_copy_file: temporary copy failed src=%u written=%u on-disk=%u",
         (unsigned)srcSize, (unsigned)total, (unsigned)verifySize);
    SD_FS.remove(temp);
    return false;
  }

  if (SD_FS.exists(backup)) SD_FS.remove(backup);
  bool had_dst = SD_FS.exists(dst);
  if (had_dst && !SD_FS.rename(dst, backup)) {
    LOGE("config_copy_file: can't preserve %s as %s", dst, backup);
    SD_FS.remove(temp);
    return false;
  }
  if (!SD_FS.rename(temp, dst)) {
    LOGE("config_copy_file: can't activate %s", dst);
    if (had_dst && !SD_FS.rename(backup, dst))
      LOGE("config_copy_file: FAILED to restore %s from %s", dst, backup);
    SD_FS.remove(temp);
    return false;
  }
  if (had_dst) SD_FS.remove(backup);
  LOG("config_copy_file: atomically replaced %s from %s (%u bytes)",
      dst, src, (unsigned)srcSize);
  return true;
}
