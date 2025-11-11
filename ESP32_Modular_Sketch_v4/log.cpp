#include "log.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>

static size_t s_max_bytes = 50 * 1024;  // 50 KiB na plik
static int    s_max_files = 5;          // log0 + log1..log4 (łącznie 5 plików)
static const char* LOG0_PATH = "/log0.txt";

// Usuń najstarszy i przesuń logi w górę tak, by po rotacji:
///  log4 (ostatni) znika,
///  log3 -> log4, log2 -> log3, log1 -> log2, log0 -> log1,
///  a nowy log0 będzie tworzony „od zera”.
static void rotate_if_needed() {
  File f = LittleFS.open(LOG0_PATH, "r");
  if (!f) return;
  size_t sz = f.size();
  f.close();
  if (sz < s_max_bytes) return;

  // 1) Usuń najstarszy (log{s_max_files-1}.txt), jeśli istnieje
  if (s_max_files < 2) s_max_files = 2; // sanity
  String oldest = "/log" + String(s_max_files - 1) + ".txt";
  if (LittleFS.exists(oldest)) LittleFS.remove(oldest);

  // 2) Przesuń: log{i-1} -> log{i} dla i = s_max_files-1 .. 2
  for (int i = s_max_files - 1; i >= 2; --i) {
    String src = "/log" + String(i - 1) + ".txt";
    String dst = "/log" + String(i)     + ".txt";
    if (LittleFS.exists(dst)) LittleFS.remove(dst);
    if (LittleFS.exists(src)) LittleFS.rename(src, dst);
  }

  // 3) log0 -> log1 (specjalnie, bo log0 ma inną ścieżkę)
  if (LittleFS.exists("/log1.txt")) LittleFS.remove("/log1.txt");
  if (LittleFS.exists(LOG0_PATH))    LittleFS.rename(LOG0_PATH, "/log1.txt");
}

void Log::begin() {
  // Upewnij się, że LittleFS.begin() został wywołany PRZED Log::begin()
  if (!LittleFS.exists(LOG0_PATH)) {
    File f = LittleFS.open(LOG0_PATH, "w");
    if (f) f.close();
  }
}

void Log::setMaxSize(size_t bytes) { s_max_bytes = bytes; }
void Log::setMaxFiles(int n)       { s_max_files = (n < 2 ? 2 : n); }

// (opcjonalny) bardzo prosty heartbeat co ~10 s; wywołuj w loop() jeśli chcesz
void Log::heartbeat() {
  static unsigned long tLast = 0;
  if (millis() - tLast >= 10000UL) {
    tLast = millis();
    Log::printf("INFO", "heartbeat %lu s", tLast / 1000UL);
  }
}

void Log::printf(const char* level, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // --- Serial
  Serial.print("[");
  Serial.print(level);
  Serial.print("] ");
  Serial.println(buf);

  // --- Plik (open-append-close + rotacja)
  rotate_if_needed();
  File f = LittleFS.open(LOG0_PATH, "a");
  if (f) {
    // dopisz millis, poziom i treść; CRLF dla czytelności w Windows
    char line[320];
    unsigned long ms = millis();
    snprintf(line, sizeof(line), "%lu [%s] %s\r\n", ms, level, buf);
    f.print(line);
    f.close();
  }
}
