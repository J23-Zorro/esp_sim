#include "cfg_sync.h"
#include "config.h"
#include "alarm_config.h"
#include "email_config.h"
#include "ftp_upload.h"
#include "gsm_wifi.h"
#include "log.h"

#include <LittleFS.h>
#include <WiFiClient.h>
#include <vector>
#include "measurement.h"

// ------------------ USTAWIENIA I STAN ------------------

namespace {
  uint32_t g_interval = 900;          // domyślnie co 15 min
  unsigned long g_lastMs = 0;

  // Nazwy na serwerze (w katalogu ftp_dir):
  const char* REMOTE_IN  = "configZ.txt"; // wejściowy, częściowy
  // Uwaga: po pobraniu REMOTE_IN -> zmieniamy jego nazwę na configZ_<EPOCH>.txt

  String nowEpochStr() {
    return String((unsigned long)time(nullptr));
  }

  // Prosty FTP klient „ad hoc” (minimum do: CWD, SIZE/LIST, RETR, RNFR/RNTO)
  // Używamy Net::client() z Twojej warstwy sieci
  class FtpClient {
  public:
    bool connected = false;
    WiFiClient ctrl;
    WiFiClient data;

    bool connectCtrl(const String& host, uint16_t port){
      if (connected) return true;
      if (!ctrl.connect(host.c_str(), port)) return false;
      if (!expect2xx()) { ctrl.stop(); return false; }
      connected = true;
      return true;
    }

    void quit(){
      if (!connected) return;
      send("QUIT");
      ctrl.stop();
      connected = false;
    }

    bool login(const String& user, const String& pass){
      if (!send("USER " + user)) return false;
      if (!expect3xx() && !expect2xxPeek()) return false;
      if (!send("PASS " + pass)) return false;
      if (!expect2xx()) return false;
      return true;
    }

    bool cwd(const String& dir){
      if (dir.length()==0) return true; // bieżący
      if (!send("CWD " + dir)) return false;
      return expect2xx();
    }

    // Sprawdza czy plik istnieje (SIZE)
    bool exists(const String& name){
      if (!send("SIZE " + name)) return false;
      // 213 size - istnieje; 550 - brak
      int code = readCode();
      return code == 213;
    }

    // Tryb pasywny -> ustawia data socket
    bool pasv(){
      if (!send("PASV")) return false;
      String line = readLine(5000);
      if (!line.startsWith("227")) return false;
      int l = line.indexOf('('), r = line.indexOf(')');
      if (l<0 || r<0 || r<=l+1) return false;
      String p = line.substring(l+1, r);
      int a1,a2,a3,a4,p1,p2;
      if (sscanf(p.c_str(), "%d,%d,%d,%d,%d,%d",&a1,&a2,&a3,&a4,&p1,&p2) != 6) return false;
      uint16_t port = (p1<<8) | p2;
      char host[32]; snprintf(host,sizeof(host), "%d.%d.%d.%d", a1,a2,a3,a4);
      if (!data.connect(host, port)) return false;
      return true;
    }

    // RETR do lokalnego pliku
    bool retrToFile(const String& remote, const String& localPath){
      if (!pasv()) return false;
      if (!send("RETR " + remote)) { data.stop(); return false; }
      int code = readCode();
      if (code != 150 && code != 125) { data.stop(); return false; }

      File f = LittleFS.open(localPath, "w");
      if (!f) { data.stop(); (void)readCode(); return false; }

      uint8_t buf[1024];
      unsigned long t0 = millis();
      while (data.connected() || data.available()) {
        int n = data.read(buf, sizeof(buf));
        if (n>0) { f.write(buf, n); t0 = millis(); }
        else { delay(2); if (millis()-t0>3000) break; }
      }
      f.close();
      data.stop();
      return expect2xx();
    }

    bool rename(const String& from, const String& to){
      if (!send("RNFR " + from)) return false;
      if (!expect3xx()) return false;
      if (!send("RNTO " + to)) return false;
      return expect2xx();
    }

  private:
    bool send(const String& cmd){
      String c = cmd + "\r\n";
      // LOGD("FTP >> %s", cmd.c_str());
      return ctrl.print(c) == (int)c.length();
    }
    int readCode(){
      String line = readLine(5000);
      if (line.length() < 3) return 0;
      int code = line.substring(0,3).toInt();
      // LOGD("FTP << %s", line.c_str());
      // Jeśli są linie wielowierszowe (format 123- ... 123 ...), czytamy do końca:
      if (line.length()>=4 && line[3]=='-') {
        while (true) {
          String l2 = readLine(5000);
          // LOGD("FTP << %s", l2.c_str());
          if (l2.length()>=4 && l2.startsWith(String(code)) && l2[3]==' ') break;
        }
      }
      return code;
    }
    bool expect2xx(){
      int c = readCode();
      return c>=200 && c<300;
    }
    bool expect2xxPeek(){
      ctrl.setTimeout(5000);
      String line = ctrl.readStringUntil('\n'); // pojedyncza
      if (line.length()<3) return false;
      int code = line.substring(0,3).toInt();
      return code>=200 && code<300;
    }
    bool expect3xx(){
      int c = readCode();
      return c>=300 && c<400;
    }
    String readLine(uint32_t to=3000){
      ctrl.setTimeout(to);
      String s = ctrl.readStringUntil('\n');
      s.trim();
      return s;
    }
  };

  // ------------------ GENERATOR FULL CONFIG (key=value) ------------------

  String fullConfigText() {
    String out;
    out.reserve(8000);
    out += "# ====== CONFIG (Panel konfiguracja) ======\n";
    const auto& c = Config::get();
    out += "cfg.net_mode="      + c.net_mode      + "\n";
    out += "cfg.wifi_ssid="     + c.wifi_ssid     + "\n";
    out += "cfg.wifi_pass="     + c.wifi_pass     + "\n";
    out += "cfg.apn="           + c.apn           + "\n";
    out += "cfg.pin="           + c.pin           + "\n";
    out += "cfg.uart_tx="       + String(c.uart_tx)     + "\n";
    out += "cfg.uart_rx="       + String(c.uart_rx)     + "\n";
    out += "cfg.uart_baud="     + String((unsigned long)c.uart_baud) + "\n";
    out += "cfg.mqtt_host="     + c.mqtt_host     + "\n";
    out += "cfg.mqtt_port="     + String(c.mqtt_port)   + "\n";
    out += "cfg.mqtt_user="     + c.mqtt_user     + "\n";
    out += "cfg.mqtt_pass="     + c.mqtt_pass     + "\n";
    out += "cfg.mqtt_topic_pub="+ c.mqtt_topic_pub+ "\n";
    out += "cfg.ftp_host="      + c.ftp_host      + "\n";
    out += "cfg.ftp_port="      + String(c.ftp_port)    + "\n";
    out += "cfg.ftp_user="      + c.ftp_user      + "\n";
    out += "cfg.ftp_pass="      + c.ftp_pass      + "\n";
    out += "cfg.ftp_dir="       + c.ftp_dir       + "\n";
    out += "cfg.http_user="     + c.http_user     + "\n";
    out += "cfg.http_pass="     + c.http_pass     + "\n";
    out += "cfg.sendFTPInterval_sec=" + String(c.sendFTPInterval_sec) + "\n";

    out += "\n# ====== ALARM ======\n";
    auto ac = AlarmCfg::get();
    for (int i=0;i<8;++i){
      out += "alarm.ana[" + String(i) + "].hi="      + String(ac.analog[i].hi,6) + "\n";
      out += "alarm.ana[" + String(i) + "].lo="      + String(ac.analog[i].lo,6) + "\n";
      out += "alarm.ana[" + String(i) + "].hystP="   + String(ac.analog[i].hystP,6) + "\n";
      out += "alarm.ana[" + String(i) + "].hystN="   + String(ac.analog[i].hystN,6) + "\n";
      out += "alarm.ana[" + String(i) + "].timeSec=" + String(ac.analog[i].timeSec) + "\n";
      out += "alarm.ana[" + String(i) + "].countReq="+ String(ac.analog[i].countReq) + "\n";
    }
    for (int i=0;i<5;++i){
      out += "alarm.bin[" + String(i) + "].mode="     + String(ac.binary[i].mode) + "\n";
      out += "alarm.bin[" + String(i) + "].action="   + String(ac.binary[i].action) + "\n";
      out += "alarm.bin[" + String(i) + "].timeSec="  + String(ac.binary[i].timeSec) + "\n";
      out += "alarm.bin[" + String(i) + "].countReq=" + String(ac.binary[i].countReq) + "\n";
    }

    out += "\n# ====== EMAIL ======\n";
    EmailCfg::Settings es; EmailCfg::load(es);
    out += "mail.enabled="  + String(es.enabled?1:0) + "\n";
    out += "mail.smtp_host="+ es.smtp_host + "\n";
    out += "mail.smtp_port="+ String(es.smtp_port) + "\n";
    out += "mail.pop3_host="+ es.pop3_host + "\n";
    out += "mail.pop3_port="+ String(es.pop3_port) + "\n";
    out += "mail.user="     + es.user + "\n";
    out += "mail.pass="     + es.pass + "\n";
    out += "mail.sender="   + es.sender + "\n";
    out += "mail.group1="   + es.group1_csv + "\n";
    out += "mail.group2="   + es.group2_csv + "\n";
    out += "mail.group3="   + es.group3_csv + "\n";
    out += "mail.wait_g1="  + String(es.wait_g1) + "\n";
    out += "mail.wait_g2="  + String(es.wait_g2) + "\n";
    out += "mail.wait_g3="  + String(es.wait_g3) + "\n";
    out += "mail.body_alarm="   + es.body_alarm + "\n";
    out += "mail.body_recover=" + es.body_recover + "\n";

    return out;
  }

  // ------------------ PARSER DELTY (key=value) ------------------

  static String trim(const String& s){
    String t=s; t.trim(); return t;
  }
  static bool isCommentOrEmpty(const String& s){
    String t=trim(s);
    return t.length()==0 || t.startsWith("#") || t.startsWith("//") || t.startsWith(";");
  }

  // Podstawowe helpery do liczbowych
  static bool toInt(const String& s, int& out){ char* e=nullptr; long v=strtol(s.c_str(), &e, 10); if(e && *e==0){ out=(int)v; return true;} return false; }
  static bool toU32(const String& s, uint32_t& out){ char* e=nullptr; unsigned long v=strtoul(s.c_str(), &e, 10); if(e && *e==0){ out=(uint32_t)v; return true;} return false; }
  static bool toF64(const String& s, double& out){ char* e=nullptr; double v=strtod(s.c_str(), &e); if(e && *e==0){ out=v; return true;} return false; }

  // Zastosuj jedną parę key=value
  static void applyKV(const String& k, const String& v,
                      ConfigData& cfg,
                      AlarmCfg::Config& ac,
                      EmailCfg::Settings& es)
  {
    // ---- cfg.*
    if (k=="cfg.net_mode") cfg.net_mode = v;
    else if (k=="cfg.wifi_ssid") cfg.wifi_ssid = v;
    else if (k=="cfg.wifi_pass") cfg.wifi_pass = v;
    else if (k=="cfg.apn") cfg.apn = v;
    else if (k=="cfg.pin") cfg.pin = v;
    else if (k=="cfg.uart_tx"){ int t; if(toInt(v,t)) cfg.uart_tx=t; }
    else if (k=="cfg.uart_rx"){ int t; if(toInt(v,t)) cfg.uart_rx=t; }
    else if (k=="cfg.uart_baud"){ uint32_t t; if(toU32(v,t)) cfg.uart_baud=t; }
    else if (k=="cfg.mqtt_host") cfg.mqtt_host = v;
    else if (k=="cfg.mqtt_port"){ int t; if(toInt(v,t)) cfg.mqtt_port=t; }
    else if (k=="cfg.mqtt_user") cfg.mqtt_user = v;
    else if (k=="cfg.mqtt_pass") cfg.mqtt_pass = v;
    else if (k=="cfg.mqtt_topic_pub") cfg.mqtt_topic_pub = v;
    else if (k=="cfg.ftp_host") cfg.ftp_host = v;
    else if (k=="cfg.ftp_port"){ int t; if(toInt(v,t)) cfg.ftp_port=t; }
    else if (k=="cfg.ftp_user") cfg.ftp_user = v;
    else if (k=="cfg.ftp_pass") cfg.ftp_pass = v;
    else if (k=="cfg.ftp_dir")  cfg.ftp_dir = v;
    else if (k=="cfg.http_user") cfg.http_user = v;
    else if (k=="cfg.http_pass") cfg.http_pass = v;
    else if (k=="cfg.sendFTPInterval_sec"){ uint32_t t; if(toU32(v,t)) cfg.sendFTPInterval_sec=t; }
    else if (k=="cfg.cfgSyncInterval_sec"){ uint32_t t; if(toU32(v,t)) cfg.cfgSyncInterval_sec=t; }

    // ---- alarm.ana[i].*
    else if (k.startsWith("alarm.ana[")) {
      int idx = k.substring(10, k.indexOf(']')).toInt();
      if (idx>=0 && idx<8) {
        if (k.endsWith("].hi"))      { double d; if(toF64(v,d)) ac.analog[idx].hi=d; }
        else if (k.endsWith("].lo")) { double d; if(toF64(v,d)) ac.analog[idx].lo=d; }
        else if (k.endsWith("].hystP")){ double d; if(toF64(v,d)) ac.analog[idx].hystP=d; }
        else if (k.endsWith("].hystN")){ double d; if(toF64(v,d)) ac.analog[idx].hystN=d; }
        else if (k.endsWith("].timeSec")){ int t; if(toInt(v,t)) ac.analog[idx].timeSec=t; }
        else if (k.endsWith("].countReq")){ int t; if(toInt(v,t)) ac.analog[idx].countReq=t; }
      }
    }
    // ---- alarm.bin[i].*
    else if (k.startsWith("alarm.bin[")) {
      int idx = k.substring(10, k.indexOf(']')).toInt();
      if (idx>=0 && idx<5) {
        if (k.endsWith("].mode"))     { int t; if(toInt(v,t)) ac.binary[idx].mode=t; }
        else if (k.endsWith("].action")){ int t; if(toInt(v,t)) ac.binary[idx].action=t; }
        else if (k.endsWith("].timeSec")){ int t; if(toInt(v,t)) ac.binary[idx].timeSec=t; }
        else if (k.endsWith("].countReq")){ int t; if(toInt(v,t)) ac.binary[idx].countReq=t; }
      }
    }
    // ---- mail.*
    else if (k=="mail.enabled"){ int t; if(toInt(v,t)) es.enabled = (t!=0); }
    else if (k=="mail.smtp_host") es.smtp_host = v;
    else if (k=="mail.smtp_port"){ int t; if(toInt(v,t)) es.smtp_port=t; }
    else if (k=="mail.pop3_host") es.pop3_host = v;
    else if (k=="mail.pop3_port"){ int t; if(toInt(v,t)) es.pop3_port=t; }
    else if (k=="mail.user") es.user = v;
    else if (k=="mail.pass") es.pass = v;
    else if (k=="mail.sender") es.sender = v;
    else if (k=="mail.group1") es.group1_csv = v;
    else if (k=="mail.group2") es.group2_csv = v;
    else if (k=="mail.group3") es.group3_csv = v;
    else if (k=="mail.wait_g1"){ int t; if(toInt(v,t)) es.wait_g1=t; }
    else if (k=="mail.wait_g2"){ int t; if(toInt(v,t)) es.wait_g2=t; }
    else if (k=="mail.wait_g3"){ int t; if(toInt(v,t)) es.wait_g3=t; }
    else if (k=="mail.body_alarm")   es.body_alarm = v;
    else if (k=="mail.body_recover") es.body_recover = v;
  }

  // Wczytaj lok. plik i zastosuj delty
  bool applyDeltaFile(const String& localPath){
    File f = LittleFS.open(localPath, "r");
    if (!f) return false;

    auto cfg = Config::get();
    auto ac  = AlarmCfg::get();
    EmailCfg::Settings es; EmailCfg::load(es);

    while (f.available()){
      String line = f.readStringUntil('\n'); line.trim();
      if (isCommentOrEmpty(line)) continue;
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = trim(line.substring(0, eq));
      String val = trim(line.substring(eq+1));
      applyKV(key, val, cfg, ac, es);
    }
    f.close();

    // Zapisz wszystkie sekcje
    Config::save(cfg);
    AlarmCfg::set(ac);
    EmailCfg::save(es);
    //Dzięki temu interwały „żyją” od razu po zdalnej zmianie:
    Measure::setSendFTPInterval(cfg.sendFTPInterval_sec);
    CfgSync::setInterval(cfg.cfgSyncInterval_sec);

    // Jeśli zmienił się interwał wysyłki FTP – poinformuj Measure (jeśli masz API)
    // -> używasz Measure::setSendFTPInterval(sec) w innych miejscach

    return true;
  }

} // anon

// ------------------ API ------------------

void CfgSync::setInterval(uint32_t sec){ if (sec<60) sec=60; if (sec>24UL*3600UL) sec=24UL*3600UL; g_interval=sec; }
uint32_t CfgSync::getInterval(){ return g_interval; }

void CfgSync::begin(){
  // nic specjalnego
}

static bool processOnce(){
  if (!Net::connected()) return false;
  auto& cfg = Config::get();
  if (cfg.ftp_host.length()==0) return false;

  FtpClient ftp;
  if (!ftp.connectCtrl(cfg.ftp_host, cfg.ftp_port)) { LOGW("CfgSync: ctrl connect fail"); return false; }
  if (!ftp.login(cfg.ftp_user, cfg.ftp_pass))       { LOGW("CfgSync: login fail"); ftp.quit(); return false; }
  if (!ftp.cwd(cfg.ftp_dir))                        { LOGW("CfgSync: CWD fail"); ftp.quit(); return false; }

  // 1) Jeżeli istnieje configZ.txt -> najpierw wyślij pełny config jako configU_<epoch>.txt
  if (ftp.exists(REMOTE_IN)) {
    String epoch = nowEpochStr();

    // a) utwórz lokalny pełny config
    String localU = "/configU_" + epoch + ".txt";
    {
      File f = LittleFS.open(localU, "w");
      if (!f) { LOGE("CfgSync: create %s failed", localU.c_str()); ftp.quit(); return false; }
      String txt = fullConfigText();
      f.print(txt);
      f.close();
    }
    // b) wyślij (zachowaj nazwę bazową)
    if (!FTP::uploadFile(localU.c_str(), cfg.ftp_dir.c_str())) {
      LOGE("CfgSync: upload %s failed", localU.c_str());
      LittleFS.remove(localU);
      ftp.quit();
      return false;
    }
    LittleFS.remove(localU); // nie musimy trzymać lokalnej kopii

    // 2) pobierz configZ.txt do /configZ.txt
    String localZ = "/configZ.txt";
    if (!ftp.retrToFile(REMOTE_IN, localZ)) {
      LOGE("CfgSync: RETR %s failed", REMOTE_IN);
      ftp.quit();
      return false;
    }

    // 3) zmień nazwę zdalnego pliku na configZ_<epoch>.txt
    String newRemote = String("configZ_") + epoch + ".txt";
    if (!ftp.rename(REMOTE_IN, newRemote)) {
      LOGW("CfgSync: RNFR/RNTO failed (will still apply local delta)");
    }

    ftp.quit();

    // 4) zastosuj delty
    bool ok = applyDeltaFile(localZ);
    LittleFS.remove(localZ);
    LOGI("CfgSync: delta %s", ok?"APPLIED":"FAILED");
    return ok;
  } else {
    // brak pliku -> koniec
    ftp.quit();
    return false;
  }
}

bool CfgSync::runOnceNow(){
  return processOnce();
}

void CfgSync::loopTick(){
  unsigned long now = millis();
  if (now - g_lastMs < g_interval * 1000UL) return;
  g_lastMs = now;
  (void)processOnce();
}
