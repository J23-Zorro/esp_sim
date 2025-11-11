// net_ftp.h
#pragma once
#include <Arduino.h>

namespace NetFTP {

struct Result {
  bool ok;
  String msg;
  unsigned long bytes = 0;
};

void begin();                // wybierze backend wg trybu sieci (wifi/gsm) i configu
void loop();                 // nieblokująca obsługa backendu (modem AT: state machine)

Result putFile(const String& localPath, const String& remoteDir);  // upload
Result getFile(const String& remotePath, const String& localPath); // download
Result rename(const String& remoteFrom, const String& remoteTo);   // RNFR/RNTO
Result remove(const String& remotePath);
Result cwd(const String& remoteDir);                               // CWD / utwórz jeśli brak
Result exists(const String& remotePath);                           // SIZE/MLST/NLST
bool   busy();                                                     // czy backend jest zajęty (job w toku)
}
