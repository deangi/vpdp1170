#pragma once
#include <stdint.h>

// Thin compatibility wrapper around the SD_FTP_Server library. The library
// itself lives under SD_FTP_Server/src/ — see SD_FTP_Server_build.cpp for the
// build-time include shim. New code in this project should call SDFTPServer.*
// directly; these forwarders exist so the existing telnet-style call sites
// (ftp_begin / ftp_poll on net_task / status pills) keep working.

void        ftp_begin(uint16_t port, bool enabled, const char* user, const char* pass);
void        ftp_poll();  // core-0 net_task; SD I/O via SD_FTP_StorageGuard
bool        ftp_enabled();
bool        ftp_listening();
bool        ftp_connected();
uint16_t    ftp_port();
