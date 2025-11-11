#include "email_client.h"
#include "email_config.h"
#include <WiFiClientSecure.h>
#include <base64.h>
#include "log.h"
#include <time.h>

namespace {
  EmailCfg::Settings g;  // aktualne ustawienia e-mail (ładowane z FS)

  // Wczytaj jedną linię (do \n), z timeoutem
  bool readLine(WiFiClientSecure& c, String& out, uint32_t ms = 8000) {
    out = "";
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      while (c.available()) {
        char ch = c.read();
        if (ch == '\r') continue;
        if (ch == '\n') return true;
        out += ch;
      }
      delay(1);
    }
    // timeout – zwracamy to, co się udało zebrać
    return out.length() > 0;
  }

  inline bool expectCode(const String& line, const char* code3) {
    return line.startsWith(code3);
  }

  // SMTP z diagnostyką: loguje oczekiwany kod i faktyczną odpowiedź
  static bool smtp_cmd_dbg(WiFiClientSecure& c,
                           const String& cmd,
                           const char* expect,
                           const char* label,
                           uint32_t tmo = 12000) {
    if (cmd.length()) {
      c.print(cmd);
      c.print("\r\n");
    }
    String line;
    uint32_t t0 = millis();
    do {
      if (!readLine(c, line, tmo)) break;
      // Odpowiedzi SMTP mają postać: "250-..." (linia wieloczęściowa) lub "250 ..." (ostatnia linia)
      if (line.length() >= 4 && (line[3] == ' ' || line[3] == '-')) {
        if (line[3] == ' ') {
          bool ok = expectCode(line, expect);
          if (!ok) LOGE("SMTP expect %s after %s, got: %s", expect, label, line.c_str());
          return ok;
        }
      }
    } while (millis() - t0 < tmo);
    LOGE("SMTP no final response after %s", label);
    return false;
  }

  // POP3: wysyła komendę i zwraca pierwszą linię odpowiedzi
  static bool pop3_cmd(WiFiClientSecure& c,
                       const String& cmd,
                       bool* pos = nullptr,
                       String* lineOut = nullptr,
                       uint32_t tmo = 12000) {
    if (cmd.length()) { c.print(cmd); c.print("\r\n"); }
    String line;
    if (!readLine(c, line, tmo)) return false;
    if (lineOut) *lineOut = line;
    bool ok = line.startsWith("+OK");
    if (pos) *pos = ok;
    return true;
  }

  static String rfc2822DateNow() {
    time_t t = time(nullptr);
    struct tm tmv; localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", &tmv);
    return String(buf);
  }
} // namespace

namespace Email {

void begin() {
  EmailCfg::load(g);
  // Jeśli nadawca pusty – użyj loginu
  if (g.sender.length() == 0) g.sender = g.user;
}

// SMTP przez SSL (465)
// Zmiany:
//  - EHLO "esp32" (zamiast *.local)
//  - MAIL FROM używa g.user (login), bo WP tego wymaga częściej niż s.sender
//  - Lepsze logowanie błędów (555, 535 itd.)
//  - Bez setBufferSizes (nie istnieje w core 3.3.x)
bool sendSMTP(const String& subject, const String& body, const std::vector<String>& rcpts) {
  if (rcpts.empty()) return true; // nie ma do kogo – traktuj jako OK

  (void)EmailCfg::load(g); // odśwież z FS

  WiFiClientSecure cli;
  cli.setInsecure(); // jeśli masz NTP + root CA, podmień na setCACert(...)
  if (!cli.connect(g.smtp_host.c_str(), g.smtp_port)) {
    LOGE("SMTP connect fail");
    return false;
  }

  // Banner 220
  String line;
  if (!readLine(cli, line) || !line.startsWith("220")) {
    LOGE("SMTP banner fail: %s", line.c_str());
    return false;
  }

  // Bezpieczny EHLO/HELO (unikamy *.local)
  String heloId = "esp32";
  if (!smtp_cmd_dbg(cli, String("EHLO ") + heloId, "250", "EHLO")) {
    LOGW("EHLO failed, trying HELO…");
    if (!smtp_cmd_dbg(cli, String("HELO ") + heloId, "250", "HELO")) {
      LOGE("HELO failed");
      return false;
    }
  }

  // AUTH LOGIN
  if (!smtp_cmd_dbg(cli, "AUTH LOGIN", "334", "AUTH LOGIN")) return false;
  if (!smtp_cmd_dbg(cli, base64::encode(g.user), "334", "AUTH USER")) return false;
  if (!smtp_cmd_dbg(cli, base64::encode(g.pass), "235", "AUTH PASS")) {
    // Opcjonalny fallback do AUTH PLAIN
    LOGW("AUTH LOGIN failed, trying AUTH PLAIN…");
    String auth = String('\0') + g.user + String('\0') + g.pass;
    String b64  = base64::encode(auth);
    if (!smtp_cmd_dbg(cli, String("AUTH PLAIN ") + b64, "235", "AUTH PLAIN")) return false;
  }

  // MAIL FROM – WP zwykle wymaga, by MAIL FROM == login (g.user)
  if (!smtp_cmd_dbg(cli, String("MAIL FROM:<") + g.user + ">", "250", "MAIL FROM")) return false;

  // RCPT TO – każdy odbiorca osobno
  for (auto& to : rcpts) {
    if (!smtp_cmd_dbg(cli, String("RCPT TO:<") + to + ">", "250", ("RCPT TO " + to).c_str())) {
      LOGW("RCPT rejected by server: %s", to.c_str());
      // nie przerywamy – inni odbiorcy mogą przejść
    }
  }

  if (!smtp_cmd_dbg(cli, "DATA", "354", "DATA")) return false;

  // HEADERS
  String fromHdr = (g.sender.length() ? g.sender : g.user);
  cli.printf("From: %s\r\n", fromHdr.c_str());
  cli.printf("To: ");
  for (size_t i = 0; i < rcpts.size(); ++i) {
    cli.printf("<%s>%s", rcpts[i].c_str(), (i + 1 < rcpts.size()) ? ", " : "");
  }
  cli.printf("\r\n");
  cli.printf("Subject: %s\r\n", subject.c_str());
  cli.printf("Date: %s\r\n", rfc2822DateNow().c_str());
  cli.printf("MIME-Version: 1.0\r\n");
  cli.printf("Content-Type: text/plain; charset=UTF-8\r\n");
  cli.printf("Content-Transfer-Encoding: 8bit\r\n");
  cli.printf("\r\n");

  // BODY
  cli.print(body);
  if (!body.endsWith("\r\n")) cli.print("\r\n");

  // Koniec DATA
  cli.print(".\r\n");
  if (!smtp_cmd_dbg(cli, "", "250", "DATA end (.)")) return false;

  // QUIT
  smtp_cmd_dbg(cli, "QUIT", "221", "QUIT");
  cli.stop();
  return true;
}

// POP3 przez SSL (995)
// Szuka potwierdzenia EPOCH: temat "<EPOCH>_OK" lub "<EPOCH>" + treść zawiera "OK."
bool pop3CheckForEpoch(uint32_t epoch, Ack& out, bool deleteOnMatch) {
  (void)EmailCfg::load(g);

  WiFiClientSecure cli;
  cli.setInsecure();
  if (!cli.connect(g.pop3_host.c_str(), g.pop3_port)) {
    LOGE("POP3 connect fail");
    return false;
  }

  String line;
  bool pos;

  // banner
  if (!pop3_cmd(cli, "", &pos, &line) || !pos) {
    LOGE("POP3 banner fail: %s", line.c_str());
    cli.stop();
    return false;
  }
  // USER/PASS
  if (!pop3_cmd(cli, String("USER ") + g.user, &pos, &line) || !pos) {
    LOGE("POP3 USER fail");
    cli.stop();
    return false;
  }
  if (!pop3_cmd(cli, String("PASS ") + g.pass, &pos, &line) || !pos) {
    LOGE("POP3 PASS fail");
    cli.stop();
    return false;
  }

  // STAT -> liczba wiadomości
  if (!pop3_cmd(cli, "STAT", &pos, &line) || !pos) {
    LOGE("POP3 STAT fail");
    cli.stop();
    return false;
  }
  int sp = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp + 1);
  int count = (sp > 0 && sp2 > sp) ? line.substring(sp + 1, sp2).toInt() : 0;
  if (count <= 0) {
    pop3_cmd(cli, "QUIT");
    cli.stop();
    return false;
  }

  char epbuf[16];
  snprintf(epbuf, sizeof(epbuf), "%lu", (unsigned long)epoch);
  String key1 = String(epbuf) + "_OK"; // temat dokładny
  String key2 = String(epbuf);         // temat zawiera epoch (treść musi mieć "OK.")

  // Przeglądaj od najnowszych
  for (int i = count; i >= 1; --i) {
    if (!pop3_cmd(cli, String("RETR ") + i, &pos, &line) || !pos) continue;

    bool inHeaders = true;
    bool bodyOK = false;
    String fromHdr, subjHdr;

    // Czytaj całego maila aż do pojedynczej kropki
    while (readLine(cli, line, 8000)) {
      if (line == ".") break;
      if (inHeaders) {
        if (line.length() == 0) { inHeaders = false; continue; }
        if (line.startsWith("From:")) {
          fromHdr = line.substring(5);
          fromHdr.trim();
        }
        if (line.startsWith("Subject:")) {
          subjHdr = line.substring(8);
          subjHdr.trim();
        }
      } else {
        // body – szukamy "OK."
        if (!bodyOK) {
          if (line.indexOf("OK.") >= 0 || line.indexOf("Ok.") >= 0 || line.indexOf("ok.") >= 0) {
            bodyOK = true;
          }
        }
      }
    }

    bool subjHasEpochOK = (subjHdr.indexOf(key1) >= 0);
    bool subjHasEpoch   = (subjHdr.indexOf(key2) >= 0);

    if (subjHasEpochOK || (subjHasEpoch && bodyOK)) {
      out.ok = true;
      out.subject = subjHdr;
      out.from = fromHdr;
      out.date = time(nullptr);
      if (deleteOnMatch) {
        pop3_cmd(cli, String("DELE ") + i, &pos, &line); // ignorujemy wynik
      }
      break;
    }
  }

  pop3_cmd(cli, "QUIT");
  cli.stop();
  return out.ok;
}

} // namespace Email
