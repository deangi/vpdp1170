#pragma once
#include <Arduino.h>
#include <stdint.h>

// Generic [wifi]/[ntp]/[telnet]/[ftp] (+ optional [dz11]) INI fields.
struct HostNetConfig {
  String wifi_ssid;
  String wifi_password;
  String wifi_hostname;
  bool ntp_enabled = true;
  String ntp_server;
  bool telnet_enabled = true;
  int telnet_port = 23;
  bool extra_telnet_enabled = true;
  int extra_telnet_port = 2323;
  bool ftp_enabled = true;
  int ftp_port = 21;
  String ftp_user;
  String ftp_password;
};

bool host_net_ini_is_section(const char* section);
bool host_net_ini_apply(HostNetConfig& cfg, const char* section,
                        const char* key, const char* val);

// SD-root variant picker: files matching "<prefix>NAME.ini".
int host_ini_list_variants(const char* prefix, char names[][44], int max);
bool host_ini_copy_file(const char* src, const char* dst);
