#include <Arduino.h>
#include "measurement.h"
#include "adc_mcp3424.h"
#include "adc_values.h"
#include "data_files.h"
#include "ftp_queue.h"
#include "config.h"
#include "log.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>

static time_t alignToNoonEdge(time_t now, uint32_t intervalSec);  // fwd decl

// ================== Zmienne globalne modułu ==================
namespace Measure {
  // interwały [s]
  uint32_t pomiarADCInterval = 600; // domyślnie 600 s
  uint32_t pomiarMCPInterval = 1;   // domyślnie 1 s (próbkowanie MCP)

  // ostatnia „krawędź” do zapisu wg ADC
  time_t   lastPomiarUploadTime = 0;
}

// Interwał wysyłki FTP + stan wewnętrzny harmonogramu
namespace Measure {
  uint32_t sendFTPInterval = 3600;   // [s] domyślnie 1h
  static time_t lastSendEdge = 0;    // ostatnia krawędź wysyłki od 12:00
  static String pendingSnap;         // ścieżka snapshota czekającego na sukces FTP
}

namespace Measure {
  void setSendFTPInterval(uint32_t sec) {
    sendFTPInterval = (sec == 0 ? 1 : sec);
    time_t now = time(nullptr);
    lastSendEdge = alignToNoonEdge(now, sendFTPInterval);
  }

  uint32_t getSendFTPInterval() {
    return sendFTPInterval;
  }
}

// ================== Helpers (internal) ==================

static String two(uint32_t v){
  char b[3]; snprintf(b, sizeof(b), "%02u", (unsigned)(v % 100));
  return String(b);
}

// RR:MM:DD:GG:NN:SS (rok 2-cyfrowo)
namespace Measure {
  String pomTimeStamp() {
    time_t t = time(nullptr);
    struct tm tmv; localtime_r(&t, &tmv);
    String yy = two((tmv.tm_year + 1900) % 100);
    String mo = two(tmv.tm_mon + 1);
    String dd = two(tmv.tm_mday);
    String hh = two(tmv.tm_hour);
    String mi = two(tmv.tm_min);
    String ss = two(tmv.tm_sec);
    return yy + ":" + mo + ":" + dd + ":" + hh + ":" + mi + ":" + ss;
  }
} // namespace Measure

// CRC16 Modbus (poly 0xA001, init 0xFFFF)
namespace Measure {
  String calculateCRC16(const String& data) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < data.length(); ++i) {
      crc ^= (uint8_t)data[i];
      for (uint8_t b = 0; b < 8; ++b) {
        if (crc & 1) crc = (crc >> 1) ^ 0xA001;
        else         crc = (crc >> 1);
      }
    }
    String s = String(crc, HEX);
    s.toLowerCase();
    return s;
  }
} // namespace Measure

// mapowanie RSSI -> 0..99
static uint8_t wifiPercent0_99() {
  long rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50)  return 99;
  float p = (rssi + 100.0f) * (99.0f / 50.0f);
  if (p < 0) p = 0; if (p > 99) p = 99;
  return (uint8_t)(p + 0.5f);
}

// losowe "xyz" 000..100
static String xyz000_100() {
  long v = random(101);
  char b[4]; snprintf(b, sizeof(b), "%03ld", v);
  return String(b);
}

// zwraca ostatnią „krawędź” k*interval od lokalnej 12:00 (w przeszłości)
static time_t alignToNoonEdge(time_t now, uint32_t intervalSec) {
  struct tm tmv; localtime_r(&now, &tmv);
  tmv.tm_hour = 12; tmv.tm_min = 0; tmv.tm_sec = 0;
  time_t base = mktime(&tmv);
  if (now < base) base -= 24 * 3600;            // jeśli przed 12:00 -> wczoraj 12:00
  if (intervalSec == 0) return base;
  time_t delta = now - base;
  return base + (delta / intervalSec) * intervalSec;
}

// egzekwowanie warunku 8*MCP < ADC
static void enforceMcpVsAdc() {
  using namespace Measure;
  if (pomiarADCInterval == 0) pomiarADCInterval = 1;
  if (pomiarMCPInterval == 0) pomiarMCPInterval = 1;

  if (8ULL * pomiarMCPInterval >= pomiarADCInterval) {
    uint32_t allowed = (pomiarADCInterval > 1) ? (uint32_t)((pomiarADCInterval - 1) / 8) : 0;
    uint32_t old = pomiarMCPInterval;
    if (allowed < 1) {
      pomiarMCPInterval = 1;
      LOGW("MCP interval adjusted to 1s; increase pomiarADCInterval to >8s to satisfy 8*MCP<ADC (ADC=%lus).",
           (unsigned long)pomiarADCInterval);
    } else {
      pomiarMCPInterval = allowed;
      LOGW("Adjusted pomiarMCPInterval from %lus to %lus to satisfy 8*MCP<ADC (ADC=%lus).",
           (unsigned long)old, (unsigned long)pomiarMCPInterval, (unsigned long)pomiarADCInterval);
    }
  }
}

// ================== API ==================

namespace Measure {

void setPomiarInterval(uint32_t sec) {
  pomiarADCInterval = (sec == 0 ? 1 : sec);
  enforceMcpVsAdc();
  time_t now = time(nullptr);
  lastPomiarUploadTime = alignToNoonEdge(now, pomiarADCInterval);
}

void setMCPInterval(uint32_t sec) {
  pomiarMCPInterval = (sec == 0 ? 1 : sec);
  enforceMcpVsAdc();
}

void startMCP3424() {
  ::startMCP3424();                  // driver MCP3424
  randomSeed((uint32_t)esp_random());

  enforceMcpVsAdc();

  time_t now = time(nullptr);
  lastPomiarUploadTime = alignToNoonEdge(now, pomiarADCInterval);
  LOGI("Measurement started: ADC int=%lus, MCP int=%lus, current file=%s",
       (unsigned long)pomiarADCInterval, (unsigned long)pomiarMCPInterval,
       DataFiles::pathCurrent().c_str());
}

void pomiarMCP3424() {
  ::pomiarMCP3424();                 // szybki odczyt MCP3424
}

// Buduje 12 rekordów: A001..A004, AKU, B001, UAZS, A005..A008
void myTestPomiar() {
  // 1) przeliczenie A*x+B
  pomiarADClicz();

  // 2) stałe dla całego bloku wpisów
  const String IMEI  = DataFiles::macNoSep();   // MAC zamiast IMEI
  const String Rczas = pomTimeStamp();          // RR:MM:DD:GG:NN:SS
  const String xyz   = xyz000_100();            // wspólny xyz
  const String poziom_GSM = String((unsigned)wifiPercent0_99());
  const String stan_wyjsc = "0000";

  auto appendRecord = [&](const String& Czujnik, const String& raw, const String& calc){
    const String crcBase = IMEI + ";" + Czujnik + ";" + Rczas + ";" + xyz + ";" +
                           raw + ";" + calc + ";" + poziom_GSM + ";" + stan_wyjsc;
    const String CRC16 = calculateCRC16(crcBase);

    String line; line.reserve(128);
    line += IMEI; line+=';'; line+=Czujnik; line+=';'; line+=Rczas; line+=';';
    line+=xyz; line+=';'; line+=raw; line+=';'; line+=calc; line+=';';
    line+=poziom_GSM; line+=';'; line+=stan_wyjsc; line+=';'; line+=CRC16; line+=";\r\n";

    if (!DataFiles::appendLineToCurrent(line)) {
      LOGE("appendLineToCurrent failed");
    }
  };

  // A001..A004
  appendRecord("A001", String(weADC1,6), String(weADC1licz,6));
  appendRecord("A002", String(weADC2,6), String(weADC2licz,6));
  appendRecord("A003", String(weADC3,6), String(weADC3licz,6));
  appendRecord("A004", String(weADC4,6), String(weADC4licz,6));
  // dodatki
  appendRecord("AKU",  "1071",  "6.9912345");
  appendRecord("B001", "1",     "1.0");
  appendRecord("UAZS", "1",     "1.0");
  // A005..A008
  appendRecord("A005", String(weADC5,6), String(weADC5licz,6));
  appendRecord("A006", String(weADC6,6), String(weADC6licz,6));
  appendRecord("A007", String(weADC7,6), String(weADC7licz,6));
  appendRecord("A008", String(weADC8,6), String(weADC8licz,6));
}

// Jeśli (a) plik osiągnął limit 100 kB LUB (b) minął kolejny „tik” interwału od 12:00,
// to rotacja i enqueue bieżącego _1 (przez snapshot, oczekując na sukces FTP).
void WyslijDaneNaFTP() {
  using namespace DataFiles;

  // jeżeli czekamy na zakończenie poprzedniej wysyłki (snapshot jeszcze istnieje), nic nie rób
  if (pendingSnap.length() > 0) {
    if (!LittleFS.exists(pendingSnap)) {
      // snapshot zniknął -> kolejka zakończyła sukcesem -> ROTACJA
      if (rotateAfterSend()) {
        LOGI("Rotation after send done.");
      } else {
        LOGE("Rotation after send FAILED.");
      }
      pendingSnap = "";
    }
    // kiedy snapshot istnieje – upload w toku, wstrzymaj nowe wysyłki
    if (pendingSnap.length() > 0) return;
  }

  // a) rozmiar
  bool dueBySize = (fileSize(pathCurrent()) >= FILE_SIZE_LIMIT);

  // b) czas wg sendFTPInterval (kotwica 12:00)
  time_t now = time(nullptr);
  time_t edge = alignToNoonEdge(now, sendFTPInterval);
  bool dueByTime = (edge > lastSendEdge);

  if (!(dueBySize || dueByTime)) return;

  // Przygotuj snapshot aktualnego D_<MAC>.txt
  String src = pathCurrent();
  String snap = makeUploadSnapshotPath();

  if (!copyFile(src, snap)) {
    LOGE("Snapshot copy failed: %s -> %s", src.c_str(), snap.c_str());
    return;
  }

  // Enqueue snap – na serwerze i tak nazwa końcowa będzie D_<MAC>_<EPOCH>.txt
  String dir = Config::get().ftp_dir;
  if (FTPQ::enqueue(snap.c_str(), dir.c_str())) {
    pendingSnap = snap;
    LOGI("Enqueued snapshot: %s (trigger=%s)", snap.c_str(), dueBySize ? "SIZE" : "TIME");
    if (dueByTime) lastSendEdge = edge;
  } else {
    LOGE("FTP enqueue failed for %s", snap.c_str());
    LittleFS.remove(snap); // sprzątanie
  }
}

// Główna pętla: rozdzielone harmonogramy MCP (szybciej) i zapisu (wolniej)
void loopTick() {
  static uint32_t lastMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastMs < 250) return;  // „oddech” co ~250 ms
  lastMs = nowMs;

  time_t now = time(nullptr);

  // 1) Harmonogram MCP: niezależny interwał (też „siatka” od 12:00)
  static time_t lastMcpEdge = 0;
  time_t mcpEdge = alignToNoonEdge(now, pomiarMCPInterval);
  if (mcpEdge > lastMcpEdge) {
    lastMcpEdge = mcpEdge;
    pomiarMCP3424();
  }

  // 2) Harmonogram zapisu rekordu: wg pomiarADCInterval (jak było)
  static time_t lastAdcEdge = 0;
  time_t adcEdge = alignToNoonEdge(now, pomiarADCInterval);
  if (adcEdge > lastAdcEdge) {
    lastAdcEdge = adcEdge;
    myTestPomiar();
  }

  // 3) Warunki wysyłki
  WyslijDaneNaFTP();
}

} // namespace Measure
