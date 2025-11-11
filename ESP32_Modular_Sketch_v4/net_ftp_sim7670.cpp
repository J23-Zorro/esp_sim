// net_ftp_sim7670.cpp
#include "net_ftp.h"
#include "config.h"
#include "log.h"
#include <TinyGsmClient.h>
#include <LittleFS.h>

// Załóżmy, że masz globalnie (w gsm_wifi.cpp) modem i serial:
extern TinyGsm        modem;
extern HardwareSerial GSM_SERIAL;

namespace NetFTP {

enum class Op { None, Put, Get, Rename, Remove, Cwd, Exists };

struct Job {
  Op op = Op::None;
  String a, b;         // ścieżki/parametry
  File   f;            // lokalny plik (PUT/GET)
  size_t sent = 0;     // ile już wysłano
  size_t size = 0;     // rozmiar pliku
  String msg;
  bool ok = false;
};

static Job j;
static uint32_t tmo_ms = 0;

static bool waitFor(const String& token, uint32_t ms=12000) {
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < ms) {
    while (GSM_SERIAL.available()) {
      char c = GSM_SERIAL.read();
      if (c == '\n') {
        // LOGD line if potrzebujesz
        if (line.indexOf(token) >= 0) return true;
        line = "";
      } else if (c != '\r') line += c;
    }
    delay(1);
  }
  return false;
}

static void at(const String& cmd) {
  GSM_SERIAL.print(cmd); GSM_SERIAL.print("\r\n");
}

// --- przygotowanie sesji FTP (host, user, pass, path)
static bool ftpSetup(const String& fileName, const String& path) {
  auto& c = Config::get();
  at("AT+FTPCID=1");
  if (!waitFor("OK")) return false;
  at(String("AT+FTPSERV=\"") + c.ftp_host + "\"");
  if (!waitFor("OK")) return false;
  at(String("AT+FTPPORT=") + String(c.ftp_port));
  if (!waitFor("OK")) return false;
  at(String("AT+FTPUN=\"") + c.ftp_user + "\"");
  if (!waitFor("OK")) return false;
  at(String("AT+FTPPW=\"") + c.ftp_pass + "\"");
  if (!waitFor("OK")) return false;
  at("AT+FTPTYPE=\"I\""); // binary
  if (!waitFor("OK")) return false;
  at(String("AT+FTPPUTNAME=\"") + fileName + "\"");
  if (!waitFor("OK")) return false;
  String p = path; if (!p.endsWith("/")) p += "/";
  at(String("AT+FTPPUTPATH=\"") + p + "\"");
  if (!waitFor("OK")) return false;
  return true;
}

// --- upload nieblokujący (porcjami)
static void tickPut() {
  if (!j.f) { j.ok=false; j.msg="file not open"; j.op=Op::None; return; }

  if (j.sent == 0) {
    // start PUT
    at("AT+FTPPUT=1");
    if (!waitFor("+FTPPUT:1")) { j.ok=false; j.msg="FTPPUT=1 fail"; j.op=Op::None; return; }
  }

  static uint8_t buf[1024];
  size_t chunk = j.f.read(buf, sizeof(buf));
  if (chunk > 0) {
    at(String("AT+FTPPUT=2,") + String(chunk));
    // modem teraz czeka na surowe <chunk> bajtów:
    GSM_SERIAL.write(buf, chunk);
    if (!waitFor("OK")) { j.ok=false; j.msg="FTPPUT data fail"; j.op=Op::None; return; }
    j.sent += chunk;
    return; // kolejna porcja w następnym ticku
  }

  // koniec pliku → zakończ
  at("AT+FTPPUT=2,0");
  if (!waitFor("OK")) { j.ok=false; j.msg="FTPPUT finish fail"; j.op=Op::None; return; }

  j.ok = true; j.msg="OK"; j.op=Op::None;
  j.f.close();
}

// (opcjonalnie) GET, RENAME, REMOVE, EXISTS… – podobnie stanowo,
// tu skracam dla czytelności. W razie potrzeby dopiszę.

void begin() {
  // tu nic – zakładamy, że PDP `AT+NETOPEN` zrobione wcześniej w warstwie GSM
}

void loop() {
  switch (j.op) {
    case Op::Put: tickPut(); break;
    default: break;
  }
}

bool busy() { return j.op != Op::None; }

Result putFile(const String& localPath, const String& remoteDir) {
  if (busy()) return {false,"busy"};
  String name;
  int slash = localPath.lastIndexOf('/'); 
  name = (slash>=0) ? localPath.substring(slash+1) : localPath;

  if (!ftpSetup(name, remoteDir)) return {false,"ftp setup fail"};

  j = Job{};
  j.op   = Op::Put;
  j.f    = LittleFS.open(localPath, "r");
  j.size = j.f ? j.f.size() : 0;
  if (!j.f) { j.op = Op::None; return {false,"open local fail"}; }

  // pierwsza porcja poleci w loop()
  return {true,"started", j.size};
}

// getFile/rename/remove/exists/cwd dołożysz analogicznie
Result getFile(const String&, const String&) { return {false,"not-impl"}; }
Result rename(const String&, const String&)  { return {false,"not-impl"}; }
Result remove(const String&)                 { return {false,"not-impl"}; }
Result cwd(const String&)                    { return {true,"noop"}; }
Result exists(const String&)                 { return {false,"not-impl"}; }

} // namespace
