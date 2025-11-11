#include "data_files.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>
#include "log.h"

namespace DataFiles {

// ===== MAC (bez separatorów) – cache =====
static String g_mac;

static String readMacNoSepOnce() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char b[13];
  snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

const String& macNoSep() {
  if (g_mac.length() == 0) g_mac = readMacNoSepOnce();
  return g_mac;
}

// ===== Ścieżki =====
String baseName()    { return "D_" + macNoSep(); }
String pathCurrent() { return "/" + baseName() + ".txt"; }
String path1()       { return "/" + baseName() + "_1.txt"; }
String path2()       { return "/" + baseName() + "_2.txt"; }

// ===== Narzędzia plikowe =====
size_t fileSize(const String& p) {
  if (!LittleFS.exists(p)) return 0;
  File f = LittleFS.open(p, "r");
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

bool appendLineToCurrent(const String& l) {
  const String p = pathCurrent();
  File f = LittleFS.open(p, "a");
  if (!f) {
    f = LittleFS.open(p, "w");
    if (!f) {
      LOGE("appendLineToCurrent: open '%s' failed", p.c_str());
      return false;
    }
  }
  int n = f.print(l);
  f.close();
  return n == (int)l.length();
}

// Kopia pliku 1:1 (używane do tworzenia snapshota wysyłki)
bool copyFile(const String& from, const String& to) {
  File src = LittleFS.open(from, "r");
  if (!src) { LOGE("copyFile: open src '%s' failed", from.c_str()); return false; }

  File dst = LittleFS.open(to, "w");
  if (!dst) { LOGE("copyFile: open dst '%s' failed", to.c_str()); src.close(); return false; }

  uint8_t buf[1024];
  while (true) {
    int n = src.read(buf, sizeof(buf));
    if (n < 0) { LOGE("copyFile: read err"); dst.close(); src.close(); LittleFS.remove(to); return false; }
    if (n == 0) break;
    if (dst.write(buf, n) != n) { LOGE("copyFile: write err"); dst.close(); src.close(); LittleFS.remove(to); return false; }
  }
  dst.close();
  src.close();
  return true;
}

// Rotacja po UDANEJ wysyłce bieżącego pliku:
//   D_<MAC>_1.txt -> D_<MAC>_2.txt
//   D_<MAC>.txt   -> D_<MAC>_1.txt
//   + założenie pustego D_<MAC>.txt
bool rotateAfterSend() {
  const String p0 = pathCurrent();
  const String p1 = path1();
  const String p2 = path2();

  if (LittleFS.exists(p2)) {
    if (!LittleFS.remove(p2)) {
      LOGW("rotateAfterSend: remove '%s' failed", p2.c_str());
    }
  }
  if (LittleFS.exists(p1)) {
    if (!LittleFS.rename(p1, p2)) {
      LOGE("rotateAfterSend: rename '%s'->'%s' failed", p1.c_str(), p2.c_str());
      return false;
    }
  }
  if (LittleFS.exists(p0)) {
    if (!LittleFS.rename(p0, p1)) {
      LOGE("rotateAfterSend: rename '%s'->'%s' failed", p0.c_str(), p1.c_str());
      return false;
    }
  }
  // utwórz pusty bieżący plik
  File f = LittleFS.open(p0, "w");
  if (!f) {
    LOGE("rotateAfterSend: create '%s' failed", p0.c_str());
    return false;
  }
  f.close();

  LOGI("Rotation done: %s -> %s -> %s", p0.c_str(), p1.c_str(), p2.c_str());
  return true;
}

// Zachowujemy zgodność z wcześniejszym API
bool rotate3() {
  return rotateAfterSend();
}

// Ścieżka snapshota do wysyłki (unikalna nazwa dzięki epoch)
String makeUploadSnapshotPath() {
  time_t t = time(nullptr);
  char ep[16];
  snprintf(ep, sizeof(ep), "%lu", (unsigned long)t);
  return String("/") + baseName() + "_UP_" + ep + ".txt";
}

} // namespace DataFiles
