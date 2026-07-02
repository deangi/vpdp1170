// SD_FTP_Server - Basic example
//
// Brings up WiFi, mounts the SD_MMC card at /sdcard, and starts an
// anonymous FTP server on port 21 that exposes the card's root.
//
// Connect with FileZilla/WinSCP (PASV) or Windows cmd `ftp` (active).

#include <WiFi.h>
#include <SD_MMC.h>
#include <SD_FTP_Server.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

// Optional log sinks so you can see what the FTP server is doing on Serial.
static void log_info(const char* s) { Serial.print("[ftp] ");  Serial.println(s); }
static void log_err (const char* s) { Serial.print("[ftp!] "); Serial.println(s); }

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.println();
  Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());

  // Mount the SD card at /sdcard (the default vfs_root for the FTP lib).
  // Adjust pins to match your board.
  if (!SD_MMC.begin("/sdcard", false, false, 20000)) {
    Serial.println("SD_MMC mount failed");
    return;
  }

  SD_FTP_Server::Config cfg;
  cfg.port       = 21;
  cfg.user       = "";          // empty = accept any USER
  cfg.pass       = "";          // empty = no password required
  cfg.vfs_root   = "/sdcard";
  cfg.log_fn     = log_info;
  cfg.log_err_fn = log_err;
  SDFTPServer.begin(cfg);
}

void loop() {
  SDFTPServer.poll();
  delay(2);
}
