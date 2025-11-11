#pragma once
#include <Arduino.h>

namespace EmailAlert {

// Inicjalizacja i pętla
void begin();
void loopTick();

// Ustawienia POP3 (włącz/wyłącz i interwał checku)
void setPop3Enabled(bool enabled);
void setPop3Interval(uint32_t seconds);

// EPOCH, na który czekamy (ustawiane po wysłaniu START)
void setPendingEpoch(uint32_t epoch);
bool     getPop3Enabled();
uint32_t getPop3Interval();
uint32_t getPendingEpoch();

// Test SMTP (pojedynczy adres)
bool testSend(const String& to, const String& subj, const String& body);

// Zgłoszenia z alarm.cpp
void notifyAlarmStart(const String& channelLabel, double value, uint32_t epoch);
void notifyAlarmCleared(const String& channelLabel, uint32_t epoch);

} // namespace EmailAlert
