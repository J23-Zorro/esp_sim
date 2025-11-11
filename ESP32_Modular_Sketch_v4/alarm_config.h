#pragma once
#include <Arduino.h>

namespace AlarmCfg {

struct AnalogCfg {
  double  hi;       // próg górny
  double  lo;       // próg dolny
  double  hystP;    // histereza dla powrotu z alarmu „HI”: v <= hi - hystP
  double  hystN;    // histereza dla powrotu z alarmu „LO”: v >= lo + hystN
  uint16_t timeSec; // czas kwalifikacji (sekundy)
  uint16_t countReq;// ile kolejnych pomiarów po czasie (0..256)
};

struct BinaryCfg {
  uint8_t  mode;     // 0 alarm na 0, 1 alarm na 1, 2=wyłączone
  uint8_t  action;   // XY: X=wyjście 1..4, 0=brak; Y=0/1
  uint16_t timeSec;  // czas kwalifikacji
  uint16_t countReq; // liczba pomiarów po czasie
};

struct Config {
  AnalogCfg analog[8];
  BinaryCfg binary[5];
};

Config get();              // odczyt z RAM (po load())
void set(const Config&);   // ustaw w RAM (i zapisz)
bool load();               // z LittleFS (/alarm_config.json), gdy brak -> defaults i save()
bool save();               // do LittleFS

// ułatwienia
void defaults(Config& c);

} // namespace
