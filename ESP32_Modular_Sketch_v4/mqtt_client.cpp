#include "mqtt_client.h"
#include "config.h"
#include "log.h"
#include "gsm_wifi.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

static PubSubClient* client = nullptr;
static unsigned long lastConnTry = 0;
static uint8_t backoffExp = 0;                 // 0..5 => 2s..30s
static const uint8_t BACKOFF_EXP_MAX = 5;      // 2^5=32(~30s limit)

// ── util: unikalny clientId z MAC (bez dwukropków)
static String makeClientId() {
  uint8_t m[6]; WiFi.macAddress(m);
  char b[32];
  snprintf(b, sizeof(b), "esp32-%02X%02X%02X%02X%02X%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(b);
}

static void ensureClient() {
  if (client) return;
  client = new PubSubClient();
  auto& cfg = Config::get();
  client->setServer(cfg.mqtt_host.c_str(), cfg.mqtt_port);

  // Krótkie time-outy, mniejsze ryzyko WDT w connect()
  client->setKeepAlive(30);
  client->setSocketTimeout(2);   // 2 s blokady max w connect()
  // client->setBufferSize(1024); // jeśli kiedyś będziesz wysyłać większe payloady
}

// zwraca aktualne opóźnienie w ms (2s..30s)
static unsigned long backoffDelayMs() {
  unsigned long d = 2000UL << (backoffExp > BACKOFF_EXP_MAX ? BACKOFF_EXP_MAX : backoffExp); // 2s * 2^n
  if (d > 30000UL) d = 30000UL;
  return d;
}

static void tryConnectNonBlocking() {
  if (!Net::connected() || !client) return;
  if (client->connected()) return;

  unsigned long now = millis();
  unsigned long delayMs = backoffDelayMs();
  if (now - lastConnTry < delayMs) return;

  lastConnTry = now;
  LOGI("MQTT connecting... (backoff=%lus)", delayMs/1000UL);

  esp_task_wdt_reset();
  yield();

  // Uwaga: connect() jest blokujące, ale max 2 s dzięki setSocketTimeout(2)
  String cid = makeClientId();
  bool ok = client->connect(cid.c_str(),
                            Config::get().mqtt_user.c_str(),
                            Config::get().mqtt_pass.c_str());

  esp_task_wdt_reset();
  yield();

  if (ok) {
    LOGI("MQTT connected.");
    backoffExp = 0;  // reset backoff po sukcesie
    // tutaj ewentualne subskrypcje:
    // client->subscribe("x/y");
  } else {
    int rc = client->state();
    LOGW("MQTT failed rc=%d", rc);
    if (backoffExp < BACKOFF_EXP_MAX) backoffExp++;
  }
}

void Mqtt::begin() {
  ensureClient();
  client->setClient(Net::client());   // podłącz aktualnego Clienta z warstwy Net
}

void Mqtt::loop() {
  if (!client) return;

  // upewnij się, że PubSubClient operuje na aktualnym gnieździe z Net
  client->setClient(Net::client());

  // szybki reconnect bez blokowania pętli
  if (!client->connected()) {
    tryConnectNonBlocking();
    return; // nie wołamy loop() gdy niepołączony – i tak nic nie zrobi
  }

  // Utrzymanie połączenia/pingi – lekkie
  client->loop();

  // Karm WDT przy intensywniejszym ruchu
  esp_task_wdt_reset();
}

bool Mqtt::publish(const char* topic, const char* payload) {
  if (!client || !client->connected()) return false;
  return client->publish(topic, payload);
}
