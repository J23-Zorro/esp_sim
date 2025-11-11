#include "ftp_upload.h"
#include "config.h"
#include "gsm_wifi.h"
#include "log.h"
#include "ftp_utils.h"     // <--- DODANE
#include <LittleFS.h>

#ifdef ARDUINO_ARCH_ESP32
  #include <esp_task_wdt.h>
  #define FEED_WDT() do{ esp_task_wdt_reset(); }while(0)
#else
  #define FEED_WDT() do{}while(0)
#endif

// ---- util ----

static bool connectWithTimeout(Client& c, const char* host, uint16_t port, uint32_t ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (c.connect(host, port)) return true;
    FEED_WDT();
    delay(50);
  }
  return false;
}

static void sendCmd(Client& c, const String& cmd) {
  //LOGI("FTP >> %s", cmd.c_str());
  c.print(cmd); c.print("\r\n");
}

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

// zwraca ostatni 3-cyfrowy kod odpowiedzi (np. 227/150/226), czeka do linii końcowej
static int readResp(Client& c, String* lastLine=nullptr, uint32_t timeoutMs=12000) {
  String line, last;
  int code = 0, start = -1;
  unsigned long t0 = millis();
  while ((millis() - t0) < timeoutMs) {
    if (!readLine(c, line, timeoutMs)) break;
    //LOGI("FTP << %s", line.c_str());
    if (line.length() >= 3 && isDigit(line[0]) && isDigit(line[1]) && isDigit(line[2])) {
      int cur = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
      if (start < 0) start = cur;
      // wielowierszowe: "xyz-" aż do "xyz "
      if ((line.length()>3) && line[3]==' ') { code = cur; last = line; break; }
      if ((line.length()>3) && line[3]=='-') { last = line; continue; }
    }
    last = line;
    FEED_WDT();
  }
  if (lastLine) *lastLine = last;
  return code;
}

static bool parsePASV(const String& resp, String& host, uint16_t& port) {
  int l = resp.indexOf('('), r = resp.indexOf(')');
  if (l < 0 || r < 0 || r <= l) return false;
  String inside = resp.substring(l+1, r);
  int vals[6] = {0};
  int idx = 0, start = 0;
  for (int i=0; i<=inside.length() && idx<6; ++i) {
    if (i==inside.length() || inside[i]==',') {
      vals[idx++] = inside.substring(start, i).toInt();
      start = i+1;
    }
  }
  if (idx != 6) return false;
  host = String(vals[0]) + "." + String(vals[1]) + "." + String(vals[2]) + "." + String(vals[3]);
  port = (uint16_t)(vals[4]*256 + vals[5]);
  return true;
}

static bool ensureFile(const char* path){
  if (!LittleFS.exists(path)) { LOGE("File not found: %s", path); return false; }
  return true;
}

// Pomocniczo: wyciągnij samą nazwę pliku z lokalnej ścieżki
static String basenameOnly(const char* local_path) {
  String lp(local_path ? local_path : "");
  int slash = lp.lastIndexOf('/');
  if (slash >= 0 && slash + 1 < (int)lp.length()) {
    return lp.substring(slash + 1);
  }
  return lp;
}

// ---- main ----
bool FTP::uploadFile(const char* local_path, const char* remote_dir) {
  if (!ensureFile(local_path)) return false;
  const auto& cfg = Config::get();

  auto openAndLogin = [&](Client*& ctrl)->bool {
    // Otwórz kontrolny i zaloguj
    ctrl = Net::newClient();
    if (!ctrl) { LOGE("no ctrl client"); return false; }
    if (!ctrl->connect(cfg.ftp_host.c_str(), cfg.ftp_port)) {
      LOGE("FTP ctrl connect fail");
      Net::disposeClient(ctrl); ctrl = nullptr; return false;
    }
    if (readResp(*ctrl) != 220) { LOGE("FTP 220 fail"); ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; return false; }
    sendCmd(*ctrl, String("USER ") + cfg.ftp_user);
    if (readResp(*ctrl) >= 400) { LOGE("FTP USER fail"); ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; return false; }
    sendCmd(*ctrl, String("PASS ") + cfg.ftp_pass);
    if (readResp(*ctrl) >= 400) { LOGE("FTP PASS fail"); ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; return false; }
    sendCmd(*ctrl, "TYPE I");
    if (readResp(*ctrl) >= 400) { LOGE("FTP TYPE fail"); ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; return false; }
    if (remote_dir && strlen(remote_dir)) {
      sendCmd(*ctrl, String("CWD ") + remote_dir);
      int code = readResp(*ctrl);
      if (code >= 400) LOGW("FTP CWD fail (%d), continue in current dir", code);
    }
    return true;
  };

  auto drainCtrl = [&](Client& c, uint32_t ms=200)->void {
    // wyczyść zaległe odpowiedzi
    unsigned long t0 = millis(), last = t0;
    while (millis() - t0 < ms) {
      while (c.available()) { (void)c.read(); last = millis(); }
      if (millis() - last > 50) break;
      delay(1);
    }
  };

  // Zadbanie o czas (dla EPOCH w końcowej nazwie)
  (void)ensureTimeSynced(1700000000UL, 15000);

  Client* ctrl = nullptr;
  if (!openAndLogin(ctrl)) return false;

  // Nazwa „pożądana” to sama nazwa pliku z lokalnej ścieżki:
  String desiredName = basenameOnly(local_path);

  const String ctrlHost = cfg.ftp_host; // zawsze używamy hosta kontrolnego (IP z PASV ignorujemy)
  size_t totalSent = 0;
  bool ok = false;

  // Wyznacz unikalną nazwę (sprawdza istniejące pliki: SIZE/MLST/NLST)
  String uploadName = ftpUniqueName(*ctrl, desiredName, ctrlHost);
// DODAJ od razu po ftpUniqueName:
if (uploadName != desiredName) {
  LOGW("[FTP] File with desired name already exists on server: %s -> will use: %s",
       desiredName.c_str(), uploadName.c_str());
} else {
  LOGI("[FTP] Will use desired remote name: %s", desiredName.c_str());
}

  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {

    // Jeżeli kontrolny padł po poprzednim ABOR – odtwórz sesję
    if (!ctrl || !ctrl->connected()) {
      if (ctrl) { ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; }
      if (!openAndLogin(ctrl)) { LOGE("Relogin fail"); break; }
      // Po reloginie CWD ponownie (jeśli było ustawiane)
      if (remote_dir && strlen(remote_dir)) {
        sendCmd(*ctrl, String("CWD ") + remote_dir);
        (void)readResp(*ctrl);
      }
      drainCtrl(*ctrl);
      // po reloginie uploadName może już istnieć; wyznacz ponownie
      uploadName = ftpUniqueName(*ctrl, desiredName, ctrlHost);
    }

    // Najpierw EPSV (229), potem PASV (227) – ale do data łączymy się na ctrlHost
    uint16_t dataPort = 0;

    // EPSV
    sendCmd(*ctrl, "EPSV");
    String epsvLine; int code = readResp(*ctrl, &epsvLine, 10000);
    if (code == 229) {
      int l = epsvLine.indexOf('('), r = epsvLine.indexOf(')');
      if (l >= 0 && r > l) {
        String inside = epsvLine.substring(l+1, r); // "|||<port>|"
        int lastBar = inside.lastIndexOf('|');
        int prevBar = inside.lastIndexOf('|', lastBar-1);
        if (prevBar >= 0 && lastBar > prevBar) {
          uint16_t p = (uint16_t) inside.substring(prevBar+1, lastBar).toInt();
          if (p > 0) dataPort = p;
        }
      }
      if (!dataPort) LOGW("EPSV parse fail: %s", epsvLine.c_str());
    }

    // PASV fallback
    if (!dataPort) {
      sendCmd(*ctrl, "PASV");
      String pasvLine; code = readResp(*ctrl, &pasvLine, 15000);
      if (code != 227) {
        LOGE("PASV fail (%d)", code);
        if (code == 0) { ctrl->stop(); Net::disposeClient(ctrl); ctrl=nullptr; }
        continue; // następna próba
      }
      String dummyHost; uint16_t pasvPort=0;
      if (!parsePASV(pasvLine, dummyHost, pasvPort) || pasvPort==0) {
        LOGE("PASV parse fail: %s", pasvLine.c_str());
        continue;
      }
      dataPort = pasvPort;
    }

    // DATA→STOR lub STOR→DATA (elastycznie)
    Client* data = Net::newClient();
    if (!data) { LOGE("no data client"); break; }

    bool dataUp = connectWithTimeout(*data, ctrlHost.c_str(), dataPort, 8000);
    if (dataUp) {
      sendCmd(*ctrl, String("STOR ") + uploadName);
    } else {
      sendCmd(*ctrl, String("STOR ") + uploadName);
      dataUp = connectWithTimeout(*data, ctrlHost.c_str(), dataPort, 8000);
    }

    if (!dataUp) {
      LOGW("DATA connect fail %s:%u (try %d)", ctrlHost.c_str(), (unsigned)dataPort, attempt+1);
      sendCmd(*ctrl, "ABOR"); readResp(*ctrl, nullptr, 5000);
      Net::disposeClient(data);
      drainCtrl(*ctrl);
      continue;
    }

    // krótkie czekanie na 150/125 – jeśli nie ma, i tak streamujemy
    int pre = readResp(*ctrl, nullptr, 2000);
    if (pre != 150 && pre != 125) {
      LOGW("No 150 yet (code=%d), streaming anyway", pre);
    }

    // Transfer w chunkach + WDT
    File f = LittleFS.open(local_path, "r");
    if (!f) { LOGE("open local failed"); data->stop(); Net::disposeClient(data); break; }

    const size_t CHUNK = 512;
    uint8_t buf[CHUNK];
    unsigned long lastProgress = millis();

    bool failed = false;
    while (!failed) {
      size_t n = f.read(buf, sizeof(buf));
      if (n == 0) break;
      size_t off = 0;
      while (off < n) {
        if (!data->connected()) { LOGE("DATA closed"); failed = true; break; }
        int w = data->write(buf + off, n - off);
        if (w > 0) { off += (size_t)w; totalSent += (size_t)w; lastProgress = millis(); }
        else       { delay(5); }
        FEED_WDT(); delay(0);
        if (millis() - lastProgress > 8000) { LOGE("DATA stalled"); failed = true; break; }
      }
    }
    f.close();

    data->stop(); Net::disposeClient(data);

    if (failed) {
      sendCmd(*ctrl, "ABOR"); readResp(*ctrl, nullptr, 5000);
      drainCtrl(*ctrl);
      continue;
    }

    // kod końcowy 226/250 (dłuższy timeout)
    int endCode = readResp(*ctrl, nullptr, 30000);
    if (endCode != 226 && endCode != 250) {
      LOGE("End code=%d", endCode);
      sendCmd(*ctrl, "ABOR"); readResp(*ctrl, nullptr, 5000);
      drainCtrl(*ctrl);
      continue;
    }

    ok = true;
    break;
  }

  // Jeśli upload się powiódł – spróbuj zmienić nazwę na D_MAC_EPOCH.txt
  if (ok) {
    String target = finalDataName();
    if (!ftpRename(*ctrl, uploadName, target)) {
      LOGW("Rename failed: %s -> %s (file remains under interim name)", uploadName.c_str(), target.c_str());
    } else {
      LOGI("[FTP] Final rename OK: %s --> %s", uploadName.c_str(), target.c_str());
    }
  }

  // zamykanie sesji
  if (ctrl) {
    sendCmd(*ctrl, "QUIT");
    readResp(*ctrl);
    ctrl->stop(); Net::disposeClient(ctrl);
    ctrl = nullptr;
  }

  if (!ok) return false;
  LOGI("FTP upload OK: %s (%u bytes)", local_path, (unsigned)totalSent);
  return true;
}
