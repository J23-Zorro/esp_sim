#include "ftp_queue.h"
#include <LittleFS.h>
#include "log.h"
#include "ftp_upload.h"   // FTP::uploadFile(local_path, remote_dir)
#include <WiFi.h>

#ifdef ARDUINO_ARCH_ESP32
  #include <esp_task_wdt.h>
  #define FEED_WDT() do{ esp_task_wdt_reset(); }while(0)
#else
  #define FEED_WDT() do{}while(0)
#endif

namespace {

// Plik z kolejką (trwałość)
constexpr const char* kQueueFile = "/ftp_queue.txt";
// Pojemność kolejki w pamięci
constexpr size_t kMaxTasks = 128;

// Retry/backoff
constexpr uint32_t kInitialBackoffMs = 5000;    // 5s
constexpr uint32_t kMaxBackoffMs     = 120000;  // 120s
static uint8_t  gMaxRetries = 5;
static bool     gDeleteLocalOnSuccess = true;

// Zadanie w kolejce
struct Task {
  String localPath;   // np. "/logs/log1.txt"
  String remoteDir;   // np. "inbox" albo "" (bieżący katalog)
  uint8_t tries = 0;
  uint32_t backoffMs = kInitialBackoffMs;
  uint32_t nextAtMs  = 0; // millis() kiedy najwcześniej próbować
};

// Prosta „baza” kolejki w RAM
static Task gQueue[kMaxTasks];
static size_t gSize = 0;

// --- NARZĘDZIA: serializacja CSV z percent-encodingiem ---

static char hexDigit(uint8_t v){ return v<10 ? ('0'+v) : ('A'+(v-10)); }

static String pctEncode(const String& s) {
  String out; out.reserve(s.length()*2);
  for (size_t i=0;i<s.length();++i) {
    char c = s[i];
    bool safe = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='.';
    if (safe) out += c;
    else {
      uint8_t b = (uint8_t)c;
      out += '%'; out += hexDigit((b>>4)&0xF); out += hexDigit(b&0xF);
    }
  }
  return out;
}

static String pctDecode(const String& s) {
  String out; out.reserve(s.length());
  for (size_t i=0;i<s.length();) {
    if (s[i]=='%' && i+2<s.length()) {
      char h = s[i+1], l = s[i+2];
      auto val = [&](char x)->int{
        if (x>='0'&&x<='9') return x-'0';
        if (x>='A'&&x<='F') return x-'A'+10;
        if (x>='a'&&x<='f') return x-'a'+10;
        return -1;
      };
      int hi=val(h), lo=val(l);
      if (hi>=0 && lo>=0) {
        out += (char)((hi<<4)|lo);
        i+=3; continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// ... w anon namespace, obok pctEncode/pctDecode:
static String jsonEscape(const String& s) {
  String out; out.reserve(s.length() + 8);
  for (size_t i=0;i<s.length();++i) {
    char c = s[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7]; // \u00XX
          snprintf(buf, sizeof(buf), "\\u%04X", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}


static bool saveQueue() {
  File f = LittleFS.open(kQueueFile, "w");
  if (!f) { LOGE("[FTPQ] save open fail"); return false; }
  for (size_t i=0;i<gSize;++i) {
    const Task& t = gQueue[i];
    // CSV: tries,backoffMs,nextAtMs,remoteDir(local-encoded),localPath
    String line = String((unsigned)t.tries) + "," +
                  String((unsigned)t.backoffMs) + "," +
                  String((unsigned)t.nextAtMs) + "," +
                  pctEncode(t.remoteDir) + "," +
                  pctEncode(t.localPath) + "\n";
    if (f.print(line) != (int)line.length()) { f.close(); return false; }
  }
  f.close();
  return true;
}

static bool loadQueue() {
  gSize = 0;
  if (!LittleFS.exists(kQueueFile)) {
    File nf = LittleFS.open(kQueueFile, "w");
    if (!nf) { LOGE("[FTPQ] create file fail"); return false; }
    nf.close();
    return true;
  }
  File f = LittleFS.open(kQueueFile, "r");
  if (!f) { LOGE("[FTPQ] open file fail"); return false; }

  while (f.available() && gSize < kMaxTasks) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    int p1 = line.indexOf(',');
    int p2 = (p1>=0) ? line.indexOf(',', p1+1) : -1;
    int p3 = (p2>=0) ? line.indexOf(',', p2+1) : -1;
    int p4 = (p3>=0) ? line.indexOf(',', p3+1) : -1;
    if (p1<0 || p2<0 || p3<0 || p4<0) continue;

    Task t;
    t.tries     = (uint8_t) line.substring(0, p1).toInt();
    t.backoffMs = (uint32_t)line.substring(p1+1, p2).toInt();
    t.nextAtMs  = (uint32_t)line.substring(p2+1, p3).toInt();
    t.remoteDir = pctDecode(line.substring(p3+1, p4));
    t.localPath = pctDecode(line.substring(p4+1));
    if (t.backoffMs==0) t.backoffMs = kInitialBackoffMs;

    gQueue[gSize++] = t;
  }
  f.close();
  return true;
}

// usunięcie elementu 0 (po przetworzeniu) – przesuwa ogon
static void popFront() {
  if (gSize==0) return;
  for (size_t i=1;i<gSize;++i) gQueue[i-1] = gQueue[i];
  --gSize;
}

} // anon

namespace FTPQ {

bool begin() {
  bool ok = loadQueue();
  if (ok) LOGI("[FTPQ] ready, tasks=%u", (unsigned)gSize);
  return ok;
}

void setDeleteLocalOnSuccess(bool enable){ gDeleteLocalOnSuccess = enable; }
void setMaxRetries(uint8_t maxRetries){ gMaxRetries = maxRetries; }

bool enqueue(const char* local_path, const char* remote_dir) {
  if (!local_path || !*local_path) return false;
  if (gSize >= kMaxTasks) { LOGE("[FTPQ] queue full"); return false; }

  Task t;
  t.localPath = String(local_path);
  t.remoteDir = String(remote_dir ? remote_dir : "");
  t.tries     = 0;
  t.backoffMs = kInitialBackoffMs;
  t.nextAtMs  = 0;  // od razu „due”

  gQueue[gSize++] = t;
  bool ok = saveQueue();
  LOGI("[FTPQ] enqueued: local=%s, dir=%s, size=%u", t.localPath.c_str(), t.remoteDir.c_str(), (unsigned)gSize);
  return ok;
}

bool clear() {
  gSize = 0;
  return saveQueue();
}

size_t size() { return gSize; }

Stats stats() {
  Stats s{gSize, 0};
  if (gSize==0) return s;
  uint32_t now = millis();
  int32_t dt = (int32_t)(gQueue[0].nextAtMs - now);
  s.nextDueMs = dt > 0 ? (uint32_t)dt : 0;
  return s;
}

// Warunek sieci (tu: minimalnie Wi-Fi). Możesz rozbudować, jeśli chcesz GSM itd.
static bool isNetworkOk() {
  return WiFi.status() == WL_CONNECTED;
}

bool tick() {
  if (gSize == 0) return false;

  // tylko pierwszy element (FIFO)
  Task &t = gQueue[0];

  uint32_t now = millis();
  if ((int32_t)(t.nextAtMs - now) > 0) {
    // jeszcze nie pora
    return false;
  }

  if (!isNetworkOk()) {
    // odłóż na później (nie zwiększaj tries)
    t.nextAtMs = now + 3000; // 3s
    saveQueue();
    return false;
  }

  // Wysyłka
  LOGI("[FTPQ] start upload: local=%s, dir=%s, try=%u/%u",
       t.localPath.c_str(), t.remoteDir.c_str(), (unsigned)(t.tries+1), (unsigned)gMaxRetries);

  bool ok = FTP::uploadFile(t.localPath.c_str(),
                            t.remoteDir.length() ? t.remoteDir.c_str() : nullptr);

  FEED_WDT();

  if (ok) {
    LOGI("[FTPQ] upload OK: %s", t.localPath.c_str());
    // po sukcesie – opcjonalnie usuń lokalny plik
    if (gDeleteLocalOnSuccess && LittleFS.exists(t.localPath)) {
      if (LittleFS.remove(t.localPath)) {
        LOGI("[FTPQ] local deleted: %s", t.localPath.c_str());
      } else {
        LOGW("[FTPQ] local delete failed: %s", t.localPath.c_str());
      }
    }
    popFront();
    saveQueue();
    return true;
  }

  // Niepowodzenie: backoff / retry
  t.tries++;
  if (t.tries >= gMaxRetries) {
    LOGE("[FTPQ] giving up after %u tries: %s", (unsigned)t.tries, t.localPath.c_str());
    popFront();
    saveQueue();
    return true;
  }

  // backoff z eksponentą i sufitem
  t.backoffMs = (t.backoffMs >= kMaxBackoffMs/2) ? kMaxBackoffMs : (t.backoffMs * 2);
  t.nextAtMs  = now + t.backoffMs;
  LOGW("[FTPQ] retry scheduled in %u ms (try %u/%u)", (unsigned)t.backoffMs, (unsigned)t.tries+1, (unsigned)gMaxRetries);
  saveQueue();
  return true;
}

String toJson() {
  // snapshot bieżącego stanu w RAM (gQueue)
  uint32_t now = millis();
  String js = "{";
  js += "\"size\":";      js += (unsigned)gSize;
  js += ",\"nextDueMs\":"; 
  if (gSize == 0) js += "0";
  else {
    int32_t dt = (int32_t)(gQueue[0].nextAtMs - now);
    js += (dt > 0) ? String((uint32_t)dt) : "0";
  }
  js += ",\"items\":[";
  for (size_t i=0;i<gSize;++i) {
    if (i) js += ",";
    const auto &t = gQueue[i];
    int32_t due = (int32_t)(t.nextAtMs - now);
    js += "{";
      js += "\"local\":\"";    js += jsonEscape(t.localPath); js += "\"";
      js += ",\"dir\":\"";      js += jsonEscape(t.remoteDir); js += "\"";
      js += ",\"tries\":";      js += (unsigned)t.tries;
      js += ",\"backoffMs\":";  js += (unsigned)t.backoffMs;
      js += ",\"nextAtMs\":";   js += (unsigned)t.nextAtMs;
      js += ",\"dueInMs\":";    js += (due>0) ? String((uint32_t)due) : "0";
    js += "}";
  }
  js += "]}";
  return js;
}


} // namespace FTPQ
