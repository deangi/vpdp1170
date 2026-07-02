#include "SD_FTP_Server.h"
#include <WiFi.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// All state is file-scope: this library is a singleton (the FTP listening
// socket is a global resource and supporting multiple instances would
// require multiplexing port numbers and listener sockets — not useful for
// the embedded use case).

// ---- Config + state -------------------------------------------------------
static SD_FTP_Server::Config g_cfg;
static bool        g_configured = false;

static WiFiServer  g_ctrl_server(21);
static WiFiClient  g_ctrl_client;
static bool        g_started    = false;
static bool        g_client_active = false;

// Passive-mode data channel: server listens, client connects to us.
static WiFiServer  g_pasv_server(0);
static WiFiClient  g_pasv_data;
static bool        g_pasv_armed = false;
static uint16_t    g_pasv_port  = 0;

// Active-mode data channel (PORT command): client gave us an addr+port we
// must connect *to* on the next transfer command. Windows ftp.exe defaults
// to active mode, so we have to support it even though PASV is preferred.
static IPAddress   g_port_addr;
static uint16_t    g_port_port  = 0;
static bool        g_port_armed = false;

static bool        g_logged_in  = false;
static char        g_cwd[128]   = "/";     // FTP-side current directory
static char        g_rnfr[128]  = {0};     // pending RNFR source
static char        g_linebuf[256];
static size_t      g_linelen    = 0;
static bool        g_line_overflow = false;
static SemaphoreHandle_t g_storage_mutex = nullptr;
static StaticSemaphore_t g_storage_mutex_buffer;
static portMUX_TYPE g_storage_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;

// 4 KiB transfer buffer in PSRAM. Tuned empirically for SD_MMC throughput;
// smaller starves the bus, larger gains nothing on PSRAM-backed memory.
// EXT_RAM_BSS_ATTR places it in PSRAM if available; falls back to DRAM.
EXT_RAM_BSS_ATTR static uint8_t g_xfer_buf[4096];

// ---- Logging helpers ------------------------------------------------------
static void log_info(const char* fmt, ...) {
  if (!g_cfg.log_fn) return;
  char buf[160];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_cfg.log_fn(buf);
}
static void log_err(const char* fmt, ...) {
  if (!g_cfg.log_err_fn) return;
  char buf[160];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_cfg.log_err_fn(buf);
}

// ---- Helpers --------------------------------------------------------------
static void send_reply(const char* line) {
  if (!g_ctrl_client) return;
  g_ctrl_client.print(line);
  g_ctrl_client.print("\r\n");
}

static void send_replyf(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  send_reply(buf);
}

static void copy_string(char* dst, size_t dstsz, const char* src) {
  if (!dst || dstsz == 0) return;
  snprintf(dst, dstsz, "%s", src ? src : "");
}

static bool same_ip(const IPAddress& a, const IPAddress& b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static bool path_is_protected(const char* ftp_path) {
  return g_cfg.path_protected_fn && g_cfg.path_protected_fn(ftp_path);
}

static bool data_write_all(const uint8_t* data, size_t bytes) {
  size_t sent = 0;
  uint32_t last_progress = millis();
  while (sent < bytes) {
    if (!g_pasv_data.connected()) return false;
    size_t n = g_pasv_data.write(data + sent, bytes - sent);
    if (n > 0) {
      sent += n;
      last_progress = millis();
    } else {
      if (millis() - last_progress > 10000) return false;
      delay(2);
    }
  }
  return true;
}

// Normalize an FTP-side path into a canonical absolute form under "/".
// Handles "..", ".", duplicate slashes, and the difference between absolute
// arguments ("/foo") and relative ones ("foo", "./foo"). Result always starts
// with "/" and never contains ".." or trailing slash (except for "/").
static bool normalize_ftp_path(const char* in, char* out, size_t outsz) {
  if (!out || outsz < 2) return false;
  char tmp[256];
  int n;
  if (!in || in[0] == 0) n = snprintf(tmp, sizeof(tmp), "%s", g_cwd);
  else if (in[0] == '/') n = snprintf(tmp, sizeof(tmp), "%s", in);
  else                   n = snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, in);
  if (n < 0 || (size_t)n >= sizeof(tmp)) return false;

  char* stack[32];
  int   depth = 0;
  char* p = tmp;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    char* comp = p;
    while (*p && *p != '/') p++;
    if (*p) *p++ = 0;
    if (comp[0] == 0 || (comp[0] == '.' && comp[1] == 0)) {
      // empty or "." -- skip
    } else if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
      if (depth > 0) depth--;
    } else if (depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
      stack[depth++] = comp;
    } else {
      return false;
    }
  }

  size_t needed = 1;
  for (int i = 0; i < depth; i++)
    needed += strlen(stack[i]) + (i + 1 < depth ? 1 : 0);
  if (needed >= outsz) return false;

  char* w = out;
  *w++ = '/';
  for (int i = 0; i < depth; i++) {
    const char* s = stack[i];
    while (*s) *w++ = *s++;
    if (i + 1 < depth) *w++ = '/';
  }
  *w = 0;
  return true;
}

// Convert an already-normalized FTP path into the VFS path used by POSIX:
// "/" -> "<vfs_root>", "/foo" -> "<vfs_root>/foo".
static bool vfs_path_of(const char* ftp_path, char* out, size_t outsz) {
  const char* root = g_cfg.vfs_root ? g_cfg.vfs_root : "/sdcard";
  int n;
  if (!ftp_path || ftp_path[0] == 0 || (ftp_path[0] == '/' && ftp_path[1] == 0)) {
    n = snprintf(out, outsz, "%s", root);
  } else {
    n = snprintf(out, outsz, "%s%s", root, ftp_path);
  }
  return n >= 0 && (size_t)n < outsz;
}

static bool resolve_path(const char* arg, char* ftp_path, size_t ftp_size,
                         char* vfs_path, size_t vfs_size) {
  if (!normalize_ftp_path(arg, ftp_path, ftp_size) ||
      !vfs_path_of(ftp_path, vfs_path, vfs_size)) {
    send_reply("553 Path too long");
    return false;
  }
  return true;
}

// ---- Lifecycle ------------------------------------------------------------
static void close_client_state() {
  if (g_pasv_armed) { g_pasv_server.stop(); g_pasv_armed = false; g_pasv_port = 0; }
  if (g_pasv_data)  { g_pasv_data.stop(); }
  if (g_ctrl_client){ g_ctrl_client.stop(); }
  g_client_active = false;
  g_port_armed = false;
  g_port_port  = 0;
  g_logged_in  = false;
  g_rnfr[0]    = 0;
  copy_string(g_cwd, sizeof(g_cwd), "/");
  g_linelen = 0;
  g_line_overflow = false;
}

static bool start_listener() {
  g_ctrl_server = WiFiServer(g_cfg.port);
  g_ctrl_server.begin();
  g_ctrl_server.setNoDelay(true);
  return true;
}

static void stop_listener() {
  close_client_state();
  g_ctrl_server.stop();
}

// ---- PASV / data channel --------------------------------------------------
// Arm a one-shot WiFiServer on port+1 and reply 227 with host+port encoded
// as P1*256+P2. The client connects to it before issuing a transfer command.
static void cmd_pasv() {
  if (g_pasv_armed) { g_pasv_server.stop(); g_pasv_armed = false; }
  uint16_t dp = (uint16_t)(g_cfg.port + 1);
  g_pasv_server = WiFiServer(dp);
  g_pasv_server.begin();
  g_pasv_server.setNoDelay(true);
  g_pasv_armed = true;
  g_pasv_port  = dp;

  IPAddress ip = WiFi.localIP();
  send_replyf("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
              ip[0], ip[1], ip[2], ip[3], (unsigned)(dp >> 8), (unsigned)(dp & 0xFF));
}

// PORT a,b,c,d,p1,p2 -- client tells us to connect TO that address for the
// next data transfer. We just stash it; the connect happens in open_data().
static void cmd_port(const char* arg) {
  unsigned a, b, c, d, p1, p2;
  if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a, &b, &c, &d, &p1, &p2) != 6) {
    send_reply("501 Bad PORT syntax"); return;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255 || p1 > 255 || p2 > 255) {
    send_reply("501 Bad PORT address"); return;
  }
  IPAddress requested(a, b, c, d);
  uint16_t requested_port = (uint16_t)(p1 * 256 + p2);
  if (!g_ctrl_client || !same_ip(requested, g_ctrl_client.remoteIP())) {
    send_reply("501 PORT address must match control client"); return;
  }
  if (requested_port == 0) {
    send_reply("501 Bad PORT number"); return;
  }
  g_port_addr  = requested;
  g_port_port  = requested_port;
  g_port_armed = true;
  // PASV and PORT are mutually exclusive; arming PORT cancels any pending PASV.
  if (g_pasv_armed) { g_pasv_server.stop(); g_pasv_armed = false; g_pasv_port = 0; }
  send_reply("200 PORT OK");
}

// Open the data channel for a transfer, honouring whichever mode the client
// armed (PASV by waiting for them to connect, PORT by dialing them). On
// failure, sends a 425 to the control channel.
static bool open_data() {
  if (g_port_armed) {
    if (!g_pasv_data.connect(g_port_addr, g_port_port)) {
      g_port_armed = false;
      send_reply("425 Can't connect for data"); return false;
    }
    g_pasv_data.setNoDelay(true);
    g_port_armed = false;          // single-shot
    return true;
  }
  if (g_pasv_armed) {
    uint32_t start = millis();
    while (millis() - start < 5000) {
      if (g_pasv_server.hasClient()) {
        g_pasv_data = g_pasv_server.available();
        g_pasv_data.setNoDelay(true);
        return true;
      }
      delay(10);
    }
    send_reply("425 Can't open data connection"); return false;
  }
  send_reply("425 Use PASV or PORT first"); return false;
}

static void close_data() {
  if (g_pasv_data) g_pasv_data.stop();
  if (g_pasv_armed) { g_pasv_server.stop(); g_pasv_armed = false; g_pasv_port = 0; }
  g_port_armed = false;
}

// ---- LIST / NLST ----------------------------------------------------------
static void format_unix_perms(char* out, mode_t m) {
  out[0] = S_ISDIR(m) ? 'd' : '-';
  out[1] = (m & S_IRUSR) ? 'r' : '-';
  out[2] = (m & S_IWUSR) ? 'w' : '-';
  out[3] = (m & S_IXUSR) ? 'x' : '-';
  out[4] = (m & S_IRGRP) ? 'r' : '-';
  out[5] = (m & S_IWGRP) ? 'w' : '-';
  out[6] = (m & S_IXGRP) ? 'x' : '-';
  out[7] = (m & S_IROTH) ? 'r' : '-';
  out[8] = (m & S_IWOTH) ? 'w' : '-';
  out[9] = (m & S_IXOTH) ? 'x' : '-';
  out[10] = 0;
}

static void cmd_list(const char* arg, bool name_only) {
  char ftp_path[160];
  char vfs[180];
  if (!resolve_path(arg, ftp_path, sizeof(ftp_path), vfs, sizeof(vfs))) return;

  if (!open_data()) return;
  SD_FTP_StorageGuard guard;
  DIR* d = opendir(vfs);
  if (!d) {
    close_data();
    send_replyf("550 Can't open directory: %s", ftp_path);
    return;
  }
  send_reply("150 Opening data connection");

  struct dirent* ent;
  uint32_t count = 0;
  bool ok = true;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.' && (ent->d_name[1] == 0 ||
        (ent->d_name[1] == '.' && ent->d_name[2] == 0))) continue;
    if (name_only) {
      char line[300];
      int n = snprintf(line, sizeof(line), "%s\r\n", ent->d_name);
      ok = n >= 0 && (size_t)n < sizeof(line) &&
           data_write_all((const uint8_t*)line, (size_t)n);
    } else {
      char child[300];
      snprintf(child, sizeof(child), "%s/%s", vfs, ent->d_name);
      struct stat st{};
      if (stat(child, &st) != 0) { st.st_mode = ent->d_type == DT_DIR ? S_IFDIR : 0; st.st_size = 0; }
      char perms[12];
      format_unix_perms(perms, st.st_mode);
      // Date fixed to "Jan 01 00:00" — FAT SD timestamps round-trip poorly
      // through ESP-IDF and FileZilla tolerates the placeholder fine.
      char line[384];
      int n = snprintf(line, sizeof(line),
                       "%s 1 esp32 esp32 %ld Jan 01 00:00 %s\r\n",
                       perms, (long)st.st_size, ent->d_name);
      ok = n >= 0 && (size_t)n < sizeof(line) &&
           data_write_all((const uint8_t*)line, (size_t)n);
    }
    if (!ok) break;
    count++;
  }
  closedir(d);
  close_data();
  if (ok) send_replyf("226 Transfer complete (%u entries)", (unsigned)count);
  else    send_reply("426 Data connection failed");
}

// ---- RETR / STOR ----------------------------------------------------------
static void cmd_retr(const char* arg) {
  char ftp_path[160];
  char vfs[180];
  if (!resolve_path(arg, ftp_path, sizeof(ftp_path), vfs, sizeof(vfs))) return;
  if (path_is_protected(ftp_path)) {
    send_reply("450 File is mounted by the emulator"); return;
  }

  if (!open_data()) return;
  SD_FTP_StorageGuard guard;
  if (path_is_protected(ftp_path)) {
    close_data();
    send_reply("450 File is mounted by the emulator");
    return;
  }
  FILE* f = fopen(vfs, "rb");
  if (!f) {
    close_data();
    send_replyf("550 File not found: %s", ftp_path);
    return;
  }
  send_replyf("150 Opening BINARY data for %s", ftp_path);

  size_t total = 0;
  bool ok = true;
  while (true) {
    size_t n = fread(g_xfer_buf, 1, sizeof(g_xfer_buf), f);
    if (n == 0) {
      if (ferror(f)) ok = false;
      break;
    }
    if (!data_write_all(g_xfer_buf, n)) {
      log_err("ftp: RETR data connection failed after %u bytes", (unsigned)total);
      ok = false;
      break;
    }
    total += n;
  }
  fclose(f);
  close_data();
  if (ok) send_replyf("226 Transfer complete (%u bytes)", (unsigned)total);
  else    send_reply("426 Transfer aborted");
}

// STOR overwrites, APPE appends — otherwise identical. Single body keeps the
// data-channel handling and timeout policy in one place.
static void cmd_stor_or_appe(const char* arg, bool append) {
  char ftp_path[160];
  char vfs[180];
  if (!resolve_path(arg, ftp_path, sizeof(ftp_path), vfs, sizeof(vfs))) return;
  if (path_is_protected(ftp_path)) {
    send_reply("450 File is mounted by the emulator"); return;
  }

  if (!open_data()) return;
  SD_FTP_StorageGuard guard;
  if (path_is_protected(ftp_path)) {
    close_data();
    send_reply("450 File is mounted by the emulator");
    return;
  }
  FILE* f = fopen(vfs, append ? "ab" : "wb");
  if (!f) {
    close_data();
    send_replyf("550 Can't create: %s", ftp_path);
    return;
  }
  send_replyf("150 Opening BINARY data for %s", ftp_path);

  size_t total = 0;
  bool ok = true;
  bool local_error = false;
  // Read with a small per-iteration timeout so a stalled client can't hang us.
  uint32_t last_data = millis();
  while (g_pasv_data.connected() || g_pasv_data.available()) {
    int n = g_pasv_data.read(g_xfer_buf, sizeof(g_xfer_buf));
    if (n > 0) {
      size_t written = fwrite(g_xfer_buf, 1, (size_t)n, f);
      if (written != (size_t)n) {
        log_err("ftp: STOR SD write failed %u/%u",
                (unsigned)written, (unsigned)n);
        ok = false;
        local_error = true;
        break;
      }
      total += (size_t)n;
      last_data = millis();
    } else {
      if (millis() - last_data > 10000) {
        ok = false;
        break;
      }
      delay(2);
    }
  }
  if (fflush(f) != 0 || ferror(f)) { ok = false; local_error = true; }
  if (fclose(f) != 0)              { ok = false; local_error = true; }
  close_data();
  if (ok)               send_replyf("226 Transfer complete (%u bytes)", (unsigned)total);
  else if (local_error) send_reply("451 Local storage error");
  else                  send_reply("426 Transfer aborted");
}

// ---- Other simple commands ------------------------------------------------
static void cmd_dele(const char* arg) {
  char p[160], vfs[180];
  if (!resolve_path(arg, p, sizeof(p), vfs, sizeof(vfs))) return;
  SD_FTP_StorageGuard guard;
  if (path_is_protected(p)) { send_reply("450 File is mounted by the emulator"); return; }
  send_replyf(unlink(vfs) == 0 ? "250 Deleted: %s" : "550 Delete failed: %s", p);
}
static void cmd_mkd(const char* arg) {
  char p[160], vfs[180];
  if (!resolve_path(arg, p, sizeof(p), vfs, sizeof(vfs))) return;
  SD_FTP_StorageGuard guard;
  send_replyf(mkdir(vfs, 0755) == 0 ? "257 \"%s\" created" : "550 mkdir failed: %s", p);
}
static void cmd_rmd(const char* arg) {
  char p[160], vfs[180];
  if (!resolve_path(arg, p, sizeof(p), vfs, sizeof(vfs))) return;
  SD_FTP_StorageGuard guard;
  if (path_is_protected(p)) { send_reply("450 Directory contains a mounted image"); return; }
  send_replyf(rmdir(vfs) == 0 ? "250 Removed: %s" : "550 rmdir failed: %s", p);
}
static void cmd_cwd(const char* arg) {
  char p[160], vfs[180];
  if (!resolve_path(arg, p, sizeof(p), vfs, sizeof(vfs))) return;
  SD_FTP_StorageGuard guard;
  struct stat st{};
  if (stat(vfs, &st) == 0 && S_ISDIR(st.st_mode)) {
    if (strlen(p) >= sizeof(g_cwd)) { send_reply("553 Path too long"); return; }
    copy_string(g_cwd, sizeof(g_cwd), p);
    send_replyf("250 Directory changed to %s", g_cwd);
  } else {
    send_replyf("550 Not a directory: %s", p);
  }
}
static void cmd_rnfr(const char* arg) {
  char p[160], vfs[180];
  if (!resolve_path(arg, p, sizeof(p), vfs, sizeof(vfs))) return;
  if (strlen(p) >= sizeof(g_rnfr)) { send_reply("553 Path too long"); return; }
  SD_FTP_StorageGuard guard;
  if (path_is_protected(p)) { send_reply("450 Path is mounted by the emulator"); return; }
  struct stat st{};
  if (stat(vfs, &st) != 0) { send_replyf("550 No such file: %s", p); return; }
  copy_string(g_rnfr, sizeof(g_rnfr), p);
  send_reply("350 Ready for RNTO");
}
static void cmd_rnto(const char* arg) {
  if (!g_rnfr[0]) { send_reply("503 RNFR first"); return; }
  char p[160], vfs_to[180];
  if (!resolve_path(arg, p, sizeof(p), vfs_to, sizeof(vfs_to))) {
    g_rnfr[0] = 0;
    return;
  }
  SD_FTP_StorageGuard guard;
  if (path_is_protected(g_rnfr) || path_is_protected(p)) {
    g_rnfr[0] = 0;
    send_reply("450 Rename path is mounted by the emulator");
    return;
  }
  char vfs_from[180];
  if (!vfs_path_of(g_rnfr, vfs_from, sizeof(vfs_from))) {
    g_rnfr[0] = 0;
    send_reply("553 Path too long");
    return;
  }
  bool ok = (rename(vfs_from, vfs_to) == 0);
  g_rnfr[0] = 0;
  send_replyf(ok ? "250 Renamed to %s" : "550 Rename failed: %s", p);
}

// ---- Command dispatch -----------------------------------------------------
static void handle_command_line(char* line) {
  // Split into verb + arg. Verb is uppercased in place. The terminator can be
  // space/tab (verb followed by arg) OR CR/LF/NUL (verb-only commands like
  // LIST or QUIT).
  char* arg = line;
  while (*arg && *arg != ' ' && *arg != '\t' && *arg != '\r' && *arg != '\n')
    { *arg = toupper((unsigned char)*arg); arg++; }
  if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
  size_t aL = strlen(arg);
  while (aL > 0 && (arg[aL-1] == '\r' || arg[aL-1] == '\n')) arg[--aL] = 0;

  const char* user_str = g_cfg.user ? g_cfg.user : "";
  const char* pass_str = g_cfg.pass ? g_cfg.pass : "";

  if      (!strcmp(line, "USER")) {
    if (user_str[0] == 0 || !strcmp(user_str, arg)) {
      send_reply(pass_str[0] ? "331 Password required" : "230 Login ok");
      if (!pass_str[0]) g_logged_in = true;
    } else send_reply("530 Bad user");
    return;
  }
  if (!strcmp(line, "PASS")) {
    if (pass_str[0] == 0 || !strcmp(pass_str, arg)) {
      g_logged_in = true; send_reply("230 Login ok");
    } else send_reply("530 Bad password");
    return;
  }
  if (!strcmp(line, "QUIT")) { send_reply("221 Goodbye"); close_client_state(); return; }
  if (!g_logged_in) { send_reply("530 Login required"); return; }

  // Logged-in commands.
  if      (!strcmp(line, "SYST")) send_reply("215 UNIX Type: L8");
  else if (!strcmp(line, "FEAT")) send_reply("211-Features:\r\n PASV\r\n211 End");
  else if (!strcmp(line, "OPTS")) send_reply("200 OK");
  else if (!strcmp(line, "NOOP")) send_reply("200 OK");
  else if (!strcmp(line, "TYPE")) send_reply("200 OK");
  else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD"))
    send_replyf("257 \"%s\"", g_cwd);
  else if (!strcmp(line, "CWD"))  cmd_cwd(arg);
  else if (!strcmp(line, "CDUP")) cmd_cwd("..");
  else if (!strcmp(line, "PASV")) cmd_pasv();
  else if (!strcmp(line, "PORT")) cmd_port(arg);
  else if (!strcmp(line, "LIST")) cmd_list(arg[0] ? arg : g_cwd, false);
  else if (!strcmp(line, "NLST")) cmd_list(arg[0] ? arg : g_cwd, true);
  else if (!strcmp(line, "RETR")) cmd_retr(arg);
  else if (!strcmp(line, "STOR")) cmd_stor_or_appe(arg, false);
  else if (!strcmp(line, "APPE")) cmd_stor_or_appe(arg, true);
  else if (!strcmp(line, "DELE")) cmd_dele(arg);
  else if (!strcmp(line, "MKD")  || !strcmp(line, "XMKD")) cmd_mkd(arg);
  else if (!strcmp(line, "RMD")  || !strcmp(line, "XRMD")) cmd_rmd(arg);
  else if (!strcmp(line, "RNFR")) cmd_rnfr(arg);
  else if (!strcmp(line, "RNTO")) cmd_rnto(arg);
  else send_replyf("502 Unknown command: %s", line);
}

// ---- Public class methods -------------------------------------------------
void SD_FTP_Server::begin(const Config& cfg) {
  sd_ftp_storage_lock();
  sd_ftp_storage_unlock();
  g_cfg        = cfg;
  g_configured = true;
  log_info("ftp: configured port %u (will start when WiFi connects)",
           (unsigned)g_cfg.port);
}

void SD_FTP_Server::poll() {
  if (!g_configured) return;

  bool wifi_up = (WiFi.status() == WL_CONNECTED);
  if (wifi_up && !g_started) {
    start_listener();
    g_started = true;
    log_info("ftp: listening on port %u at %s",
             (unsigned)g_cfg.port, WiFi.localIP().toString().c_str());
  } else if (!wifi_up && g_started) {
    stop_listener();
    g_started = false;
    log_info("ftp: WiFi down, listener stopped");
  }
  if (!g_started) return;

  // Accept new connection (single-client model).
  if (g_ctrl_server.hasClient()) {
    WiFiClient nc = g_ctrl_server.available();
    if (g_ctrl_client && g_ctrl_client.connected()) {
      nc.println("421 Console already in use");
      nc.stop();
    } else {
      g_ctrl_client = nc;
      g_ctrl_client.setNoDelay(true);
      g_client_active = true;
      g_logged_in = false;
      copy_string(g_cwd, sizeof(g_cwd), "/");
      g_rnfr[0] = 0;
      g_linelen = 0;
      g_line_overflow = false;
      log_info("ftp: client connected from %s",
               g_ctrl_client.remoteIP().toString().c_str());
      send_reply("220 SD_FTP_Server ready");
    }
  }

  if (!g_client_active || !g_ctrl_client.connected()) {
    if (g_client_active) {
      close_client_state();
      log_info("ftp: client disconnected");
    }
    return;
  }

  // Drain pending command lines (one full \r\n-terminated line at a time).
  while (g_ctrl_client.available()) {
    int c = g_ctrl_client.read();
    if (c < 0) break;
    if (c == '\n') {
      if (g_line_overflow) {
        send_reply("500 Command line too long");
      } else {
        g_linebuf[g_linelen] = 0;
        handle_command_line(g_linebuf);
      }
      g_linelen = 0;
      g_line_overflow = false;
    } else if (g_linelen < sizeof(g_linebuf) - 1) {
      g_linebuf[g_linelen++] = (char)c;
    } else {
      g_line_overflow = true;
    }
  }
}

bool     SD_FTP_Server::listening() const { return g_started; }
bool     SD_FTP_Server::connected() const { return g_ctrl_client && g_ctrl_client.connected(); }
uint16_t SD_FTP_Server::port()      const { return g_cfg.port; }

// Singleton instance.
SD_FTP_Server SDFTPServer;

void sd_ftp_storage_lock() {
  if (!g_storage_mutex) {
    portENTER_CRITICAL(&g_storage_mutex_init_mux);
    if (!g_storage_mutex)
      g_storage_mutex = xSemaphoreCreateRecursiveMutexStatic(&g_storage_mutex_buffer);
    portEXIT_CRITICAL(&g_storage_mutex_init_mux);
  }
  if (g_storage_mutex) xSemaphoreTakeRecursive(g_storage_mutex, portMAX_DELAY);
}

void sd_ftp_storage_unlock() {
  if (g_storage_mutex) xSemaphoreGiveRecursive(g_storage_mutex);
}
