#include "email_alert.h"
#include "email_client.h"
#include "email_config.h"
#include "alarm.h"
#include "data_files.h"
#include "measurement.h"   // <-- potrzebne do Measure::pomTimeStamp()
#include "log.h"
#include <LittleFS.h>
#include <vector>

namespace {

EmailCfg::Settings cfg;

struct Event {
  bool  active = false;
  uint32_t epoch = 0;
  String yyy;           // A001/B001...
  double value = 0.0;

  bool acked = false;
  bool recovered = false;

  // etap eskalacji: 0=nie wysłano, 1=gr1, 2=gr2, 3=gr3
  int phase = 0;
  time_t tSent = 0;           // kiedy wysłano w danym etapie
  std::vector<String> notified; // komu już wysłano
};

Event ev;

// >>> ZAMIANA: korzystamy z Measure::pomTimeStamp()
String nowStamp() { return Measure::pomTimeStamp(); }

void appendAlarmLog(const String& msg) {
  String line = nowStamp(); line += " ; "; line += msg; line += "\r\n";
  File f = LittleFS.open(Alarm::alarmBasePath(), "a"); if (!f) f = LittleFS.open(Alarm::alarmBasePath(), "w");
  if (f) { f.print(line); f.close(); }
}

void splitCSV(const String& csv, std::vector<String>& out, size_t maxItems) {
  out.clear();
  String s = csv; s.replace(';', ',');
  int start = 0;
  while (start < (int)s.length() && out.size() < maxItems) {
    int comma = s.indexOf(',', start);
    String token = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    token.trim();
    if (token.length() > 0) out.push_back(token);
    if (comma < 0) break;
    start = comma + 1;
  }
}

std::vector<String> groupVec(int g){
  std::vector<String> v;
  if (g==1) splitCSV(cfg.group1_csv, v, 10);
  if (g==2) splitCSV(cfg.group2_csv, v, 5);
  if (g==3) splitCSV(cfg.group3_csv, v, 2);
  return v;
}

uint32_t waitSecForPhase(int p){
  if (p==1) return cfg.wait_g1;
  if (p==2) return cfg.wait_g2;
  if (p==3) return cfg.wait_g3;
  return 0;
}

String makeAlarmSubject(){ // „Alarm przekroczenie wartości xxx dla wejścia yyy_<EPOCH>.”
  char ep[16]; snprintf(ep,sizeof(ep),"%lu",(unsigned long)ev.epoch);
  return String("Alarm przekroczenie wartości ") + String(ev.value,6) + " dla wejścia " + ev.yyy + "_" + ep + ".";
}

String makeRecoverSubject(){ // „Alarm został obsłużony lub sygnał wrócił do normy _<EPOCH>.”
  char ep[16]; snprintf(ep,sizeof(ep),"%lu",(unsigned long)ev.epoch);
  return String("Alarm został obsłużony lub sygnał wrócił do normy _") + ep + ".";
}

String composeBody(const String& custom, bool includeFooter=true){
  String b;
  b += custom; if (!custom.endsWith("\r\n")) b += "\r\n";
  b += "Wejście: " + ev.yyy + "\r\n";
  b += "Wartość: " + String(ev.value,6) + "\r\n";
  b += "Epoch: " + String((unsigned long)ev.epoch) + "\r\n";
  if (includeFooter) {
    b += "\r\nAby potwierdzić otrzymanie, odpowiedz mail z tematem: ";
    b += String((unsigned long)ev.epoch) + "_OK\r\n";
    b += "lub wyślij mail o temacie: " + String((unsigned long)ev.epoch) + " i treści: OK.\r\n";
  }
  return b;
}

void sendToGroup(int g, const String& subj, const String& body) {
  auto to = groupVec(g);
  if (to.empty()) return;
  if (Email::sendSMTP(subj, body, to)) {
    for (auto& t : to) {
      bool exists=false; for (auto& n: ev.notified) if (n==t) { exists=true; break; }
      if (!exists) ev.notified.push_back(t);
    }
    appendAlarmLog(String("Wysłano e-mail (G") + g + "): " + subj);
  } else {
    appendAlarmLog(String("BŁĄD wysyłki (G") + g + "): " + subj);
  }
}

void closeWithRecoveryMail(const char* reason){
  if (!ev.notified.empty()) {
    String subj = makeRecoverSubject();
    String body = composeBody(cfg.body_recover, /*includeFooter*/false);
    Email::sendSMTP(subj, body, ev.notified);
    appendAlarmLog(String("Wysłano e-mail zakończenia do ")+String(ev.notified.size())+" odbiorców. Powód: "+reason);
  }
  ev = Event{};
}

void stepMachine(){
  if (!cfg.enabled || !ev.active) return;

  time_t now = time(nullptr);

  // 1) start -> grupa 1
  if (ev.phase == 0) {
    String subj = makeAlarmSubject();
    String body = composeBody(cfg.body_alarm);
    sendToGroup(1, subj, body);
    ev.phase = 1; ev.tSent = now;
    return;
  }

  // 2) sprawdź potwierdzenie
  Email::Ack ack;
  if (Email::pop3CheckForEpoch(ev.epoch, ack, /*delete*/true)) {
    ev.acked = true;
    appendAlarmLog(String("Otrzymano mail potwierdzający: ")+ack.subject+" ; "+ack.from);
    closeWithRecoveryMail("ACK otrzymany");
    return;
  }

  // 3) sygnał wrócił do normy
  if (ev.recovered) {
    appendAlarmLog("Sygnał wrócił do normy – zamykam sekwencję e-mail.");
    closeWithRecoveryMail("powrót do normy");
    return;
  }

  // 4) jeśli minął czas etapu -> eskaluj
  uint32_t wait = waitSecForPhase(ev.phase);
  if (wait > 0 && (uint32_t)(now - ev.tSent) >= wait) {
    appendAlarmLog(String("Brak potwierdzenia w G")+ev.phase+", eskalacja.");
    if (ev.phase == 1) {
      String subj = makeAlarmSubject();
      String body = composeBody(cfg.body_alarm);
      sendToGroup(2, subj, body);
      ev.phase = 2; ev.tSent = now;
    } else if (ev.phase == 2) {
      String subj = makeAlarmSubject();
      String body = composeBody(cfg.body_alarm);
      sendToGroup(3, subj, body);
      ev.phase = 3; ev.tSent = now;
    } else if (ev.phase == 3) {
      appendAlarmLog("Brak potwierdzenia po G3 – kończę i resetuję stan.");
      ev = Event{};
    }
  }
}

} // anon

namespace EmailAlert {

void begin() {
  EmailCfg::load(cfg);
  Email::begin();
}

void loopTick() {
  static uint32_t last = 0;
  uint32_t nowms = millis();
  if (nowms - last < 5000) return; // co ~5 s
  last = nowms;
  EmailCfg::load(cfg); // odśwież na wszelki wypadek
  stepMachine();
}

void notifyAlarmStart(const String& yyy, double value, uint32_t epoch) {
  EmailCfg::load(cfg);
  if (!cfg.enabled) return;
  if (ev.active) return;

  ev = Event{};
  ev.active = true;
  ev.epoch = epoch;
  ev.yyy = yyy;
  ev.value = value;
  ev.phase = 0;
  ev.tSent = 0;
  ev.notified.clear();

  appendAlarmLog(String("Start sekwencji e-mail: ")+yyy+" epoch="+String((unsigned long)epoch));
}

void notifyAlarmCleared(const String& yyy, uint32_t epoch) {
  if (ev.active && ev.epoch == epoch) {
    ev.recovered = true;
  } else {
    appendAlarmLog(String("Powrót do normy: ")+yyy+" (brak aktywnego eventu)");
  }
}

bool testSend(const String& to, const String& subject, const String& body){
  std::vector<String> rcpts = { to };
  return Email::sendSMTP(subject, body, rcpts);
}

} // namespace EmailAlert
