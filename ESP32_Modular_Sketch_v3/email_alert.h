#pragma once
#include <Arduino.h>
#include <vector>

namespace EmailAlert {

// Wywołaj z setupu
void begin();

// Wołaj często (np. w loop)
void loopTick();

// Zgłoszenie startu alarmu
void notifyAlarmStart(const String& yyy, double value, uint32_t epoch);

// Zgłoszenie powrotu do normy
void notifyAlarmCleared(const String& yyy, uint32_t epoch);

// (opcjonalnie) szybki test wysyłki
bool testSend(const String& to, const String& subject, const String& body);

} // namespace EmailAlert
