#pragma once

// Recursive mutex serializing all SD I/O that can run concurrently
// (FTP, guest disk backends, LP capture, telnet-shell FS commands).

void host_sd_lock();
void host_sd_unlock();

class HostSdGuard {
public:
  HostSdGuard() { host_sd_lock(); }
  ~HostSdGuard() { host_sd_unlock(); }
  HostSdGuard(const HostSdGuard&) = delete;
  HostSdGuard& operator=(const HostSdGuard&) = delete;
};

// Historical alias used throughout vpdp / FTP call sites.
using SD_FTP_StorageGuard = HostSdGuard;
inline void sd_ftp_storage_lock() { host_sd_lock(); }
inline void sd_ftp_storage_unlock() { host_sd_unlock(); }
