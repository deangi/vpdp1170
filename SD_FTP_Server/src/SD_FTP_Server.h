#pragma once
#include <Arduino.h>
#include <stdint.h>

// Minimal passive/active FTP server backed by an SD_MMC card mounted under
// a POSIX VFS root (default "/sdcard"). Single client at a time. Built
// around POSIX opendir/readdir/fopen so it sidesteps arduino-esp32 SD_MMC
// bug #8870 (root-level openNextFile() drops entries).
//
// Supports PASV (preferred; works behind NAT) and PORT (active mode; needed
// for Windows cmd.exe `ftp`). MLSD/EPSV/REST intentionally omitted.
//
// Usage:
//   SD_FTP_Server::Config cfg;
//   cfg.port = 21;
//   cfg.user = "esp32"; cfg.pass = "secret";   // both empty = anonymous
//   cfg.vfs_root = "/sdcard";                  // where SD_MMC is mounted
//   cfg.log_fn = [](const char* s){ Serial.println(s); };
//   SDFTPServer.begin(cfg);
//   // ... in loop():
//   SDFTPServer.poll();
//
// poll() lazily starts/stops the listener as WiFi state changes, so call
// begin() once and poll() forever. Run poll() on a core other than any
// realtime workload — listdir/file I/O can block for tens of ms.

class SD_FTP_Server {
public:
  struct Config {
    uint16_t    port        = 21;
    const char* user        = "";        // empty = accept any user
    const char* pass        = "";        // empty = no password required
    const char* vfs_root    = "/sdcard"; // POSIX mount point of the SD card
    void (*log_fn)(const char* msg)     = nullptr;  // optional info logger
    void (*log_err_fn)(const char* msg) = nullptr;  // optional error logger
    // Return true when an FTP path names a mounted image, or a directory
    // containing one. Protected paths cannot be transferred or mutated.
    bool (*path_protected_fn)(const char* ftp_path) = nullptr;
  };

  void     begin(const Config& cfg);
  void     poll();
  bool     listening() const;
  bool     connected() const;
  uint16_t port()      const;
};

extern SD_FTP_Server SDFTPServer;

// Shared recursive lock for every SD operation that can run concurrently
// with FTP. Emulator disk backends should hold this around open/close,
// seek/read/write, and image creation.
void sd_ftp_storage_lock();
void sd_ftp_storage_unlock();

class SD_FTP_StorageGuard {
public:
  SD_FTP_StorageGuard()  { sd_ftp_storage_lock(); }
  ~SD_FTP_StorageGuard() { sd_ftp_storage_unlock(); }
  SD_FTP_StorageGuard(const SD_FTP_StorageGuard&) = delete;
  SD_FTP_StorageGuard& operator=(const SD_FTP_StorageGuard&) = delete;
};
