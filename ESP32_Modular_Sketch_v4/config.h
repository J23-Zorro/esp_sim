#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
// TinyGSM (PPPoS) płytka WeMOS LOLIN32 Lite
//#define TINY_GSM_MODEM_SIM7600  // <--- zmień jeśli masz inny modem (np. SIM7600)
//#include <TinyGsmClient.h>
/*
/upload – wgrywanie plików do LittleFS

/logs – lista logów, /download?file=log0.txt – pobranie

/ftp_queue_clear – wyczyszczenie kolejki

/fs – podgląd zawartości LittleFS
/menu
/mail

# Dodaj do kolejki (przykład)
curl -u user:pass -X POST "http://<ESP-IP>/ftpq/enqueue?file=/test.txt&dir=inbox"

# Podgląd
curl "http://<ESP-IP>/ftpq/stats"

# Wyczyść kolejkę
curl -u user:pass -X POST "http://<ESP-IP>/ftpq/clear"

Jak używać  Wejdź na: http://<ESP-IP>/measure — zobaczysz ostatnie 50 linii.

Zmień liczbę w polu i kliknij „Odśwież”, np. N=200.

*/




struct ConfigData {
  String net_mode = "wifi";  // "wifi" or "pppos"
  // WiFi
  String wifi_ssid = "UPC0041590";
  String wifi_pass = "AKz@_ori223_Zbv?";
  // PPPoS
  String apn = "playmetric";
  String pin = "";
  int uart_tx = 17;//26
  int uart_rx = 18;//27
  uint32_t uart_baud = 115200;
  // MQTT
  String mqtt_host = "broker.hivemq.com";
  int mqtt_port = 1883;
  String mqtt_user = "";
  String mqtt_pass = "";
  String mqtt_topic_pub = "esp32/status";
  // FTP
  String ftp_host = "ftp.itelemetria.pl";
  int ftp_port = 21;
  String ftp_user = "terminal100.jpalio";
  String ftp_pass = "term100";
  String ftp_dir = "/Dane";
  // Web UI auth
  String http_user = "admin";
  String http_pass = "admin123";
    // Interwał wysyłki na FTP (sekundy). Domyślnie 3600s = 1h
  uint32_t sendFTPInterval_sec = 3600;
  uint32_t cfgSyncInterval_sec = 900;   // interwał sprawdzania zdalnej konfiguracji (sek)
  bool     email_pop3_enabled   = true;   // włączony auto-check POP3
  uint32_t pop3CheckInterval_sec = 360;   // co ile sekund sprawdzać POP3
};

class Config {
 public:
  static void begin();
  static ConfigData& get();
  static bool save(const ConfigData& d);
 private:
  static bool load();
};
