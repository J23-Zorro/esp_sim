// net_ftp_wifi.cpp
#include "net_ftp.h"
#include "config.h"
#include "ftp_upload.h"  // jeżeli masz już gotowe TCP-FTP (pasv/binary) – użyj go
#include "log.h"
namespace NetFTP {

void begin() { /* nic – używasz istniejącego klienta */ }
void loop()  { /* nic */ }

Result putFile(const String& localPath, const String& remoteDir) {
  bool ok = FTP::uploadFile(localPath.c_str(), remoteDir.c_str());
  return { ok, ok ? "OK" : "upload fail" };
}
// Analogicznie getFile/rename/remove/cwd/exists możesz zmapować na swoje funkcje…
Result getFile(const String&, const String&) { return {false,"not-impl"}; }
// i tak dalej – jeśli po Wi-Fi nie potrzebujesz pobierania, możesz zwrócić not-impl.
Result rename(const String& a, const String& b){ bool ok = FTP::rename(a,b); return {ok, ok?"OK":"rename fail"}; }
// ...
bool busy() { return false; }
}
