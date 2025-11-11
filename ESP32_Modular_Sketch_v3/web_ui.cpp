#include "web_ui.h"
#include "config.h"
#include "log.h"
#include "ftp_upload.h"
#include "ftp_queue.h"
#include "gsm_wifi.h"

#include <WebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "measurement.h"
#include "data_files.h"
#include "alarm.h"
#include "alarm_config.h"
#include "io_pins.h"

#include <vector>
#include <algorithm>

#include "email_config.h"
#include "email_client.h"   // Email::Ack, Email::pop3CheckForEpoch, Email::sendSMTP
#include "email_alert.h"    // EmailAlert::...
#include "cfg_sync.h"

static WebServer server(80);

// --- auth (Basic Auth) ---
static bool auth() {
  auto& c = Config::get();
  return server.authenticate(c.http_user.c_str(), c.http_pass.c_str());
}





// POST /cfgsync/run  — sprawdź teraz
static void handleCfgSyncRun(){
  if (!auth()) { return server.requestAuthentication(); }
  bool ok = CfgSync::runOnceNow();
  server.send(200, "text/plain", ok ? "OK (delta applied)" : "NO (no file or failure)");
}





// (stary alias – już nieużywany, ale zostawiony dla zgodności)
//static void handleRoot() { sendIndex(); }

// --- tail ostatnich N linii z pliku (czyta od końca) ---
static String tailLastLines(const String& path, size_t maxLines) {
  if (maxLines == 0) return "";
  if (!LittleFS.exists(path)) return "";

  File f = LittleFS.open(path, "r");
  if (!f) return "";
  size_t size = f.size();
  if (size == 0) { f.close(); return ""; }

  const size_t CHUNK = 1024;
  size_t pos = size;
  String acc;
  size_t linesFound = 0;

  while (pos > 0 && linesFound <= maxLines) {
    size_t toRead = (pos >= CHUNK) ? CHUNK : pos;
    pos -= toRead;
    f.seek(pos, SeekSet);

    static char buf[CHUNK + 1];
    size_t r = f.readBytes(buf, toRead);
    buf[r] = '\0';

    for (size_t i = 0; i < r; ++i) if (buf[i] == '\n') ++linesFound;

    String chunk(buf);
    acc = chunk + acc;
  }
  f.close();

  int need = (int)maxLines;
  int i = acc.length() - 1, nl = 0;
  for (; i >= 0; --i) {
    if (acc[i] == '\n') { ++nl; if (nl == need) { ++i; break; } }
  }
  int start = (i < 0) ? 0 : i;
  return acc.substring(start);
}

// --- LittleFS podgląd ---
static void handleFS() {
  if (!auth()) { return server.requestAuthentication(); }
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  String html = "<!doctype html><meta charset='utf-8'><title>FS</title>";
  html += "<h1>LittleFS</h1>";
  html += "<p>Total: " + String((unsigned long)total) + " B, Used: " + String((unsigned long)used) + " B</p><ul>";
  File root = LittleFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      html += "<li>" + String(f.name()) + " (" + String((unsigned long)f.size()) + " B)</li>";
      f = root.openNextFile();
    }
  }
  html += "</ul><p><a href='/'>← powrót</a></p>";
  server.send(200, "text/html", html);
}

// --- /status (JSON) ---
static void handleStatus() {
  JsonDocument doc;
  auto& cfg = Config::get();
  JsonObject cfgObj = doc["config"].to<JsonObject>();  // v7 API

  cfgObj["net_mode"] = cfg.net_mode;
  cfgObj["wifi_ssid"] = cfg.wifi_ssid;
  cfgObj["wifi_pass"] = cfg.wifi_pass;
  cfgObj["apn"] = cfg.apn;
  cfgObj["pin"] = cfg.pin;
  cfgObj["uart_tx"] = cfg.uart_tx;
  cfgObj["uart_rx"] = cfg.uart_rx;
  cfgObj["uart_baud"] = cfg.uart_baud;
  cfgObj["mqtt_host"] = cfg.mqtt_host;
  cfgObj["mqtt_port"] = cfg.mqtt_port;
  cfgObj["mqtt_user"] = cfg.mqtt_user;
  cfgObj["mqtt_pass"] = cfg.mqtt_pass;
  cfgObj["mqtt_topic_pub"] = cfg.mqtt_topic_pub;
  cfgObj["ftp_host"] = cfg.ftp_host;
  cfgObj["ftp_port"] = cfg.ftp_port;
  cfgObj["ftp_user"] = cfg.ftp_user;
  cfgObj["ftp_pass"] = cfg.ftp_pass;
  cfgObj["ftp_dir"] = cfg.ftp_dir;
  cfgObj["http_user"] = cfg.http_user;
  cfgObj["http_pass"] = cfg.http_pass;

  // info o wysyłce
  cfgObj["sendFTPInterval"] = (int)cfg.sendFTPInterval_sec;
  cfgObj["cfgSyncInterval_sec"] = cfg.cfgSyncInterval_sec;
  doc["online"] = Net::connected();
  doc["ip"] = Net::wifiIpStr();
  doc["queue_size"] = (int)FTPQ::size();
  doc["sendFTPInterval_sec"] = (int)Measure::getSendFTPInterval();

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- /config (POST) ---
static void handleConfigPost() {
  if (!auth()) { return server.requestAuthentication(); }
  ConfigData d = Config::get();

  if (server.hasArg("net_mode")) d.net_mode = server.arg("net_mode");
  if (server.hasArg("wifi_ssid")) d.wifi_ssid = server.arg("wifi_ssid");
  if (server.hasArg("wifi_pass")) d.wifi_pass = server.arg("wifi_pass");

  if (server.hasArg("apn")) d.apn = server.arg("apn");
  if (server.hasArg("pin")) d.pin = server.arg("pin");
  if (server.hasArg("uart_tx")) d.uart_tx = server.arg("uart_tx").toInt();
  if (server.hasArg("uart_rx")) d.uart_rx = server.arg("uart_rx").toInt();
  if (server.hasArg("uart_baud")) d.uart_baud = server.arg("uart_baud").toInt();

  if (server.hasArg("mqtt_host")) d.mqtt_host = server.arg("mqtt_host");
  if (server.hasArg("mqtt_port")) d.mqtt_port = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) d.mqtt_user = server.arg("mqtt_user");
  if (server.hasArg("mqtt_pass")) d.mqtt_pass = server.arg("mqtt_pass");
  if (server.hasArg("mqtt_topic_pub")) d.mqtt_topic_pub = server.arg("mqtt_topic_pub");

  if (server.hasArg("ftp_host")) d.ftp_host = server.arg("ftp_host");
  if (server.hasArg("ftp_port")) d.ftp_port = server.arg("ftp_port").toInt();
  if (server.hasArg("ftp_user")) d.ftp_user = server.arg("ftp_user");
  if (server.hasArg("ftp_pass")) d.ftp_pass = server.arg("ftp_pass");
  if (server.hasArg("ftp_dir")) d.ftp_dir = server.arg("ftp_dir");

  if (server.hasArg("http_user")) d.http_user = server.arg("http_user");
  if (server.hasArg("http_pass")) d.http_pass = server.arg("http_pass");

  // NOWE: interwał wysyłki FTP z formularza /config (jeśli chcesz z tego endpointa też zmieniać)
  if (server.hasArg("sendFTPInterval")) {
    uint32_t v = strtoul(server.arg("sendFTPInterval").c_str(), nullptr, 10);
    if (v < 60) v = 60;
    if (v > 24UL*3600UL) v = 24UL*3600UL;
    d.sendFTPInterval_sec = v;
  }
  if (server.hasArg("cfgSyncInterval_sec")) {
  uint32_t v = strtoul(server.arg("cfgSyncInterval_sec").c_str(), nullptr, 10);
  if (v < 60) v = 60;
  if (v > 24UL*3600UL) v = 24UL*3600UL;
  d.cfgSyncInterval_sec = v;
}

  Config::save(d);
  // natychmiast w życie:
  Measure::setSendFTPInterval(d.sendFTPInterval_sec);
  CfgSync::setInterval(d.cfgSyncInterval_sec);

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved");
  delay(100);
  ESP.restart();
}

// --- /reboot ---
static void handleReboot() {
  if (!auth()) { return server.requestAuthentication(); }
  server.send(200, "text/plain", "Rebooting...");
  delay(100);
  ESP.restart();
}
//
// 404 + zarządzanie index.html (upload + delete)
static void handleNotFound404() {
  // Nie wymuszamy auth na samą stronę 404 – formularze i tak wywołają chronione endpointy
  const bool hasIndex = LittleFS.exists("/index.html");
  unsigned long sz = 0;
  if (hasIndex) { File f = LittleFS.open("/index.html", "r"); if (f) { sz = (unsigned long)f.size(); f.close(); } }

  String html;
  html.reserve(3000);
  html += "<!doctype html><html><head><meta charset='utf-8'><title>Not found</title>"
          "<style>"
          "body{font-family:sans-serif;margin:20px}"
          ".card{border:1px solid #ddd;border-radius:8px;padding:12px;margin:10px 0}"
          ".btn{display:inline-block;margin:4px 6px 0 0;padding:6px 10px;border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000;background:#fff}"
          ".btn:hover{background:#eee}"
          "code{background:#f5f5f5;padding:2px 4px;border-radius:4px}"
          "</style></head><body>";

  html += "<h1>404 – Not found</h1>";
  html += "<p>Żądana ścieżka: <code>" + server.uri() + "</code></p>";

  html += "<div class='card'><h3>index.html</h3>";
  if (hasIndex) {
    html += "<p>Plik <code>/index.html</code> istnieje (" + String(sz) + " B).</p>";
  } else {
    html += "<p>Pliku <code>/index.html</code> brak.</p>";
  }

  // Upload (POST /upload – istniejący handler)
  html += "<h4>Wgraj/aktualizuj index.html</h4>"
          "<form method='POST' action='/upload' enctype='multipart/form-data'>"
          "<input type='file' name='upfile' accept='.html,.htm' required> "
          "<button class='btn' type='submit'>Wyślij</button>"
          "</form>";

  // Delete (POST /index_delete – handler poniżej)
  html += "<h4>Usuń index.html</h4>"
          "<form method='POST' action='/index_delete' onsubmit='return confirm(\"Skasować /index.html?\");'>"
          "<button class='btn' type='submit'>Usuń</button>"
          "</form>";

  html += "<p><a class='btn' href='/'>Strona główna</a>"
          "<a class='btn' href='/menu'>Menu</a>"
          "<a class='btn' href='/upload'>Upload (pełna strona)</a></p>";

  html += "</div></body></html>";

  server.send(404, "text/html", html);
}

// POST: usuń /index.html (z auth)
static void handleIndexDelete() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!LittleFS.exists("/index.html")) {
    server.send(404, "text/plain", "no /index.html");
    return;
  }
  bool ok = LittleFS.remove("/index.html");
  if (ok) {
    server.send(200, "text/plain", "deleted");
  } else {
    server.send(500, "text/plain", "delete failed");
  }
}

// --- /ftp_test ---
static void handleFTPTest() {
  if (!auth()) { return server.requestAuthentication(); }
  bool ok = FTP::uploadFile("/test.txt", Config::get().ftp_dir.c_str());
  server.send(200, "text/plain", ok? "FTP OK" : "FTP FAIL");
}

// --- /ftp_enqueue_test ---
static void handleFTPEnqueueTest() {
  if (!auth()) { return server.requestAuthentication(); }
  bool ok = FTPQ::enqueue("/test.txt", Config::get().ftp_dir.c_str());
  server.send(200, "text/plain", ok? "ENQUEUED" : "ENQUEUE FAIL");
}

// --- /logs + /download + /ftp_queue_clear ---
static bool isAllowedLogFile(const String& name) {
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (!name.startsWith("log")) return false;
  if (!name.endsWith(".txt")) return false;
  return true;
}

// Usuwa najstarsze logi tak, by zostało co najwyżej 'keep' plików
static void pruneLogs(size_t keep = 5) {
  std::vector<String> names;
  File root = LittleFS.open("/");
  if (!root) return;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = String(f.name());
    if (isAllowedLogFile(name)) names.push_back(name);
  }
  if (names.size() <= keep) return;
  std::sort(names.begin(), names.end(), [](const String& a, const String& b){ return a < b; });
  size_t toDelete = names.size() - keep;
  for (size_t i = 0; i < toDelete; ++i) {
    String path = "/" + names[i];
    if (!LittleFS.remove(path)) {
      LOGW("pruneLogs: remove failed: %s", path.c_str());
    } else {
      LOGI("pruneLogs: removed old log: %s", path.c_str());
    }
  }
}

static void handleLogsIndex() {
  if (!auth()) { return server.requestAuthentication(); }
  pruneLogs(5);
  String html = "<!doctype html><html><head><meta charset='utf-8'><title>Logs</title>"
                "<style>body{font-family:sans-serif;margin:20px} li{margin:4px 0}</style>"
                "</head><body>";
  html += "<h1>Log files</h1><ul>";
  std::vector<String> names;
  File root = LittleFS.open("/");
  if (root) {
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      String name = String(f.name());
      if (isAllowedLogFile(name)) names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end(), [](const String& a, const String& b){ return a < b; });
  for (auto& name : names) {
    String path = "/" + name;
    File f = LittleFS.open(path, "r");
    unsigned long sz = f ? (unsigned long)f.size() : 0;
    if (f) f.close();
    html += "<li><a href='/download?file=" + name + "'>" + name + "</a> (" + String(sz) + " B)"
            " &nbsp; <a href='/log_delete?file=" + name + "' onclick='return confirm(\"Skasować " + name + "?\");'>[usuń]</a></li>";
  }
  html += "</ul><p><a href='/'>← powrót</a></p></body></html>";
  server.send(200, "text/html", html);
}

static void handleLogDelete() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("file")) { server.send(400, "text/plain", "missing file"); return; }
  String name = server.arg("file");
  if (!isAllowedLogFile(name)) { server.send(403, "text/plain", "forbidden"); return; }
  String path = "/" + name;
  if (!LittleFS.exists(path)) { server.send(404, "text/plain", "not found"); return; }
  bool ok = LittleFS.remove(path);
  if (!ok) { server.send(500, "text/plain", "delete failed"); return; }
  pruneLogs(5);
  server.sendHeader("Location", "/logs");
  server.send(302, "text/plain", "deleted");
}

static void handleDownload() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("file")) { server.send(400, "text/plain", "missing file"); return; }
  String name = server.arg("file");
  if (!isAllowedLogFile(name)) { server.send(403, "text/plain", "forbidden"); return; }
  String path = "/" + name;
  if (!LittleFS.exists(path)) { server.send(404, "text/plain", "not found"); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  server.streamFile(f, "text/plain"); f.close();
}

static void handleFTPQueueClear() {
  if (!auth()) { return server.requestAuthentication(); }
  FTPQ::clear();
  server.send(200, "text/plain", "QUEUE CLEARED");
}

/* ====== MAIL ====== */

static void handleMailPage(){
  if (!auth()) { return server.requestAuthentication(); }
  EmailCfg::Settings s; EmailCfg::load(s);

  String h = "<!doctype html><meta charset='utf-8'><title>E-mail</title>"
             "<style>body{font-family:sans-serif;margin:20px} input,textarea{width:100%} .row{display:grid;grid-template-columns:180px 1fr;gap:10px;align-items:center} .card{border:1px solid #ddd;border-radius:8px;padding:12px;margin-bottom:12px}</style>";
  h += "<h1>Powiadomienia e-mail</h1>";

  h += "<form method='POST' action='/mail/save'>";
  h += "<div class='card'><h3>Konto</h3>";
  h += "<div class='row'><label>SMTP host</label><input name='smtp_host' value='"+s.smtp_host+"'></div>";
  h += "<div class='row'><label>SMTP port</label><input name='smtp_port' value='"+String(s.smtp_port)+"'></div>";
  h += "<div class='row'><label>POP3 host</label><input name='pop3_host' value='"+s.pop3_host+"'></div>";
  h += "<div class='row'><label>POP3 port</label><input name='pop3_port' value='"+String(s.pop3_port)+"'></div>";
  h += "<div class='row'><label>Użytkownik</label><input name='user' value='"+s.user+"'></div>";
  h += "<div class='row'><label>Hasło</label><input type='password' name='pass' value='"+s.pass+"'></div>";
  h += "<div class='row'><label>Nadawca (From)</label><input name='sender' value='"+s.sender+"'></div>";
  h += "<div class='row'><label>Włączone</label><input name='enabled' value='"+String(s.enabled?1:0)+"'></div>";
  h += "</div>";

  h += "<div class='card'><h3>Grupy adresów</h3>";
  h += "<div class='row'><label>Grupa 1 (max 10)</label><input name='g1' value='"+s.group1_csv+"'></div>";
  h += "<div class='row'><label>Grupa 2 (max 5)</label><input name='g2' value='"+s.group2_csv+"'></div>";
  h += "<div class='row'><label>Grupa 3 (max 2)</label><input name='g3' value='"+s.group3_csv+"'></div>";
  h += "</div>";

  h += "<div class='card'><h3>Czasy oczekiwania (sekundy)</h3>";
  h += "<div class='row'><label>Grupa 1</label><input name='w1' value='"+String(s.wait_g1)+"'></div>";
  h += "<div class='row'><label>Grupa 2</label><input name='w2' value='"+String(s.wait_g2)+"'></div>";
  h += "<div class='row'><label>Grupa 3</label><input name='w3' value='"+String(s.wait_g3)+"'></div>";
  h += "</div>";

  h += "<div class='card'><h3>Treści</h3>";
  h += "<div class='row'><label>Body alarm (max 256)</label><textarea name='ba' maxlength='256'>"+s.body_alarm+"</textarea></div>";
  h += "<div class='row'><label>Body powrót (max 256)</label><textarea name='br' maxlength='256'>"+s.body_recover+"</textarea></div>";
  h += "</div>";

  h += "<p><button type='submit'>Zapisz</button></p></form>";

  h += "<div class='card'><h3>Test</h3>";
  h += "<form method='POST' action='/mail/test'><div class='row'><label>Wyślij do</label><input name='to'></div>";
  h += "<div class='row'><label>Temat</label><input name='subj' value='Test ESP32'></div>";
  h += "<div class='row'><label>Treść</label><textarea name='body'>Wiadomość testowa</textarea></div>";
  h += "<button type='submit'>Wyślij</button></form>";
  h += "<p><a href='/mail/check'>Ręcznie sprawdź skrzynkę (POP3)</a></p>";
  h += "<p><a href='/menu'>← powrót</a></p></div>";

  server.send(200, "text/html", h);
}

static void handleMailSave(){
  if (!auth()) { return server.requestAuthentication(); }
  EmailCfg::Settings s; EmailCfg::load(s);

  if (server.hasArg("smtp_host")) s.smtp_host = server.arg("smtp_host");
  if (server.hasArg("smtp_port")) s.smtp_port = server.arg("smtp_port").toInt();
  if (server.hasArg("pop3_host")) s.pop3_host = server.arg("pop3_host");
  if (server.hasArg("pop3_port")) s.pop3_port = server.arg("pop3_port").toInt();
  if (server.hasArg("user")) s.user = server.arg("user");
  if (server.hasArg("pass")) s.pass = server.arg("pass");
  if (server.hasArg("sender")) s.sender = server.arg("sender");
  if (server.hasArg("enabled")) s.enabled = (server.arg("enabled").toInt()!=0);
  if (server.hasArg("g1")) s.group1_csv = server.arg("g1");
  if (server.hasArg("g2")) s.group2_csv = server.arg("g2");
  if (server.hasArg("g3")) s.group3_csv = server.arg("g3");
  if (server.hasArg("w1")) s.wait_g1 = server.arg("w1").toInt();
  if (server.hasArg("w2")) s.wait_g2 = server.arg("w2").toInt();
  if (server.hasArg("w3")) s.wait_g3 = server.arg("w3").toInt();
  if (server.hasArg("ba")) s.body_alarm = server.arg("ba");
  if (server.hasArg("br")) s.body_recover = server.arg("br");

  EmailCfg::save(s);
  server.sendHeader("Location", "/mail");
  server.send(302, "text/plain", "saved");
}

static void handleMailTest(){
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("to")) { server.send(400,"text/plain","missing 'to'"); return; }
  String to = server.arg("to");
  String subj = server.hasArg("subj")? server.arg("subj") : "Test";
  String body = server.hasArg("body")? server.arg("body") : "Hello";
  bool ok = EmailAlert::testSend(to, subj, body);
  server.send(200, "text/plain", ok? "OK" : "FAIL");
}

static void handleMailCheck(){
  if (!auth()) { return server.requestAuthentication(); }
  Email::Ack ack; // placeholder – logika akceptacji w EmailAlert
  bool ok = false;
  server.send(200, "text/plain", ok? "ACK found" : "No ACK");
}

/* ===== Wrapper z przyciskiem MENU + iframe index.html ===== */

static void handleRootWrapper() {
  String html = F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Panel</title>"
    "<style>"
    "body{margin:0;font-family:sans-serif}"
    ".bar{display:flex;gap:.5rem;align-items:center;padding:.5rem 1rem;background:#111;color:#fff}"
    ".bar a,.bar button{background:#2b7cff;color:#fff;border:0;border-radius:6px;padding:.4rem .75rem;cursor:pointer;text-decoration:none}"
    ".bar .sp{flex:1}"
    "iframe{border:0;width:100%;height:calc(100vh - 48px)}"
    "</style></head><body>"
    "<div class='bar'>"
      "<strong>ESP32 Panel</strong>"
      "<span class='sp'></span>"
      "<a href='/menu'>Menu</a>"
    "</div>"
    "<iframe src='/ui_index'></iframe>"
    "</body></html>"
  );
  server.send(200, "text/html", html);
}

static void handleUIIndex() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(200, "text/html",
      "<!doctype html><meta charset='utf-8'><h2>Brak /index.html</h2>"
      "<p>Wgraj plik przez <a href='/upload'>Upload</a> lub użyj <a href='/menu'>Menu</a>.</p>");
    return;
  }
  server.streamFile(f, "text/html"); f.close();
}

/* ===== MENU – scalona wersja (status + linki + akcje + interwał FTP) ===== */

static void handleMenu() {
  if (!auth()) { return server.requestAuthentication(); }

  auto& cfg = Config::get();
  const bool online = Net::connected();
  const String ip   = Net::wifiIpStr();
  const int qsize   = (int)FTPQ::size();
  const uint32_t curFTP = Measure::getSendFTPInterval();

  String html;
  html.reserve(7000);
  html += "<!doctype html><html><head><meta charset='utf-8'><title>Panel</title>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<style>"
          "body{font-family:sans-serif;margin:20px}"
          ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}"
          ".card{border:1px solid #ddd;border-radius:10px;padding:12px}"
          "h1{margin-top:0}"
          ".pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid #ccc;margin-left:8px}"
          ".ok{background:#e9fbe9;border-color:#b8e6b8}"
          ".warn{background:#fff7e6;border-color:#ffd27f}"
          ".bad{background:#ffecec;border-color:#ffbaba}"
          ".btn{display:inline-block;margin:4px 6px 0 0;padding:6px 10px;border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000;background:#fff}"
          ".btn:hover{background:#eee}"
          "form{display:inline-block;margin:0}"
          "input[type=number]{width:110px}"
          "code{background:#f5f5f5;padding:2px 4px;border-radius:4px}"
          "ul{margin:0;padding-left:18px}"
          "</style></head><body>";

  html += "<h1>Panel nawigacyjny</h1><div class='grid'>";

  // STATUS
  html += "<div class='card'><h3>Status</h3>";
  html += "<p>Sieć: "; 
  html += online ? "<span class='pill ok'>online</span>" : "<span class='pill bad'>offline</span>";
  html += "</p>";
  html += "<p>IP Wi-Fi: <code>" + ip + "</code></p>";
  html += "<p>Kolejka FTP: <span class='pill " + String(qsize>0?"warn":"ok") + "'>" + String(qsize) + "</span></p>";
  html += "<p>FTP host: <code>" + cfg.ftp_host + ":" + String(cfg.ftp_port) + "</code></p>";
  html += "</div>";

  // LINKI – podstrony
  html += "<div class='card'><h3>Podstrony</h3><ul>";
  html += "<li><a class='btn' href='/measure'>Pomiary / Dane</a></li>";
  html += "<li><a class='btn' href='/alarm'>Alarmy (status + konfiguracja)</a></li>";
  html += "<li><a class='btn' href='/mail'>E-mail (powiadomienia)</a></li>";
  html += "<li><a class='btn' href='/fs'>Pliki (LittleFS)</a></li>";
  html += "<li><a class='btn' href='/logs'>Logi (pobierz)</a></li>";
  html += "<li><a class='btn' href='/upload'>Upload do LittleFS</a></li>";
  html += "<li><a class='btn' href='/status' target='_blank'>Status (JSON)</a></li>";
  html += "<li><a class='btn' href='/ftpq/stats' target='_blank'>Kolejka FTP – statystyki</a></li>";
  html += "<li><a class='btn' href='/'>Strona główna (index.html)</a></li>";
  html += "</ul></div>";

  // AKCJE + INTERWAŁ FTP
  html += "<div class='card'><h3>Akcje</h3>";
  html += "<div>";
  html += "<a class='btn' href='/ftp_test'>Test FTP</a>";
  html += "<a class='btn' href='/ftp_enqueue_test'>Dodaj /test.txt do kolejki</a>";
  html += "<a class='btn' href='/ftp_queue_clear'>Wyczyść kolejkę FTP</a>";
  html += "<a class='btn' href='/reboot'>Reboot</a>";
  html += "</div>";
  html += "<hr>";
  html += "<div>";
  html += "<form method='POST' action='/measure/rotate_send'><button class='btn' type='submit'>Rotuj bieżący plik i wyślij (enqueue)</button></form>";
  html += "</div>";
  html += "<div style='margin-top:12px'>";
  html += "<form method='POST' action='/set_send_interval'>"
          "<label>Ustaw interwał wysyłki na serwer FTP (sekundy)</label><br>"
          "<input type='number' min='60' max='86400' step='60' name='sec' value='" + String((unsigned long)curFTP) + "' required> "
          "<button class='btn' type='submit'>Zapisz</button></form>";
  html += "</div>";
  html += "</div>"; // /card
  // ... Zdalna konfiguracja FTP ...
  html += "<div class='card'><h3>Zdalna konfiguracja (FTP)</h3>";
  html += "<p>Sprawdza cyklicznie, czy na serwerze istnieje <code>configZ.txt</code>.<br>"
          "Jeśli tak — najpierw wysyła pełny <code>configU_&lt;epoch&gt;.txt</code>, potem pobiera <code>configZ.txt</code>, "
          "aplikuje różnice i zmienia nazwę zdalnego pliku na <code>configZ_&lt;epoch&gt;.txt</code>.</p>";
  html += "<form method='POST' action='/cfgsync/run' style='display:inline-block;margin-right:8px'>"
          "<button class='btn' type='submit'>Sprawdź teraz</button></form>";
  html += "<form method='POST' action='/cfgsync/interval' style='display:inline-block'>"
          "Interwał (sek): <input type='number' name='sec' min='60' max='86400' value='" + String((unsigned long)CfgSync::getInterval()) + "'> "
          "<button class='btn' type='submit'>Zapisz</button></form>";
  html += "</div>";

  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

// --- POST: zapisz nowy interwał wysyłki FTP ---
static void handleSetSendIntervalPost() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("sec")) { server.send(400, "text/plain", "missing sec"); return; }
  uint32_t sec = strtoul(server.arg("sec").c_str(), nullptr, 10);
  if (sec < 60) sec = 60;
  if (sec > 24UL*3600UL) sec = 24UL*3600UL;

  ConfigData d = Config::get();
  d.sendFTPInterval_sec = sec;
  Config::save(d);
  Measure::setSendFTPInterval(sec);

  server.sendHeader("Location", "/menu");
  server.send(302, "text/plain", "OK");
}
// POST /cfgsync/interval
static void handleCfgSyncInterval(){
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("sec")) { server.send(400,"text/plain","missing sec"); return; }
  uint32_t sec = strtoul(server.arg("sec").c_str(), nullptr, 10);
  if (sec < 60) sec = 60;
  if (sec > 24UL*3600UL) sec = 24UL*3600UL;

  // zapisz do Config
  ConfigData d = Config::get();
  d.cfgSyncInterval_sec = sec;
  Config::save(d);

  // i w życie
  CfgSync::setInterval(sec);

  server.sendHeader("Location","/menu");
  server.send(302,"text/plain","OK");
}

/* ===== MEASURE panel ===== */

static void handleMeasurePage() {
  if (!auth()) { return server.requestAuthentication(); }

  const String curPath  = DataFiles::pathCurrent();
  const size_t curSize  = DataFiles::fileSize(curPath);
  const uint32_t interval = Measure::pomiarADCInterval;

  size_t n = 50;
  if (server.hasArg("n")) {
    long v = server.arg("n").toInt();
    if (v < 1) v = 1;
    if (v > 2000) v = 2000;
    n = (size_t)v;
  }

  String qjson = FTPQ::toJson();
  String preview = tailLastLines(curPath, n);

  String html;
  html.reserve(4096);
  html += "<!doctype html><html><head><meta charset='utf-8'><title>Measure</title>";
  html += "<style>"
          "body{font-family:sans-serif;margin:20px}"
          "pre{background:#f3f3f3;padding:8px;border-radius:6px;overflow:auto;max-height:60vh}"
          ".btn{display:inline-block;margin-right:6px;margin-bottom:6px;padding:6px 10px;border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000;background:#fff}"
          ".btn:hover{background:#eee}"
          "</style>";
  html += "</head><body>";

  html += "<h1>Pomiary / Dane</h1>";
  html += "<p><b>Bieżący plik:</b> " + curPath + " (" + String((unsigned long)curSize) + " B)</p>";

  html += "<p>";
  html += "<form method='POST' action='/measure/rotate_send' style='display:inline-block;margin-right:8px'>";
  html += "<button type='submit'>Rotuj i dodaj do kolejki FTP (teraz)</button>";
  html += "</form>";
  html += "<a href='/measure/view' target='_blank'>Pełny podgląd pliku</a>";
  html += "</p>";

  html += "<h2>Mini-podgląd ostatnich N linii</h2>";
  html += "<div style='margin:6px 0'>";
  html += "<a class='btn' href='/measure?n=100'>Pokaż ostatnie 100</a>";
  html += "<a class='btn' href='/measure?n=500'>Pokaż ostatnie 500</a>";
  html += "<a class='btn' href='/measure?n=1000'>Pokaż ostatnie 1000</a>";
  html += "</div>";

  html += "<form method='GET' action='/measure' style='margin:6px 0'>";
  html += "Pokaż ostatnie <input type='number' name='n' min='1' max='2000' value='" + String((unsigned)n) + "'> linii ";
  html += "<button type='submit'>Odśwież</button>";
  html += "</form>";

  html += "<pre>" + preview + "</pre>";

  html += "<h2>Interwał pomiaru</h2>";
  html += "<form method='POST' action='/measure/interval'>";
  html += "Interwał (sekundy): <input type='number' name='sec' min='1' value='" + String(interval) + "'>";
  html += " <button type='submit'>Zapisz</button>";
  html += "</form>";

  html += "<h2>Interwał próbkowania MCP</h2>";
  html += "<form method='POST' action='/measure/mcp_interval'>";
  html += "MCP (sekundy): <input type='number' name='sec' min='1' value='" + String(Measure::pomiarMCPInterval) + "'>";
  html += " <button type='submit'>Zapisz</button>";
  html += "</form>";

  html += "<h2>Kolejka FTP</h2>";
  html += "<pre>" + qjson + "</pre>";
  html += "<p><a href='/ftpq/stats' target='_blank'>/ftpq/stats</a></p>";

  html += "<p><a href='/'>← powrót</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

static void handleMeasureView() {
  if (!auth()) { return server.requestAuthentication(); }
  String path = DataFiles::pathCurrent();
  if (!LittleFS.exists(path)) { server.send(404, "text/plain", "no data"); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  server.streamFile(f, "text/plain"); f.close();
}

static void handleMeasureRotateSend() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!DataFiles::rotate3()) {
    server.send(500, "text/plain", "rotate failed"); return;
  }
  const String toSend = DataFiles::path1();
  const String dir    = Config::get().ftp_dir;
  if (FTPQ::enqueue(toSend.c_str(), dir.c_str())) {
    server.sendHeader("Location", "/measure");
    server.send(302, "text/plain", "enqueued");
  } else {
    server.send(500, "text/plain", "enqueue failed");
  }
}

static void handleMeasureSetInterval() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("sec")) { server.send(400, "text/plain", "missing 'sec'"); return; }
  uint32_t sec = (uint32_t) server.arg("sec").toInt();
  if (sec == 0) sec = 1;
  Measure::setPomiarInterval(sec);
  server.sendHeader("Location", "/measure");
  server.send(302, "text/plain", "ok");
}

static void handleMeasureSetMcpInterval() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("sec")) { server.send(400, "text/plain", "missing 'sec'"); return; }
  uint32_t sec = (uint32_t) server.arg("sec").toInt();
  Measure::setMCPInterval(sec);
  server.sendHeader("Location", "/measure");
  server.send(302, "text/plain", "ok");
}

/* ===================== NOWE ENDPOINTY: FTP QUEUE ===================== */

static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json", body);
}
static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

static void handleFtpqEnqueue() {
  if (!auth()) { return server.requestAuthentication(); }
  if (!server.hasArg("file")) {
    sendJson(400, "{\"ok\":false,\"msg\":\"missing 'file' query param\"}");
    return;
  }
  String file = server.arg("file");
  String dir  = server.hasArg("dir") ? server.arg("dir") : "";
  if (!LittleFS.exists(file)) {
    String msg = "{\"ok\":false,\"msg\":\"local file not found\",\"file\":\"" + file + "\"}";
    sendJson(404, msg); return;
  }
  bool ok = FTPQ::enqueue(file.c_str(), dir.c_str());
  if (ok) sendJson(200, "{\"ok\":true}");
  else    sendJson(500, "{\"ok\":false,\"msg\":\"enqueue failed\"}");
}
static void handleFtpqClear() {
  if (!auth()) { return server.requestAuthentication(); }
  bool ok = FTPQ::clear();
  sendJson(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"ok\":false,\"msg\":\"clear failed\"}");
}
static void handleFtpqStats() {
  String js = FTPQ::toJson();
  sendJson(200, js);
}

/* =================== ALARM panel =================== */

static void handleAlarmPage() {
  if (!auth()) { return server.requestAuthentication(); }

  
  Alarm::Status st; Alarm::getStatus(st);

  bool autoRefresh = (server.hasArg("autorefresh") && server.arg("autorefresh") == "1");
  const uint8_t gpio[5] = {13,15,21,22,23};

  String h;
  h.reserve(12000);
  h += "<!doctype html><html><head><meta charset='utf-8'><title>Alarm</title>";
  if (autoRefresh) h += "<meta http-equiv='refresh' content='3'>";
  h += "<style>"
       "body{font-family:sans-serif;margin:20px}"
       ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}"
       ".card{border:1px solid #ddd;border-radius:8px;padding:10px}"
       ".pill{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid #ccc;margin-left:6px}"
       ".ok{background:#e9fbe9;border-color:#b8e6b8}"
       ".on{background:#e9f3ff;border-color:#b8d0ff}"
       ".off{background:#f6f6f6;border-color:#ddd}"
       ".hi{background:#ffecec;border-color:#ffbaba}"
       ".lo{background:#fff0d6;border-color:#ffd699}"
       ".al{background:#ffecec;border-color:#ffbaba}"
       ".muted{color:#666}"
       "table{border-collapse:collapse;width:100%}"
       "th,td{border:1px solid #ddd;padding:4px;text-align:left;font-size:13px}"
       ".btn{display:inline-block;margin-right:6px;margin-bottom:6px;padding:6px 10px;border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000;background:#fff}"
       ".btn:hover{background:#eee}"
       "</style></head><body>";

  h += "<h1>Alarm – konfiguracja i status</h1>";

  h += "<div class='grid'>";
  h += "<div class='card'><h3>Wejścia binarne</h3><ul style='list-style:none;padding-left:0;margin:0'>";
  for (int i=0;i<5;++i) {
    char lbl[8];  snprintf(lbl, sizeof(lbl), "B%03d", i+1);
    h += "<li>";
    h += lbl;
    h += " <span class='muted'>(GPIO "; h += String(gpio[i]); h += ")</span>";
    if (i==4) h += " <span class='muted'>&middot; sieć AC</span>";
    h += st.binIn[i] ? "<span class='pill on'>1</span>" : "<span class='pill off'>0</span>";
    if (st.binActive[i]) h += "<span class='pill al'>ALARM</span>";
    h += "</li>";
  }
  h += "</ul></div>";

  h += "<div class='card'><h3>Wyjścia (przekaźniki)</h3><ul style='list-style:none;padding-left:0;margin:0'>";
  for (int i=0;i<4;++i) {
    char lbl[8]; snprintf(lbl, sizeof(lbl), "O%d", i+1);
    h += "<li>"; h += lbl;
    h += st.relay[i] ? "<span class='pill on'>ON</span>" : "<span class='pill off'>OFF</span>";
    h += "</li>";
  }
  h += "</ul></div>";

  h += "<div class='card'><h3>Wejścia analogowe</h3>";
  h += "<table><tr><th>Kanał</th><th>Wartość</th><th>Stan</th></tr>";
  for (int i=0;i<8;++i) {
    char ch[8]; snprintf(ch, sizeof(ch), "A%03d", i+1);
    h += "<tr><td>"; h += ch; h += "</td><td>";
    h += String(st.anaVal[i], 6);
    h += "</td><td>";
    if (st.anaActive[i]) {
      if (st.anaSide[i] > 0)      h += "<span class='pill hi'>HI</span>";
      else if (st.anaSide[i] < 0) h += "<span class='pill lo'>LO</span>";
      else                        h += "<span class='pill al'>ALARM</span>";
    } else {
      h += "<span class='pill ok'>OK</span>";
    }
    h += "</td></tr>";
  }
  h += "</table></div>";

  h += "<div class='card'><h3>Podsumowanie</h3>";
  h += "<p>WykrytoAlarm: ";
  h += Alarm::WykrytoAlarm ? "<span class='pill al'>TAK</span>" : "<span class='pill ok'>NIE</span>";
  h += "</p>";
  h += "<p>Auto-odświeżanie: ";
  if (autoRefresh) h += "<a class='btn' href='/alarm?autorefresh=0'>Wyłącz</a>";
  else             h += "<a class='btn' href='/alarm?autorefresh=1'>Włącz (co 3 s)</a>";
  h += "</p>";
  h += "<p>Bieżący plik alarmów: <code>";
  h += Alarm::alarmBasePath();
  h += "</code></p>";
  h += "<form method='POST' action='/alarm/sendnow'><button type='submit'>Wyślij log alarmów teraz (rotuj + FTP)</button></form>";
  h += "</div>";

  h += "</div>";

  h += "<h2>Konfiguracja progów i zachowań</h2>";
  h += "<form method='POST' action='/alarm/save'>";

  auto conf = AlarmCfg::get();
  h += "<h3>Analog (A001..A008)</h3><table><tr><th>Kanał</th><th>Hi</th><th>Lo</th><th>Hyst+</th><th>Hyst-</th><th>Czas [s]</th><th>Ile pom.</th></tr>";
  for (int i=0;i<8;++i) {
    char ch[8]; snprintf(ch, sizeof(ch), "A%03d", i+1);
    h += String("<tr><td>") + ch + "</td>"
       + "<td><input name='a"+String(i)+"_hi' value='"+String(conf.analog[i].hi,6)+"'></td>"
       + "<td><input name='a"+String(i)+"_lo' value='"+String(conf.analog[i].lo,6)+"'></td>"
       + "<td><input name='a"+String(i)+"_hp' value='"+String(conf.analog[i].hystP,6)+"'></td>"
       + "<td><input name='a"+String(i)+"_hn' value='"+String(conf.analog[i].hystN,6)+"'></td>"
       + "<td><input name='a"+String(i)+"_t'  value='"+String(conf.analog[i].timeSec)+"' size='5'></td>"
       + "<td><input name='a"+String(i)+"_n'  value='"+String(conf.analog[i].countReq)+"' size='5'></td></tr>";
  }
  h += "</table>";

  const uint8_t gpioArr[5] = {13,15,21,22,23};
  h += "<h3>Binarne (B001..B005)</h3><table><tr><th>Wejście (GPIO)</th><th>Tryb</th><th>Akcja</th><th>Czas [s]</th><th>Ile pom.</th></tr>";
  for (int i=0;i<5;++i) {
    char bl[8]; snprintf(bl, sizeof(bl), "B%03d", i+1);
    h += "<tr><td>"; h += bl; h += " (GPIO "; h += String(gpioArr[i]); h += ")</td>";
    h += "<td><select name='b"+String(i)+"_m'>";
    for (int m=0;m<=2;++m) {
      h += "<option value='"+String(m)+"'"; if ((int)conf.binary[i].mode==m) h+=" selected"; h+=">";
      if (m==0) h+="alarm na 0"; if (m==1) h+="alarm na 1"; if (m==2) h+="wyłączone"; h += "</option>";
    }
    h += "</select></td>";
    h += "<td><input name='b"+String(i)+"_a' value='"+String((int)conf.binary[i].action)+"' size='4' title='XY: X=wyjście 1..4 (0=brak), Y=0/1'></td>";
    h += "<td><input name='b"+String(i)+"_t' value='"+String(conf.binary[i].timeSec)+"' size='5'></td>";
    h += "<td><input name='b"+String(i)+"_n' value='"+String(conf.binary[i].countReq)+"' size='5'></td>";
    h += "</tr>";
  }
  h += "</table>";

  h += "<p><button type='submit'>Zapisz</button></p>";
  h += "</form>";

  h += "<p><a href='/'>← powrót</a></p>";
  h += "</body></html>";

  server.send(200, "text/html", h);
}

static void handleAlarmSave() {
  if (!auth()) { return server.requestAuthentication(); }
  auto c = AlarmCfg::get();

  for (int i=0;i<8;++i) {
    String pfx = "a" + String(i) + "_";
    if (server.hasArg(pfx+"hi")) c.analog[i].hi = server.arg(pfx+"hi").toFloat();
    if (server.hasArg(pfx+"lo")) c.analog[i].lo = server.arg(pfx+"lo").toFloat();
    if (server.hasArg(pfx+"hp")) c.analog[i].hystP = server.arg(pfx+"hp").toFloat();
    if (server.hasArg(pfx+"hn")) c.analog[i].hystN = server.arg(pfx+"hn").toFloat();
    if (server.hasArg(pfx+"t"))  c.analog[i].timeSec = server.arg(pfx+"t").toInt();
    if (server.hasArg(pfx+"n"))  c.analog[i].countReq = server.arg(pfx+"n").toInt();
  }
  for (int i=0;i<5;++i) {
    String pfx = "b" + String(i) + "_";
    if (server.hasArg(pfx+"m")) c.binary[i].mode = server.arg(pfx+"m").toInt();
    if (server.hasArg(pfx+"a")) c.binary[i].action = server.arg(pfx+"a").toInt();
    if (server.hasArg(pfx+"t")) c.binary[i].timeSec = server.arg(pfx+"t").toInt();
    if (server.hasArg(pfx+"n")) c.binary[i].countReq = server.arg(pfx+"n").toInt();
  }

  AlarmCfg::set(c);
  server.sendHeader("Location", "/alarm");
  server.send(302);
}

static void handleAlarmSendNow() {
  if (!auth()) { return server.requestAuthentication(); }
  bool ok = Alarm::rotateAndEnqueueNow();
  server.send(200, "text/plain", ok ? "OK (rotated+enqueued)" : "NO DATA (or rename fail)");
}

/* =========== START / LOOP =========== */

void WebUI::begin() {
  // wrapper i czysty index
  server.on("/", HTTP_GET, handleRootWrapper);
  server.on("/ui_index", HTTP_GET, handleUIIndex);

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.on("/ftp_test", HTTP_GET, handleFTPTest);
  server.on("/ftp_enqueue_test", HTTP_GET, handleFTPEnqueueTest);
  server.on("/logs", HTTP_GET, handleLogsIndex);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/ftp_queue_clear", HTTP_GET, handleFTPQueueClear);
  server.on("/fs", HTTP_GET, handleFS);
  server.on("/log_delete", HTTP_GET, handleLogDelete);

  // FTP QUEUE API
  server.on("/ftpq/enqueue", HTTP_OPTIONS, handleOptions);
  server.on("/ftpq/clear",   HTTP_OPTIONS, handleOptions);
  server.on("/ftpq/stats",   HTTP_OPTIONS, handleOptions);
  server.on("/ftpq/enqueue", HTTP_POST,    handleFtpqEnqueue);
  server.on("/ftpq/clear",   HTTP_POST,    handleFtpqClear);
  server.on("/ftpq/stats",   HTTP_GET,     handleFtpqStats);

  // Measure
  server.on("/measure",              HTTP_GET,  handleMeasurePage);
  server.on("/measure/view",         HTTP_GET,  handleMeasureView);
  server.on("/measure/rotate_send",  HTTP_POST, handleMeasureRotateSend);
  server.on("/measure/interval",     HTTP_POST, handleMeasureSetInterval);
  server.on("/measure/mcp_interval", HTTP_POST, handleMeasureSetMcpInterval);

  // Mail
  server.on("/mail",       HTTP_GET,  handleMailPage);
  server.on("/mail/save",  HTTP_POST, handleMailSave);
  server.on("/mail/test",  HTTP_POST, handleMailTest);
  server.on("/mail/check", HTTP_GET,  handleMailCheck);

  // Alarm
  server.on("/alarm",        HTTP_GET,  handleAlarmPage);
  server.on("/alarm/save",   HTTP_POST, handleAlarmSave);
  server.on("/alarm/sendnow",HTTP_POST, handleAlarmSendNow);

  // Menu + zapis interwału FTP
  server.on("/menu",              HTTP_GET,  handleMenu);
  server.on("/set_send_interval", HTTP_POST, handleSetSendIntervalPost);
  server.on("/index_delete", HTTP_POST, handleIndexDelete);
  server.on("/cfgsync/interval", HTTP_POST, handleCfgSyncInterval);
  server.on("/cfgsync/run",      HTTP_POST, handleCfgSyncRun);

    // globalna strona 404 (ostatnia rejestracja!) Uwaga: onNotFound() powinno być dodane po wszystkich innych server.on(...),
  server.onNotFound(handleNotFound404);

  server.begin();

  if (!LittleFS.exists("/index.html")) {
    File f = LittleFS.open("/index.html", "w");
    if (f) { f.print("<h1>Upload index.html to LittleFS</h1>"); f.close(); }
  }
  LOGI("WebUI started on http://%s", Net::wifiIpStr());
}

void WebUI::loop() { server.handleClient(); }
