#pragma once
#include <Arduino.h>

namespace Measure {
  // interwał (sekundy) i ostatnia krawędź od 12:00 (czas unix)
  extern uint32_t pomiarADCInterval;   // sekundy (kotwica 12:00)
  extern uint32_t pomiarMCPInterval;   // sekundy (niezależny, patrz warunek)
  extern time_t   lastPomiarUploadTime;

  // API
  void startMCP3424();   // deleguje do drivera
  void pomiarMCP3424();  // deleguje do drivera
  void myTestPomiar();   // liczy A*x+B i zapisuje 8 rekordów A001..A008
  void WyslijDaneNaFTP();// jeśli rozmiar >= 100kB lub „tik” interwału – rotacja i enqueue

// measurement.h

  extern uint32_t sendFTPInterval;     // [s] co ile wysyłać D_<MAC>.txt (czasowo)
  void setSendFTPInterval(uint32_t sec);
  uint32_t getSendFTPInterval();

  // Wywołuj często
  void loopTick();
  // NOWE: setter interwału (przelicza kotwicę od 12:00)
  void setPomiarInterval(uint32_t sec);
  void setMCPInterval(uint32_t sec);
  // Wspólne narzędzia wymagane przez opis:
  String pomTimeStamp();                           // RR:MM:DD:GG:NN:SS
  String calculateCRC16(const String& payload);    // jak w Twoim kodzie
}
