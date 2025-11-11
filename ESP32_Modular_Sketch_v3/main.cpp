#include <Arduino.h>
#include "config.h"
#include "log.h"
#include "led.h"
#include "gsm_wifi.h"
#include "mqtt_client.h"
#include "ftp_upload.h"
#include "ftp_queue.h"
#include "web_ui.h"
#include <LittleFS.h>
#include "esp_task_wdt.h"
// #include "ftp_queue.h" // duplikat – usunięty
#include "adc_mcp3424.h"
#include "measurement.h"
#include "io_pins.h"
#include "alarm.h"
#include "email_alert.h"
#include "cfg_sync.h"


static const int WDT_TIMEOUT_SEC = 60;

void setup() {
  Serial.begin(115200);
  delay(200);

  // Diagnoza pamięci (po ustawieniu PSRAM=Enabled (OPI) w Tools) potem wyłączyć
size_t flash = ESP.getFlashChipSize();  
size_t ps = ESP.getPsramSize();
Serial.printf("Flash: %lu MB, PSRAM: %lu MB\n",
              (unsigned long)(flash / 1024UL / 1024UL),
              (unsigned long)(ps    / 1024UL / 1024UL));

if (ps > 0 && psramFound()) {
  Serial.println("PSRAM OK");
  // próba alokacji 2 MB w PSRAM
  uint8_t* buf = (uint8_t*) ps_malloc(2 * 1024 * 1024);
  if (buf) {
    memset(buf, 0xAA, 2 * 1024 * 1024);
    free(buf);
    Serial.println("PSRAM alloc OK");
  } else {
    Serial.println("PSRAM alloc FAIL");
  }
} else {
  Serial.println("PSRAM not present / failed");
}
//-------------


  Log::begin();
  LOGI("Booting...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(LEDrgb_PIN, OUTPUT);
  digitalWrite(LEDrgb_PIN, HIGH);

  // 1) LittleFS – montuj na starcie, z auto-formatem przy 1. użyciu
  if (!LittleFS.begin(true)) {
    LOGE("LittleFS mount failed (even after format)");
  } else {
    LOGI("LittleFS OK: total=%u used=%u",
         (unsigned)LittleFS.totalBytes(),
         (unsigned)LittleFS.usedBytes());
    // Zapewnienie minimalnego index.html (fallback)
    if (!LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "w");
      if (f) { f.print("<!doctype html><meta charset='utf-8'><h1>Upload index.html</h1>"); f.close(); }
    }
  }

  // 2) Konfiguracja + LED
  Config::begin();
  Led::begin();
  Measure::setSendFTPInterval(Config::get().sendFTPInterval_sec);
  // 3) Watchdog
// Watchdog (bez podwójnej inicjalizacji)

#if ESP_IDF_VERSION_MAJOR >= 5
  const esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WDT_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true,
  };
  esp_task_wdt_deinit();                 // << kluczowe
  ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_config));
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
#else
  esp_task_wdt_deinit();                 // << kluczowe
  ESP_ERROR_CHECK(esp_task_wdt_init(WDT_TIMEOUT_SEC, true));
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
#endif


  // 4) Sieć (Wi-Fi/PPPoS)
  Net::begin();

  // 5) Alarmy (UWAGA: Alarm::begin() samo wywołuje IO::begin())
  // -> nie trzeba wołać IO::begin() osobno
  Alarm::begin();

  // 6) MQTT
  Mqtt::begin();

  // 7) Web UI (po sieci i po FS)
  WebUI::begin();
  CfgSync::begin();
  // opcjonalnie: startowy interwał np. 15 min – albo weź z Config (możesz dodać pole do ConfigData)
  // CfgSync::setInterval(900);
  CfgSync::setInterval(Config::get().cfgSyncInterval_sec);//ważne by setInterval() mieć po Config::begin().
  // 8) FTP Queue (po FS)
  FTPQ::begin();
  FTPQ::setDeleteLocalOnSuccess(true); // po sukcesie usuń snapshot lokalny

  // 9) E-mail alerty
  EmailAlert::begin();
delay(100);
  // 10) Pomiary
  // Ustaw interwały (sekundy); pilnuj: 8 * pomiarMCPInterval < pomiarADCInterval
  Measure::pomiarADCInterval = 600;  // np. 10 min
  Measure::pomiarMCPInterval = 1;    // np. 5 s  => 8*5=40 < 600  (warunek spełniony)
  Measure::startMCP3424();           // start I2C + reset MCP3424

  // Plik testowy do szybkiej próby FTP/UI
  if (!LittleFS.exists("/test.txt")) {
    File f = LittleFS.open("/test.txt", "w");
    if (f) { f.println("Hello from ESP32-S3!"); f.close(); }
  }

  LOGI("Setup done.");
  delay(1000);
  digitalWrite(LEDrgb_PIN, LOW);
}

void loop() {
  Led::loop();
  // FTP kolejka – lekki tick (1 zadanie / iteracja jeśli warunki sprzyjają)
  (void)FTPQ::tick();

  Net::loop();
  Mqtt::loop();
  WebUI::loop();
  CfgSync::loopTick();

  // Pomiary / zapisy / wysyłka wg interwałów opisanych w Measure
  Measure::loopTick();

  // Alarmy: detekcja i log/akcje
  Alarm::loopTick();

  // E-mail alerty: sekwencer (wysyłka/odbiór/eskalacja)
  EmailAlert::loopTick();

  // Przykładowy publish co ~10 s
  static unsigned long lastPub = 0;
  if (millis() - lastPub > 10000) {
    lastPub = millis();
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"uptime\":%lu}", (unsigned long)(millis()/1000UL));
    Mqtt::publish(Config::get().mqtt_topic_pub.c_str(), payload);
  }

  esp_task_wdt_reset();
  delay(10);
}
