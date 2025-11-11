#include <Arduino.h>
#include "alarm.h"
#include "io_pins.h"
#include "alarm_config.h"
#include "adc_values.h"
#include "data_files.h"
#include "measurement.h"
#include "ftp_queue.h"
#include "config.h"
#include "log.h"
#include <LittleFS.h>
#include "email_alert.h"
#include <time.h>

//static String labelA(int i) {  // i = 0..7  => "A001".."A008"
//  char b[8]; snprintf(b, sizeof(b), "A%03d", i + 1); return String(b);
//}
//static String labelB(int i) {  // i = 0..4  => "B001".."B005"
 // char b[8]; snprintf(b, sizeof(b), "B%03d", i + 1); return String(b);
//}

namespace Alarm {

volatile bool WykrytoAlarm = false;

static const size_t ALARM_FILE_LIMIT = 50UL * 1024UL; // 50 kB
static String g_alarmBase; // "/alarmy_<MAC>.txt"

//inline String pad3(int n) { char b[4]; snprintf(b, sizeof(b), "%03d", n); return String(b); }
//inline String labelA(int i) { return String("A") + pad3(i + 1); }
//inline String labelB(int i) { return String("B") + pad3(i + 1); }

const char* alarmBasePath() { return g_alarmBase.c_str(); }

static String nowStamp() { return Measure::pomTimeStamp(); }

static void appendLine(const String& line) {
  File f = LittleFS.open(g_alarmBase, "a");
  if (!f) f = LittleFS.open(g_alarmBase, "w");
  if (f) { f.print(line); f.close(); }
}

static void ensureBase() {
  if (g_alarmBase.length()) return;
  g_alarmBase = String("/alarmy_") + DataFiles::macNoSep() + ".txt";
  if (!LittleFS.exists(g_alarmBase)) { File f = LittleFS.open(g_alarmBase, "w"); if (f) f.close(); }
}

static void logAlarm(const String& msg) {
  String line = nowStamp(); line += " ; "; line += msg; line += "\r\n";
  Serial.println(msg);
  appendLine(line);
}

static void maybeRotateAndEnqueue() {
  size_t s = 0;
  if (LittleFS.exists(g_alarmBase)) {
    File f = LittleFS.open(g_alarmBase, "r"); if (f) { s = f.size(); f.close(); }
  }
  if (s < ALARM_FILE_LIMIT) return;

  // rename -> epoch
  time_t t = time(nullptr);
  char ep[16]; snprintf(ep, sizeof(ep), "%lu", (unsigned long)t);
  String rotated = String("/alarmy_") + DataFiles::macNoSep() + "_" + ep + ".txt";

  if (!LittleFS.rename(g_alarmBase, rotated)) {
    LOGE("alarm log rotate rename failed");
    return;
  }
  // enqueue do FTP
  String dir = Config::get().ftp_dir;
  if (!FTPQ::enqueue(rotated.c_str(), dir.c_str())) {
    LOGE("FTP enqueue alarm log failed");
  }
}

// ====== STANY WEWNĘTRZNE AUTOMATÓW ======

struct AnaState {
  bool active = false;
  uint32_t epoch = 0;   // czas startu alarmu (EPOCH)
  bool    alarm = false;
  int8_t  side  = 0;     // +1 HI, -1 LO
  time_t  tStart = 0;    // początek przekroczenia
  uint16_t n = 0;        // licznik pomiarów podczas przekroczenia (po spełnieniu czasu)
  time_t  tStartNorm = 0;// początek powrotu do normy (z histerezą)
  uint16_t nNorm = 0;    // licznik pomiarów powrotu (po czasie)
};

struct BinState {
  bool active = false;
  uint32_t epoch = 0;   // czas startu alarmu (EPOCH)
  bool    alarm = false;
  time_t  tStart = 0;
  uint16_t n = 0;
  time_t  tStartNorm = 0;
  uint16_t nNorm = 0;
};

static AnaState aS[8];
static BinState bS[5];

// ====== API ======

bool rotateAndEnqueueNow() {
  ensureBase();
  size_t s = 0;
  if (LittleFS.exists(g_alarmBase)) {
    File f = LittleFS.open(g_alarmBase, "r"); if (f) { s = f.size(); f.close(); }
  }
  if (s == 0) return false;

  time_t t = time(nullptr);
  char ep[16]; snprintf(ep, sizeof(ep), "%lu", (unsigned long)t);
  String rotated = String("/alarmy_") + DataFiles::macNoSep() + "_" + ep + ".txt";
  if (!LittleFS.rename(g_alarmBase, rotated)) return false;

  String dir = Config::get().ftp_dir;
  return FTPQ::enqueue(rotated.c_str(), dir.c_str());
}

void begin() {
  ensureBase();
  IO::begin();
  AlarmCfg::load();
}

// ====== DETEKCJA ANALOG ======
static void processAnalog() {
  using namespace AlarmCfg;
  AlarmCfg::Config cfg = AlarmCfg::get();

  double vals[8] = { weADC1licz, weADC2licz, weADC3licz, weADC4licz,
                     weADC5licz, weADC6licz, weADC7licz, weADC8licz };
  time_t now = time(nullptr);

  for (int i=0;i<8;++i) {
    double v = vals[i];
    const auto& c = cfg.analog[i];
    auto& st = aS[i];

    bool overHi  = (v > c.hi);
    bool underLo = (v < c.lo);

    if (!st.alarm) {
      bool cond = overHi || underLo;
      if (cond) {
        if (st.tStart == 0) { st.tStart = now; st.n = 0; }
        if ((uint32_t)(now - st.tStart) >= c.timeSec) {
          if (st.n < 0xFFFF) ++st.n;
          if (st.n >= c.countReq) {
            st.alarm = true;
            st.side  = overHi ? +1 : -1;
            st.tStartNorm = 0; st.nNorm = 0;

            char yyy[8]; snprintf(yyy, sizeof(yyy), "A%03d", i+1);
            logAlarm(String("Alarm przekroczenie wartości ") + String(v,6) + " dla wejścia " + yyy);

            // --- e-mail: start alarmu ---
            st.epoch = (uint32_t)now;
            EmailAlert::notifyAlarmStart(String(yyy), v, st.epoch);

            maybeRotateAndEnqueue();
          }
        }
      } else {
        st.tStart = 0; st.n = 0;
      }
    } else {
      // powrót do normy z histerezą
      bool norm = false;
      if (st.side == +1) norm = (v <= (c.hi - c.hystP));
      if (st.side == -1) norm = (v >= (c.lo + c.hystN));

      if (norm) {
        if (st.tStartNorm == 0) { st.tStartNorm = now; st.nNorm = 0; }
        if ((uint32_t)(now - st.tStartNorm) >= c.timeSec) {
          if (st.nNorm < 0xFFFF) ++st.nNorm;
          if (st.nNorm >= c.countReq) {
            char yyy[8]; snprintf(yyy, sizeof(yyy), "A%03d", i+1);
            logAlarm(String("powrót sygnału do normy na wejściu ") + yyy);

            // --- e-mail: powrót do normy (z tym samym epoch co start) ---
            if (st.epoch != 0) {
              EmailAlert::notifyAlarmCleared(String(yyy), st.epoch);
            }

            st.alarm = false; st.side = 0;
            st.tStart = st.n = st.tStartNorm = st.nNorm = 0;
            st.epoch = 0;

            maybeRotateAndEnqueue();
          }
        }
      } else {
        st.tStartNorm = 0; st.nNorm = 0;
      }
    }
  }
}

// ====== DETEKCJA BINARNA ======
static void processBinary() {
  using namespace AlarmCfg;
  AlarmCfg::Config cfg = AlarmCfg::get();

  bool in[5]; IO::readBinaryInputs(in);
  time_t now = time(nullptr);

  for (int i=0;i<5;++i) {
    const auto& c = cfg.binary[i];
    auto& st = bS[i];
    if (c.mode == 2) { // nie monitoruj
      st.tStart = st.n = st.tStartNorm = st.nNorm = 0;
      continue;
    }

    bool alarmLevel = (c.mode == 1) ? true : false;
    bool cond = (in[i] == alarmLevel);

    if (!st.alarm) {
      if (cond) {
        if (st.tStart == 0) { st.tStart = now; st.n = 0; }
        if ((uint32_t)(now - st.tStart) >= c.timeSec) {
          if (st.n < 0xFFFF) ++st.n;
          if (st.n >= c.countReq) {
            st.alarm = true;
            st.tStartNorm = 0; st.nNorm = 0;

            char yyy[8]; snprintf(yyy, sizeof(yyy), "B%03d", i+1);
            logAlarm(String("Alarm przekroczenie wartości ") + (in[i] ? "1" : "0") + " dla wejścia " + yyy);

            // sterowanie wyjściem
            if (c.action != 0) {
              uint8_t x = (c.action / 10) % 10;
              uint8_t y = c.action % 10;
              IO::applyActionCode(c.action);
              char act[64]; snprintf(act, sizeof(act), "Sterowanie wyjścia O%u = %u od wejścia %s",
                                     (unsigned)x, (unsigned)y, yyy);
              logAlarm(String(act));
            }

            // --- e-mail: start alarmu binarnego ---
            st.epoch = (uint32_t)now;
            const double val = in[i] ? 1.0 : 0.0;
            EmailAlert::notifyAlarmStart(String(yyy), val, st.epoch);

            maybeRotateAndEnqueue();
          }
        }
      } else {
        st.tStart = 0; st.n = 0;
      }
    } else {
      // powrót do normy -> wejście != alarmLevel
      bool norm = (in[i] != alarmLevel);
      if (norm) {
        if (st.tStartNorm == 0) { st.tStartNorm = now; st.nNorm = 0; }
        if ((uint32_t)(now - st.tStartNorm) >= c.timeSec) {
          if (st.nNorm < 0xFFFF) ++st.nNorm;
          if (st.nNorm >= c.countReq) {
            char yyy[8]; snprintf(yyy, sizeof(yyy), "B%03d", i+1);
            logAlarm(String("powrót sygnału do normy na wejściu ") + yyy);

            // --- e-mail: powrót do normy (z tym samym epoch co start) ---
            if (st.epoch != 0) {
              EmailAlert::notifyAlarmCleared(String(yyy), st.epoch);
            }

            st.alarm = false;
            st.tStart = st.n = st.tStartNorm = st.nNorm = 0;
            st.epoch = 0;

            maybeRotateAndEnqueue();
          }
        }
      } else {
        st.tStartNorm = 0; st.nNorm = 0;
      }
    }
  }
}

void loopTick() {
  ensureBase();
  processAnalog();
  processBinary();

  // globalny WykrytoAlarm
  bool any = false;
  for (int i=0;i<8;++i) if (aS[i].alarm) { any = true; break; }
  if (!any) for (int i=0;i<5;++i) if (bS[i].alarm) { any = true; break; }
  WykrytoAlarm = any;
}

// ====== STATUS DLA WEBUI ======
void getStatus(Status& s) {
  IO::readBinaryInputs(s.binIn);
  for (int i=0;i<4;++i) s.relay[i] = IO::getRelay(i+1);

  s.anaVal[0]=weADC1licz; s.anaVal[1]=weADC2licz; s.anaVal[2]=weADC3licz; s.anaVal[3]=weADC4licz;
  s.anaVal[4]=weADC5licz; s.anaVal[5]=weADC6licz; s.anaVal[6]=weADC7licz; s.anaVal[7]=weADC8licz;

  for (int i=0;i<8;++i) { s.anaActive[i] = aS[i].alarm; s.anaSide[i] = aS[i].side; }
  for (int i=0;i<5;++i) { s.binActive[i] = bS[i].alarm; }

  s.any = WykrytoAlarm;
}

} // namespace Alarm
