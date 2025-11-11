#pragma once
#include <Arduino.h>
#include <vector>

namespace Email {

// wynik potwierdzenia
struct Ack {
  bool ok = false;
  String from;
  time_t date = 0;
  String subject;
};

// Ustaw z configu (hosty/porty/user/pass/sender)
void begin();

// Wysyła do wielu odbiorców (RCPT TO)
bool sendSMTP(const String& subject, const String& body,
              const std::vector<String>& rcpts);

// Przeskanuj POP3: jeśli znajdziesz maila z tematem "<EPOCH>_OK"
// lub "<EPOCH>" i w treści "OK.", to ustaw Ack i (opcjonalnie) kasuj.
bool pop3CheckForEpoch(uint32_t epoch, Ack& out, bool deleteOnMatch = true);

} // namespace Email
