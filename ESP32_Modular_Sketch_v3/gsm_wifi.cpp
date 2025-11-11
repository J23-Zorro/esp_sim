#include "gsm_wifi.h"
#include "config.h"
#include "log.h"
#include "led.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include "modem_select.h"
#include <TinyGsmClient.h>
#ifdef ARDUINO_ARCH_ESP32
  #include <esp_task_wdt.h>
  #define WDT_ADD()   do{ esp_task_wdt_add(NULL); }while(0)
  #define WDT_DEL()   do{ esp_task_wdt_delete(NULL); }while(0)
  #define WDT_FEED()  do{ esp_task_wdt_reset(); }while(0)
#else
  #define WDT_ADD()
  #define WDT_DEL()
  #define WDT_FEED()
#endif




//#define TINY_GSM_MODEM_SIM7070   // ← dopasuj do swojego modemu (np. SIM7600 / SIM800)
//#include <TinyGsmClient.h>

// --- Wi-Fi ---
static WiFiClient   s_wifiClient;
static bool         s_wifiOk = false;
static String       s_wifiIp = "0.0.0.0";
static bool         s_softAp = false;

// --- PPPoS (GSM) ---
static HardwareSerial SerialAT(1);
static TinyGsm*        s_modem     = nullptr;
static TinyGsmClient*  s_gsmClient = nullptr;
static bool            s_pppOk     = false;

static unsigned long s_pppNextAttempt = 0;
static uint8_t       s_pppAttempts    = 0;
static const unsigned long PPP_BASE_BACKOFF = 5000; // 5 s

// --- Polityka wyjścia (dla MQTT/FTP) ---
static inline bool usePPP() { return Config::get().net_mode == "pppos"; }

// ---------- Wi-Fi: STA z fallbackiem do AP ----------
static void begin_wifi() {
  const auto& c = Config::get();
  s_softAp = false;
  s_wifiOk = false;

  if (c.wifi_ssid.length()) {
    LOGI("WiFi STA: connect to %s", c.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(c.wifi_ssid.c_str(), c.wifi_pass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      s_wifiOk = true;
      s_wifiIp = WiFi.localIP().toString();
      LOGI("WiFi STA OK, IP=%s", s_wifiIp.c_str());
      return;
    }
    LOGW("WiFi STA failed, falling back to AP");
  }

  // Fallback: własny AP, żeby Web UI było zawsze dostępne
  WiFi.mode(WIFI_AP);
  const char* ap_ssid = "ESP32-Setup";
  const char* ap_pass = "esp32setup";
  WiFi.softAP(ap_ssid, ap_pass);
  s_softAp = true;
  s_wifiOk = true;
  s_wifiIp = WiFi.softAPIP().toString();
  LOGI("WiFi AP started: SSID=%s PASS=%s IP=%s", ap_ssid, ap_pass, s_wifiIp.c_str());
}

// ---------- PPPoS ----------
static void begin_pppos() {
  const auto& c = Config::get();
  s_pppOk = false;

  LOGI("PPPoS starting (APN=%s)", c.apn.c_str());

  // Na czas długich, blokujących wywołań wyrejestruj loopTask z WDT
  WDT_DEL();

  SerialAT.begin(c.uart_baud, SERIAL_8N1, c.uart_rx, c.uart_tx);
  if (!s_modem) s_modem = new TinyGsm(SerialAT);
  delay(300);

  (void)s_modem->restart();
  if (c.pin.length()) (void)s_modem->simUnlock(c.pin.c_str());

  // Czekaj na sieć max ~45 s, karmiąc WDT po drodze
  bool netOk = false;
  unsigned long t0 = millis();
  while (millis() - t0 < 45000) {
    if (s_modem->waitForNetwork()) { netOk = true; break; }
    WDT_FEED();
    delay(250);
  }
  if (!netOk) {
    LOGE("No network");
    // Przy kolejnej próbie zastosuj backoff
    s_pppAttempts = s_pppAttempts ? s_pppAttempts : 1;
    unsigned long backoff = PPP_BASE_BACKOFF << min<int>(s_pppAttempts, 6);
    s_pppNextAttempt = millis() + backoff + (unsigned long)random(0, 2000);
    WDT_ADD();    // ponownie obserwuj loopTask
    return;
  }

  // Połączenie danych – może blokować długo
  if (!s_modem->gprsConnect(c.apn.c_str())) {
    LOGE("GPRS connect failed");
    s_pppAttempts++;
    unsigned long backoff = PPP_BASE_BACKOFF << min<int>(s_pppAttempts, 6);
    s_pppNextAttempt = millis() + backoff + (unsigned long)random(0, 2000);
    WDT_ADD();
    return;
  }

  if (!s_gsmClient) s_gsmClient = new TinyGsmClient(*s_modem);
  s_pppOk = true;
  s_pppAttempts = 0;
  s_pppNextAttempt = millis();
  LOGI("PPPoS OK");

  // Po zakończeniu krytycznych sekcji z powrotem do WDT
  WDT_ADD();
}



// ---------- API ----------
void Net::begin() {
  // Web UI: zawsze Wi-Fi (STA lub AP)
  begin_wifi();

  // Wyjście (MQTT/FTP): wg net_mode
  if (usePPP()) {
    begin_pppos();
  }
}

void Net::loop() {
  // Utrzymuj status Wi-Fi (dla panelu)
  if (!s_softAp) {  // w AP nie sprawdzamy WL_CONNECTED
    if (WiFi.status() == WL_CONNECTED) {
      s_wifiOk = true;
      s_wifiIp = WiFi.localIP().toString();
    } else {
      if (s_wifiOk) LOGW("WiFi STA lost");
      s_wifiOk = false;
    }
  }




  // Utrzymuj PPPoS wg backoffu
  if (usePPP()) {
    // jeśli nie połączeni i pora na próbę – odpal sekwencję łączenia
    if (!s_pppOk && (long)(millis() - s_pppNextAttempt) >= 0) {
      begin_pppos(); // sam ustawi nextAttempt/backoff
    } else if (s_modem) {
      // jeżeli zrywa się połączenie – wyznacz kolejną próbę
      if (!s_modem->isGprsConnected()) {
        if (s_pppOk) LOGW("PPPoS lost");
        s_pppOk = false;
        if ((long)(millis() - s_pppNextAttempt) >= 0) {
          s_pppAttempts++;
          unsigned long backoff = PPP_BASE_BACKOFF << min<int>(s_pppAttempts, 6);
          s_pppNextAttempt = millis() + backoff + (unsigned long)random(0, 2000);
        }
      } else {
        s_pppOk = true;
        s_pppAttempts = 0;
      }
    }
  } else {
    s_pppOk = false;
  }
}

void Net::disposeClient(Client* c) {
  if (!c) return;
  if (usePPP()) {
    // utworzone jako: new TinyGsmClient(*s_modem)
    delete static_cast<TinyGsmClient*>(c);
  } else {
    // utworzone jako: new WiFiClient()
    delete static_cast<WiFiClient*>(c);
  }
}

bool Net::connected() {
  return usePPP() ? s_pppOk : s_wifiOk;
}

Client& Net::client() {
  if (usePPP() && s_gsmClient) return *s_gsmClient;
  return s_wifiClient;
}

Client* Net::newClient() {
  if (usePPP() && s_modem) return new TinyGsmClient(*s_modem);
  return new WiFiClient();
}

bool Net::wifiConnected() { return s_wifiOk; }
const char* Net::wifiIpStr() { return s_wifiIp.c_str(); }
