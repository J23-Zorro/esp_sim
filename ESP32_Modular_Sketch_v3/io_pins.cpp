#include "io_pins.h"

namespace IO {

void begin() {
  // wejścia z pullup (dla 13 – jeśli masz HW pull-down/pull-up, dopasuj)
  for (uint8_t i=0;i<5;++i) pinMode(BIN_IN_PINS[i], INPUT_PULLUP);
  // wyjścia: stan niski = przekaźnik wyłączony (dopasuj do swojej logiki)
  for (uint8_t i=0;i<4;++i) { pinMode(RELAY_OUT_PINS[i], OUTPUT); digitalWrite(RELAY_OUT_PINS[i], LOW); }
}

void readBinaryInputs(bool out[5]) {
  for (uint8_t i=0;i<5;++i) {
    out[i] = (digitalRead(BIN_IN_PINS[i]) == HIGH);
  }
}

void setRelay(uint8_t idx, bool on) {
  if (idx < 1 || idx > 4) return;
  uint8_t pin = RELAY_OUT_PINS[idx-1];
  digitalWrite(pin, on ? HIGH : LOW);
}

bool getRelay(uint8_t idx) {
  if (idx < 1 || idx > 4) return false;
  return digitalRead(RELAY_OUT_PINS[idx-1]) == HIGH;
}

} // namespace
