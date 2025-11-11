#pragma once
#include <Arduino.h>

namespace CfgSync {
  // Ustaw / pobierz interwał sprawdzania (sekundy)
  void   setInterval(uint32_t sec);
  uint32_t getInterval();

  // Inicjacja / pętla
  void begin();
  void loopTick();

  // Ręczne sprawdzenie teraz (zwraca true jeśli wykryto i przetworzono configZ.txt)
  bool runOnceNow();
}
