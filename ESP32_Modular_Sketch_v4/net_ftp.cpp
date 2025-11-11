#include "net_ftp.h"
#include "config.h"
#include "gsm_wifi.h"

namespace {
  enum class Mode { WIFI, GSM };
  Mode mode = Mode::WIFI;
}

namespace NetFTP {
void begin() {
  mode = (Config::get().net_mode == "gsm") ? Mode::GSM : Mode::WIFI;
  if (mode == Mode::WIFI) {
    extern void wifiFtpBegin(); wifiFtpBegin(); // albo bezpośrednio NetFTP::... z pliku Wi-Fi
  } else {
    extern void simFtpBegin();  simFtpBegin();
  }
}
void loop() {
  if (mode == Mode::GSM) { extern void simFtpLoop(); simFtpLoop(); }
  // Wi-Fi backend zwykle nie potrzebuje loopa
}
Result putFile(const String& l, const String& r) {
  if (mode == Mode::GSM) { extern Result simPut(const String&,const String&); return simPut(l,r); }
  else                   { extern Result wifiPut(const String&,const String&); return wifiPut(l,r); }
}
// ... analogicznie getFile/rename/remove/exists/cwd/busy
}
