#pragma once
#include <Arduino.h>

namespace IO {

// Wejścia binarne (5 szt.)
//static const uint8_t BIN_IN_PINS[5] = { 13, 15, 21, 22, 23 };
static const uint8_t BIN_IN_PINS[5] = { 4, 5, 6, 7, 8 };
// Wyjścia (przekaźniki) (4 szt.)
//#if CONFIG_IDF_TARGET_ESP32S3
   static const uint8_t RELAY_OUT_PINS[4] = { 11, 12, 13, 14 };
//#else
//   static const uint8_t RELAY_OUT_PINS[4] = { 16, 17, 18, 19 };
void begin();
void readBinaryInputs(bool out[5]);

// idx: 1..4
void setRelay(uint8_t idx, bool on);
bool getRelay(uint8_t idx);

// Akcja „XY”: X=1..4 (0=brak), Y=0/1 – do mapowania z alarmu binarnego
inline void applyActionCode(uint8_t code) {
  uint8_t x = (code / 10) % 10;
  uint8_t y = code % 10;
  if (x >= 1 && x <= 4 && (y == 0 || y == 1)) setRelay(x, y);
}

} // namespace IO
