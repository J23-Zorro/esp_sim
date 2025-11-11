#include "email_alert.h"
#include "email_client.h"
#include "email_config.h"
#include "alarm.h"
#include "data_files.h"
#include "measurement.h"   // Measure::pomTimeStamp()
#include "log.h"
#include <LittleFS.h>
#include <vector>
#include <WiFi.h>

// ====== STAN I POMOCNICZE ======
namespace {

// POP3 polling
bool     g_pop3_enabled   = true;
uint32_t g_pop3_interval  = 300;   // [s]
uint32_t g_next_pop3_ms   = 0;

// Backoff do POP3 (gdy brak ACK / błąd)
uint32_t g_backoff_sec    = 10;
constexpr uint32_t BACKOFF_MIN = 10;
constexpr uint32_t BACKOFF_MAX = 600;

// Oczekiwany ACK dla alarmu
uint32_t g_pending_epoch  = 0;

// Dane do eskalacji jednego aktywnego alarmu
struct EscState {
  bool     active = false;          // czy prowadzimy eskalację
  uint8_t  stage  = 0;              // 0: nic nie wysłano, 1: G1 sent, 2: G2 sent, 3: G3 sent
  uint32_t next_escalation_ms = 0;  // kiedy podjąć następną eskalację (millis)
  String   channel;                 // np. "A001", "B003"
  double   value = 0;               // wartość z pomiaru przy starcie
  uint32_t epoch = 0;               // EPOCH tego alarmu (ACK dotyczy tego EPOCH)
  String   ts_at_start;             // timestamp z momentu startu (ładny opis)
} g_escalation;

// --- harmonogramy ---
inline void schedule_pop3_in_ms(uint32_t d) { g_next_pop3_ms = millis() + d; }
inline void schedule_pop3_in_s (uint32_t d) { schedule_pop3_in_ms(d * 1000UL); }
inline void schedule_pop3_norm()            { schedule_pop3_in_s(g_pop3_interval); g_backoff_sec = BACKOFF_MIN; }

inline void grow_backoff() {
  if (g_backoff_sec < BACKOFF_MIN) g_backoff_sec = BACKOFF_MIN;
  else {
    uint32_t d = g_backoff_sec * 2;
    g_backoff_sec = (d > BACKOFF_MAX) ? BACKOFF_MAX : d;
  }
}

// Mini log (opcjonalnie)
void appendToLog(const String& line) {
  File f = LittleFS.open("/log0.txt", "a");
  if (f) {
    f.print(String(millis())); f.print(" [INFO] ");
    f.print(line); f.print("\r\n");
    f.close();
  }
}

// CSV → lista e-maili (separatory: przecinek, średnik, spacja)
std::vector<String> splitCsvEmails(const String& csv) {
  std::vector<String> out;
  String tok;
  for (size_t i = 0; i < csv.length(); ++i) {
    char c = csv[i];
    if (c == ',' || c == ';' || c == ' ') {
      if (tok.length()) { out.push_back(tok); tok = ""; }
    } else {
      tok += c;
    }
  }
  if (tok.length()) out.push_back(tok);

  std::vector<String> filtered;
  for (auto &s : out) if (s.length()) filtered.push_back(s);
  return filtered;
}

// Wspólne budowanie tematu i treści START
void buildAlarmStartMail(const String& channel, double value, uint32_t epoch,
                         String& subj, String& body, const String& ts)
{
  char subjBuf[160];
  snprintf(subjBuf, sizeof(subjBuf), "ALARM START %s EPOCH=%lu",
           channel.c_str(), (unsigned long)epoch);
  subj = subjBuf;

  body.reserve(256);
  body  = "Wykryto alarm na kanale: "; body += channel; body += "\r\n";
  body += "Wartość: "; body += String(value, 6); body += "\r\n";
  body += "Czas: "; body += ts; body += "\r\n";
  body += "EPOCH: "; body += String((unsigned long)epoch); body += "\r\n";
}

// Wspólne budowanie tematu i treści CLEARED
void buildAlarmClearedMail(const String& channel, uint32_t epoch,
                           String& subj, String& body, const String& ts)
{
  char subjBuf[160];
  snprintf(subjBuf, sizeof(subjBuf), "ALARM CLEARED %s EPOCH=%lu",
           channel.c_str(), (unsigned long)epoch);
  subj = subjBuf;

  body.reserve(192);
  body  = "Alarm zakończony na kanale: "; body += channel; body += "\r\n";
  body += "Czas: "; body += ts; body += "\r\n";
  body += "EPOCH: "; body += String((unsigned long)epoch); body += "\r\n";
}

// Wysyłka do wybranej grupy (1..3). Zwraca true, jeśli cokolwiek wysłano.
bool sendToGroup(uint8_t groupNo, const String& subj, const String& body) {
  EmailCfg::Settings s; EmailCfg::load(s);
  String csv;
  switch (groupNo) {
    case 1: csv = s.group1_csv; break;
    case 2: csv = s.group2_csv; break;
    case 3: csv = s.group3_csv; break;
    default: return false;
  }
  auto rcpts = splitCsvEmails(csv);
  if (rcpts.empty()) { LOGW("EmailAlert: group %u empty, skip.", groupNo); return false; }
  bool ok = Email::sendSMTP(subj, body, rcpts);
  if (ok) LOGI("EmailAlert: sent to Group%u (%u rcpts).", groupNo, (unsigned)rcpts.size());
  else    LOGE("EmailAlert: send to Group%u FAILED.", groupNo);
  return ok;
}

// Zwraca czas oczekiwania (sek) na eskalację z danej grupy
uint32_t waitAfterStage(uint8_t stageJustSent/*1..3*/) {
  EmailCfg::Settings s; EmailCfg::load(s);
  if (stageJustSent == 1) return s.wait_g1 > 0 ? s.wait_g1 : 60;
  if (stageJustSent == 2) return s.wait_g2 > 0 ? s.wait_g2 : 120;
  if (stageJustSent == 3) return s.wait_g3 > 0 ? s.wait_g3 : 180;
  return 60;
}

} // namespace

// ====== API ======
namespace EmailAlert {

void begin() {
  EmailCfg::Settings s; EmailCfg::load(s);
  g_pop3_enabled = s.enabled;
  if (g_pop3_interval < 30) g_pop3_interval = 30;

  schedule_pop3_in_s(5); // pierwszy check po starcie
  LOGI("EmailAlert::begin POP3=%s, interval=%lus",
       g_pop3_enabled ? "ON" : "OFF",
       (unsigned long)g_pop3_interval);
}

void loopTick() {
  const uint32_t nowMs = millis();

  // --- 1) Eskalacja (czasowa) jeśli brak ACK i upłynął deadline ---
  if (g_escalation.active && g_pending_epoch != 0 && WiFi.isConnected()) {
    if (nowMs >= g_escalation.next_escalation_ms) {
      // Jeśli osiągnęliśmy deadline i nadal brak ACK, wyślij do kolejnej grupy
      if (g_escalation.stage < 3) {
        const uint8_t nextGroup = g_escalation.stage + 1;
        String subj, body;
        buildAlarmStartMail(g_escalation.channel, g_escalation.value,
                            g_escalation.epoch, subj, body, g_escalation.ts_at_start);
        bool sent = sendToGroup(nextGroup, subj, body);
        if (sent) {
          g_escalation.stage = nextGroup; // zarejestrowano wysyłkę tej grupy
          const uint32_t waitSec = waitAfterStage(g_escalation.stage);
          g_escalation.next_escalation_ms = nowMs + waitSec * 1000UL;
          LOGI("EmailAlert: escalated to Group%u, next check in %us.",
               (unsigned)nextGroup, (unsigned)waitSec);
        } else {
          // jeśli pusta lista dla tej grupy — przeskocz od razu do kolejnej
          g_escalation.stage = nextGroup; // ale bez zmiany deadline: spróbujemy „od razu” kolejnej w następnym obrocie
          g_escalation.next_escalation_ms = nowMs + 2000; // krótki oddech
        }
      } else {
        // Stage==3 => wszystko już wysłane, dalszej eskalacji brak. Odczekuj i licz na POP3.
        g_escalation.next_escalation_ms = nowMs + 60000; // co minutę zaglądaj
      }
    }
  }

  // --- 2) POP3 polling (ACK) ---
  if (!g_pop3_enabled) return;

  if (nowMs < g_next_pop3_ms) return;

  if (!WiFi.isConnected()) {
    schedule_pop3_in_s(BACKOFF_MIN);
    return;
  }

  // Gdy nie oczekujemy na ACK: po prostu utrzymuj normalny interwał.
  if (g_pending_epoch == 0) {
    schedule_pop3_norm();
    return;
  }

  // Sprawdź POP3 pod kątem ACK
  Email::Ack ack;
  bool ok = Email::pop3CheckForEpoch(g_pending_epoch, ack, /*deleteOnMatch=*/true);
  if (ok) {
    LOGI("POP3 ACK for epoch %lu: subject='%s'", (unsigned long)g_pending_epoch, ack.subject.c_str());
    appendToLog(String("POP3 ACK ok for epoch ") + String((unsigned long)g_pending_epoch));
    // Wyczyść stan eskalacji i oczekiwania
    g_pending_epoch = 0;
    g_escalation = EscState{};
    schedule_pop3_norm();
  } else {
    LOGW("POP3 no ACK yet for epoch %lu, retry in %lus",
         (unsigned long)g_pending_epoch, (unsigned long)g_backoff_sec);
    appendToLog(String("POP3 no ACK, retry in ") + String((unsigned long)g_backoff_sec) + "s");
    schedule_pop3_in_s(g_backoff_sec);
    grow_backoff();
  }
}

bool testSend(const String& to, const String& subj, const String& body) {
  std::vector<String> rcpts{to};
  bool ok = Email::sendSMTP(subj, body, rcpts);
  if (ok) LOGI("Test SMTP sent to %s", to.c_str());
  else    LOGE("Test SMTP failed");
  return ok;
}

// --- Settery / Gettery POP3 ---
void setPop3Enabled(bool enabled) {
  g_pop3_enabled = enabled;
  if (enabled) schedule_pop3_in_s(3);
  LOGI("EmailAlert POP3 %s", enabled ? "ENABLED" : "DISABLED");
}

void setPop3Interval(uint32_t seconds) {
  if (seconds < 30) seconds = 30;
  g_pop3_interval = seconds;
  schedule_pop3_in_s(2);
  LOGI("EmailAlert POP3 interval=%lus", (unsigned long)g_pop3_interval);
}

void setPendingEpoch(uint32_t epoch) {
  g_pending_epoch = epoch;
  schedule_pop3_in_s(5); // szybki check po wysłaniu
  LOGI("EmailAlert pending epoch set to %lu", (unsigned long)epoch);
}

bool     getPop3Enabled()  { return g_pop3_enabled; }
uint32_t getPop3Interval() { return g_pop3_interval; }
uint32_t getPendingEpoch() { return g_pending_epoch; }

// --- Wejścia z alarm.cpp ---

void notifyAlarmStart(const String& channelLabel, double value, uint32_t epoch) {
  // Jeśli już jest aktywna eskalacja innego zgłoszenia — nadpisujemy (prosta implementacja 1-alarm-NA-RAZ)
  if (g_escalation.active && g_escalation.epoch != epoch) {
    LOGW("EmailAlert: overriding previous escalation (epoch=%lu) with new (epoch=%lu).",
         (unsigned long)g_escalation.epoch, (unsigned long)epoch);
  }

  g_escalation = EscState{};
  g_escalation.active = true;
  g_escalation.channel = channelLabel;
  g_escalation.value = value;
  g_escalation.epoch = epoch;
  g_escalation.ts_at_start = Measure::pomTimeStamp();

  // Zbuduj wiadomość
  String subj, body;
  buildAlarmStartMail(channelLabel, value, epoch, subj, body, g_escalation.ts_at_start);

  // Wyślij do Group1 natychmiast
  bool sent = sendToGroup(1, subj, body);
  g_escalation.stage = sent ? 1 : 0;

  // Ustaw oczekiwanie na ACK i zegar eskalacji
  setPendingEpoch(epoch);

  const uint32_t waitSec = waitAfterStage(sent ? 1 : 0);
  g_escalation.next_escalation_ms = millis() + waitSec * 1000UL;

  LOGI("EmailAlert: START sent to G1=%s, next escalation in %us.",
       sent ? "YES" : "NO (empty)", (unsigned)waitSec);
}

void notifyAlarmCleared(const String& channelLabel, uint32_t epoch) {
  // wyślij info o zakończeniu do Group1 (minimalny spam)
  String ts = Measure::pomTimeStamp();
  String subj, body;
  buildAlarmClearedMail(channelLabel, epoch, subj, body, ts);
  (void)sendToGroup(1, subj, body);

  // zatrzymaj eskalację i oczekiwanie
  g_pending_epoch = 0;
  g_escalation = EscState{};
  LOGI("EmailAlert: CLEARED for epoch %lu.", (unsigned long)epoch);
}

} // namespace EmailAlert
