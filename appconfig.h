#pragma once
#include <Arduino.h>

struct AppConfig {
  // [system]
  String title;
  String version;
  String build;

  // [wifi]
  String wifi_ssid;
  String wifi_password;
  String wifi_hostname;

  // [telnet]
  bool   telnet_enabled = true;
  int    telnet_port    = 23;

  // [console]
  // Bytes injected into the KL11 input queue after each PDP-11 boot/reset.
  // Parsed from escaped text in /pdpconfig.ini, e.g. "unix\r" or "\003".
  static const size_t BOOT_INPUT_MAX = 256;
  uint8_t boot_input[BOOT_INPUT_MAX];
  size_t  boot_input_len = 0;

  // [serial1]
  // Enables a second DL11-compatible TTY at 0176500. Its input and output
  // files are connected at runtime through TTY0 VPDP control commands.
  bool serial1_enabled = false;

  // [ftp]
  // FTP server exposing the SD card root. Control channel uses ftp_port;
  // passive data uses ftp_port+1.
  bool   ftp_enabled = true;
  int    ftp_port    = 21;
  String ftp_user;
  String ftp_password;

  // [diag]
  // Interval (seconds) for the host's periodic "state: PC=... R0=..." dump
  // to USB-Serial. 0 disables it; large values (e.g. 9999) effectively do
  // the same. Useful to set short for live debugging or to silence when
  // capturing other output. Default 5.
  int    diag_pcping_sec = 5;

  // Minimum host-side milliseconds between successive characters loaded
  // into the KL11 receive buffer. Closes the burst-induced klrint re-
  // entry window when a host terminal (Arduino IDE Serial Monitor,
  // anything that sends a whole line at once) hands us several bytes
  // in microseconds. 0 = no gating (immediate, instruction-rate). For
  // V6 / RT-11 / RSTS guests under a line-buffered host, 10-50 ms is
  // typical. Honoured by kl11::poll. Default 20.
  int    diag_serialdelay_ms = 20;

  // Number of upcoming I/O-page reads/writes to log. The counter decreases
  // to zero automatically. 0 disables I/O tracing.
  int    diag_io_trace = 0;

  // Number of upcoming KW11-L/KW11-P register or interrupt events to log.
  int    diag_clock_trace = 0;

  // Number of upcoming characters read from or written to the KL11 console
  // data registers to log. The counter decreases to zero automatically.
  int    diag_console_trace = 0;

  // Per-instruction trace ring for panic diagnosis. Disabled by default
  // because it costs an MMU decode, instruction read, and register snapshot
  // on every guest instruction.
  bool   diag_trace = false;

  // [compat]
  // V4B (RSTS/E V4B) requires its probe-by-write of two non-emulated
  // device ranges to be silently absorbed in dd11 (KE11-A EAE at
  // 0o772100..0o772176 and, when serial1 is disabled, the second DL11 /
  // TT1 at 0o776500..0o776516).
  // Without absorbs, V4B's bus-error handler unconditionally HALTs.
  // RSTS V7 is the opposite case: with the TT1 absorb V7's INIT.SYS
  // sees a phantom DL11, allocates a floating vector for it, and later
  // critical devices (RK, RL) collide on the next vector and disable.
  // Default true (V4B + V6 + XXDP + RT-11 all work). Set to false to
  // attempt RSTS V7; V4B will not boot in that mode.
  bool   v4b_quirks = true;

  // KW11-P programmable real-time clock. When true the kwp.cpp device
  // is fully active (CSR/CSB/CNTR live, four clock rates, up/down and
  // repeat modes, raises INTRTC at BR6 on expiry). RSTS V7
  // INIT.SYS's hardware test wants this so it stops printing
  // "KW11-P doesn't work."; however when RSTS V4B's INIT detects a
  // working KW11-P it programs it for interrupt-driven scheduling
  // that breaks the terminal driver's case handling (typed upper
  // case echoes as lower case). Default false: stub mode (reads
  // return 0, writes absorbed, no interrupts) - matches V4B + V6 +
  // RT-11 + XXDP. Set true only when trying to bring up RSTS V7.
  bool   kwp_enabled = false;

  // [disks]
  // Slot 0..3 holds host media mounted at RL11 units DL0..DL3.
  String disk_a;
  String disk_b;
  String disk_c;
  String disk_d;
  // Optional RK05 image. When boot_kind == BK_RK we mount this at slot 0
  // (overriding disk_a) so the RK11 controller can find it as RK drive 0.
  String disk_rk0;
  // Optional secondary RH11/RP image. Not bootable in the current host; guests
  // see it as RP0 via the RH11/RP register set.
  String disk_rp0;
  String disk_rp0_type = "rp06";
  // Boot drive: which of the four physical slots is the boot disk (encoded
  // as 'a'..'d' for slot 0..3). boot_kind tells cpu_reset() which boot ROM
  // to install + which controller is being used (RL11 vs RK11).
  char   boot_drive = 'a';
  enum BootKind { BK_RL = 0, BK_RK = 1 };
  uint8_t boot_kind = BK_RL;
};

// Global config instance owned by vpdp1170.ino. Other modules read it
// (e.g. ui.cpp uses cfg.title for the System Info screen) but only the
// .ino writes - notably via config_load_wifi/_pdp inside setup().
extern AppConfig cfg;

// SD card
bool sd_mount();

// Config file IO. Split (m15): wifi credentials in /wificonfig.ini,
// everything else in /pdpconfig.ini, with /wificonfig-NAME.ini and
// /pdpconfig-NAME.ini variants the user can pick from the settings
// menu (which copies the chosen variant over the active filename).
bool config_load_wifi(AppConfig& cfg);                       // returns true if /wificonfig.ini existed; writes a default if not
bool config_load_pdp (AppConfig& cfg);                       // returns true if /pdpconfig.ini  existed; writes a default if not
bool config_write_default_wifi(const AppConfig& cfg);        // writes a fresh /wificonfig.ini
bool config_write_default_pdp (const AppConfig& cfg);        // writes a fresh /pdpconfig.ini
void config_apply_compiled_defaults(AppConfig& cfg);         // fills cfg with secrets.h + config.h defaults
void config_set_boot_input(AppConfig& cfg, const String& encoded);
String config_escape_bytes(const uint8_t* bytes, size_t len);

// SD-to-SD byte copy used by the variant picker. Truncates dst.
bool config_copy_file(const char* src, const char* dst);

// List variant files in SD root matching "<prefix>NAME.ini". Stores the
// middle portion (between prefix and ".ini") in names[i]. Returns the
// count actually stored (capped at max). Skips the active file itself.
int  config_list_variants(const char* prefix, char names[][44], int max);

// Logging helper
void config_print(const AppConfig& cfg);
