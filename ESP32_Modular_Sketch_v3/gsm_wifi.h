#pragma once
#include <Arduino.h>
#include <Client.h>   // wspólna baza dla WiFiClient / TinyGsmClient

namespace Net {
  void begin();                 // start Wi-Fi (z AP fallback) + ewentualnie PPPoS
  void loop();

  // Dla logiki aplikacji (MQTT/FTP):
  bool connected();             // true = aktywne łącze wyjściowe: Wi-Fi (net_mode=wifi) lub PPPoS (net_mode=pppos)
  Client& client();             // zwraca referencję do aktywnego klienta (Wi-Fi lub PPPoS)
  Client* newClient();          // NOWE: utwórz nowego klienta odpowiedniego typu (delete po użyciu)
   void    disposeClient(Client* c); // DODAJ TO
  // Dla Web UI (zawsze Wi-Fi):
  bool wifiConnected();         // Wi-Fi STA/SoftAP aktywne
  const char* wifiIpStr();      // IP do panelu WWW (STA lub 192.168.4.1 w AP)
}

