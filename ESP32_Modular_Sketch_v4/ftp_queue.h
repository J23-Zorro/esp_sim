#pragma once
#include <Arduino.h>

// Prosta trwała kolejka z backoffem do wysyłek FTP.
// Użycie:
//   FTPQ::begin();
//   FTPQ::enqueue("/logs/dump1.txt", "inbox");     // zapisze do /inbox na FTP (CWD inbox)
//   FTPQ::enqueue("/logs/dump2.txt", "");          // bieżący katalog FTP
//   W loop(): FTPQ::tick();  // przetwarza 1 zadanie, gdy pora

namespace FTPQ {

struct Stats {
  size_t size;         // ile zadań w kolejce
  uint32_t nextDueMs;  // za ile ms najbliższe zadanie jest „due” (0 gdy już due lub brak)
};

bool begin();  // ładuje kolejkę z LittleFS, tworzy plik jeśli brak
bool enqueue(const char* local_path, const char* remote_dir); // dodaje zadanie
bool clear();  // kasuje całą kolejkę
size_t size(); // liczba zadań
Stats stats();
// --- JSON snapshot kolejki (do WebUI) ---
String toJson();

// Przetwarza co najwyżej 1 zadanie: jeśli warunki spełnione (czas, sieć itp.).
// Wywołuj często w loop(). Zwraca true, jeśli coś zrobiła (sukces lub próba).
bool tick();

// (opcjonalnie) ustawienia
void setDeleteLocalOnSuccess(bool enable); // domyślnie true
void setMaxRetries(uint8_t maxRetries);    // domyślnie 5

} // namespace FTPQ
