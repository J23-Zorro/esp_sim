#include <Arduino.h>
#include <WiFi.h>         // dla MAC (Wi-Fi tryb)
#include <time.h>         // time(nullptr)
#include "gsm_wifi.h"     // Net::newClient / disposeClient
#include "log.h"

#ifdef ARDUINO_ARCH_ESP32
  #include <esp_task_wdt.h>
  #define FEED_WDT() do{ esp_task_wdt_reset(); }while(0)
#else
  #define FEED_WDT() do{}while(0)
#endif

// ==================== Sekcja: I/O komend FTP na kanale kontrolnym ====================

static bool readLine(Client& c, String& out, uint32_t timeoutMs=10000) {
  out = "";
  unsigned long t0 = millis();
  while ((millis() - t0) < timeoutMs) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\r') continue;
      if (ch == '\n') return true;
      out += ch;
    }
    if (!c.connected()) return (out.length()>0);
    FEED_WDT();
    delay(1);
  }
  return (out.length()>0);
}

// Wyślij pełną linię komendy z CRLF
bool ftpSend(Client &ctrl, const String &line) {
  String out = line;
  if (!out.endsWith("\r\n")) out += "\r\n";
  size_t n = ctrl.print(out);
  //LOGI("FTP >> %s", out.c_str());
  return n == out.length();
}

// Czyta końcowy (terminalny) kod (3 cyfry) odpowiedzi; zwraca np. 220, 213, 227, 229, 150, 226.
// Uwaga: dla odpowiedzi wielowierszowych "xyz-" czyta do "xyz "
int ftpReadCode(Client &ctrl, String *fullLineOut, uint32_t timeoutMs) {
  String line, last;
  int code = 0, start = -1;
  unsigned long t0 = millis();
  while ((millis() - t0) < timeoutMs) {
    if (!readLine(ctrl, line, timeoutMs)) break;
    //LOGI("FTP << %s", line.c_str());
    if (line.length() >= 3 && isDigit(line[0]) && isDigit(line[1]) && isDigit(line[2])) {
      int cur = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
      if (start < 0) start = cur;
      if ((line.length()>3) && line[3]==' ') { code = cur; last = line; break; }
      if ((line.length()>3) && line[3]=='-') { last = line; continue; }
    }
    last = line;
    FEED_WDT();
  }
  if (fullLineOut) *fullLineOut = last;
  return code;
}

bool ftpExpect(Client &ctrl, int expectedCode, uint32_t timeoutMs) {
  return ftpReadCode(ctrl, nullptr, timeoutMs) == expectedCode;
}

// ==================== Sekcja: PASV/EPSV i NLST ====================

// EPSV: zwraca port (IP ignorujemy — łączymy na host kontrolny)
static bool parseEPSVPortFromLine(const String& epsvLine, uint16_t &port) {
  int l = epsvLine.indexOf('('), r = epsvLine.indexOf(')', l+1);
  if (l < 0 || r <= l) return false;
  String inside = epsvLine.substring(l+1, r); // np. "|||49152|"
  int lastSep = inside.lastIndexOf('|');
  int prevSep = inside.lastIndexOf('|', lastSep-1);
  if (prevSep < 0 || lastSep <= prevSep) return false;
  port = (uint16_t) inside.substring(prevSep+1, lastSep).toInt();
  return port > 0;
}

// PASV: zwraca port (IP ignorujemy — bywa prywatne/niepoprawne)
static bool parsePASVPortFromLine(const String& pasvLine, uint16_t &port) {
  int l = pasvLine.indexOf('('), r = pasvLine.indexOf(')', l+1);
  if (l < 0 || r <= l) return false;
  String inside = pasvLine.substring(l+1, r); // "h1,h2,h3,h4,p1,p2"
  int parts[6] = {0};
  int idx = 0, start = 0;
  for (int i=0; i<=inside.length() && idx<6; ++i) {
    if (i==inside.length() || inside[i]==',') {
      parts[idx++] = inside.substring(start, i).toInt();
      start = i+1;
    }
  }
  if (idx != 6) return false;
  port = (uint16_t)(parts[4]*256 + parts[5]);
  return port > 0;
}

// Pobiera port dla połączenia danych: EPSV -> PASV (hosta użyj tego samego co kontrolny)
bool ftpEnterPassive(Client &ctrl, uint16_t &dataPort) {
  dataPort = 0;

  ftpSend(ctrl, "EPSV");
  String epsvLine; int code = ftpReadCode(ctrl, &epsvLine, 6000);
  if (code == 229) {
    if (parseEPSVPortFromLine(epsvLine, dataPort) && dataPort) return true;
    LOGW("EPSV parse fail: %s", epsvLine.c_str());
  }

  ftpSend(ctrl, "PASV");
  String pasvLine; code = ftpReadCode(ctrl, &pasvLine, 8000);
  if (code != 227) {
    LOGE("PASV fail (%d)", code);
    return false;
  }
  if (!parsePASVPortFromLine(pasvLine, dataPort) || !dataPort) {
    LOGE("PASV parse fail: %s", pasvLine.c_str());
    return false;
  }
  return true;
}

// Odczyt z NLST: sprawdza, czy w katalogu `dir` jest nazwa `fileNameOnly`
bool ftpListNamesContains(Client &ctrl, const String &ctrlHost, const String &dir, const String &fileNameOnly, uint32_t timeoutMs) {
  uint16_t dataPort = 0;
  if (!ftpEnterPassive(ctrl, dataPort)) return false;

  Client* data = Net::newClient();
  if (!data) return false;

  bool okConn = data->connect(ctrlHost.c_str(), dataPort);
  if (!okConn) {
    Net::disposeClient(data);
    return false;
  }

  // Jeśli serwer wymaga ścieżki, przekaż "NLST dir"
  String cmd = "NLST ";
  cmd += (dir.length() ? dir : ".");
  ftpSend(ctrl, cmd);
  int pre = ftpReadCode(ctrl, nullptr, 6000);
  if (!(pre == 150 || pre == 125)) {
    // niektórzy wysyłają dane bez 150/125 – spróbujemy i tak
    //LOGW("NLST pre-code: %d", pre);
  }

  // czytanie listy
  unsigned long t0 = millis();
  String buf; bool found = false;
  while (millis() - t0 < timeoutMs) {
    while (data->available()) {
      char c = (char)data->read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (buf == fileNameOnly) found = true;
        buf = "";
      } else {
        buf += c;
      }
    }
    if (!data->connected() && !data->available()) break;
    delay(0);
    FEED_WDT();
  }
  data->stop();
  Net::disposeClient(data);

  // zbierz końcowy kod (226)
  ftpReadCode(ctrl, nullptr, 4000);
  return found;
}

// ==================== Sekcja: sprawdzanie pliku i nazwy ====================

static void splitNameExt(const String &name, String &base, String &ext) {
  int dot = name.lastIndexOf('.');
  if (dot <= 0) { base = name; ext = ""; }
  else { base = name.substring(0, dot); ext = name.substring(dot); }
}

// MLST <path> -> 250 + linia zawierająca "type=file;"
static bool ftpMlstIsFile(Client &ctrl, const String &remotePath) {
  ftpSend(ctrl, "MLST " + remotePath);
  String line;
  int code = ftpReadCode(ctrl, &line, 7000);
  if (code != 250) return false;
  if (line.indexOf(F("type=file;")) >= 0) return true;
  return false;
}

// Publiczne API: sprawdza istnienie pliku (SIZE -> MLST -> NLST)
bool ftpFileExists(Client &ctrl, const String &remotePath, const String &ctrlHost) {
  // 1) SIZE
  ftpSend(ctrl, "SIZE " + remotePath);
  int code = ftpReadCode(ctrl, nullptr, 6000);
  if (code == 213) return true;
  // 550 zwykle "no such file" – ale gdy serwer blokuje SIZE, spróbuj dalej
  // 2) MLST
  if (ftpMlstIsFile(ctrl, remotePath)) return true;

  // 3) NLST w katalogu remotePath.dirname()
  String dir = ".", nameOnly = remotePath;
  int slash = remotePath.lastIndexOf('/');
  if (slash >= 0) {
    dir = remotePath.substring(0, slash);
    if (dir.length()==0) dir = "/";
    nameOnly = remotePath.substring(slash+1);
  }
  return ftpListNamesContains(ctrl, ctrlHost, dir, nameOnly, 8000);
}

// Wybór unikalnej nazwy: name, name+1.ext, name+2.ext, ...
// ZAMIEŃ swoją wersję ftpUniqueName na tę:
String ftpUniqueName(Client &ctrl, const String &name, const String &ctrlHost) {
  // Najpierw sprawdzamy oryginał
  if (!ftpFileExists(ctrl, name, ctrlHost)) {
    LOGI("[FTP] Remote name available: %s", name.c_str());
    return name;
  }

  LOGW("[FTP] Remote name already exists: %s — searching for a unique variant...", name.c_str());

  String base, ext; splitNameExt(name, base, ext);
  for (int i = 1; i <= 999; ++i) {
    String cand = base + "+" + String(i) + ext;
    if (!ftpFileExists(ctrl, cand, ctrlHost)) {
      LOGI("[FTP] Using unique remote name: %s", cand.c_str());
      return cand;
    } else {
      // Możesz zakomentować, jeśli za głośne:
      // LOGD("[FTP] Candidate also exists: %s", cand.c_str());
    }
  }
  uint32_t rnd = (uint32_t)esp_random();
  String fallback = base + "+" + String(rnd) + ext;
  LOGW("[FTP] Many candidates taken; fallback unique name: %s", fallback.c_str());
  return fallback;
}


// ==================== Sekcja: MAC/EPOCH i rename ====================

String macNoSep() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool ensureTimeSynced(uint32_t minEpoch /*=1700000000*/, uint32_t waitMs /*=15000*/) {
  time_t now = time(nullptr);
  if ((uint32_t)now >= minEpoch) return true;

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  unsigned long t0 = millis();
  while (millis() - t0 < waitMs) {
    now = time(nullptr);
    if ((uint32_t)now >= minEpoch) return true;
    delay(200);
    FEED_WDT();
  }
  return false;
}

String finalDataName() {
  return "D_" + macNoSep() + "_" + String((uint32_t)time(nullptr)) + ".txt";
}

bool ftpRename(Client &ctrl, const String &from, const String &to) {
  ftpSend(ctrl, "RNFR " + from);
  if (ftpReadCode(ctrl, nullptr, 8000) != 350) return false;
  ftpSend(ctrl, "RNTO " + to);
  return ftpExpect(ctrl, 250, 8000);
}

// ==================== (opcjonalnie) wrapper dla gotowego uploadu ====================
// storUploadFn powinno wykonać STOR i przesłać lokalny plik (ctrl musi być zalogowany i w dobrym CWD)
bool ftpUploadWithUniqueThenRename(
  Client &ctrl,
  const String &ctrlHost,
  const char* localPath,
  const String &desiredRemoteName,
  bool (*storUploadFn)(Client&, const String&, const char*)
) {
  if (!ensureTimeSynced(1700000000UL, 15000)) {
    LOGW("Time not synced; EPOCH may be wrong");
  }

  String uniqueName = ftpUniqueName(ctrl, desiredRemoteName, ctrlHost);

  if (!storUploadFn(ctrl, uniqueName, localPath)) {
    LOGE("Upload failed for %s", uniqueName.c_str());
    return false;
  }

  String target = finalDataName();
  if (!ftpRename(ctrl, uniqueName, target)) {
    LOGW("Rename failed: %s -> %s (file remains under interim name)", uniqueName.c_str(), target.c_str());
    return true; // plik wgrany, brak finalnego RNTO
  }
  LOGI("Upload ok. Renamed to %s", target.c_str());
  return true;
}
