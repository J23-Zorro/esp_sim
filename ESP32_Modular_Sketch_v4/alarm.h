/*
#pragma once
#include <Arduino.h>

namespace Alarm {

// globalny stan – ustawiany przy aktywnym przynajmniej jednym alarmie
extern volatile bool WykrytoAlarm;

void begin();        // inicjalizacja IO i configu
void loopTick();     // wywołuj często (np. w main::loop) – kwalifikuje alarmy i zapisuje logi

// Ręczne: natychmiastowa rotacja pliku alarmowego i enqueue do FTP
bool rotateAndEnqueueNow();

// Ścieżki i rozmiar
const char* alarmBasePath(); // "/alarmy_<MAC>.txt"

} // namespace
*/

// alarm.h
#pragma once
#include <Arduino.h>

namespace Alarm {

extern volatile bool WykrytoAlarm;

void begin();
void loopTick();
bool rotateAndEnqueueNow();
const char* alarmBasePath();

// === NEW: live status snapshot ===
struct Status {
  bool     binIn[5];      // GPIO 13,15,21,22,23
  bool     relay[4];      // O1..O4 (GPIO 16..19)
  bool     anaActive[8];  // A001..A008 – czy w alarmie
  int8_t   anaSide[8];    // +1:HI, -1:LO, 0:OK
  double   anaVal[8];     // ostatnie przeliczone wartości (weADC?licz)
  bool     binActive[5];  // B001..B005 – czy w alarmie
  bool     any;           // czy jakikolwiek alarm aktywny
};

void getStatus(Status& out);  // wypełnia strukturę

} // namespace Alarm

