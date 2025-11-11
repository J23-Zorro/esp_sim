#pragma once
#include <Arduino.h>

namespace DataFiles {

// Ustal limit jako constexpr w nagłówku (NIE używaj #define o tej samej nazwie nigdzie indziej!)
constexpr size_t FILE_SIZE_LIMIT = 100UL * 1024UL; // 100 kB

// Identyfikacja plików wg MAC (bez separatorów)
const String& macNoSep();                // "AABBCCDDEEFF" (cache wewnątrz)
String        baseName();                // "D_<MAC>"
String        pathCurrent();             // "/D_<MAC>.txt"
String        path1();                   // "/D_<MAC>_1.txt"
String        path2();                   // "/D_<MAC>_2.txt"

// Operacje plikowe
size_t fileSize(const String& path);
bool   appendLineToCurrent(const String& line);
bool   copyFile(const String& from, const String& to);

// Rotacja po UDANEJ wysyłce bieżącego pliku:
//   _1 -> _2, current -> _1, a następnie zakładamy pusty current
bool   rotateAfterSend();

// Dla zgodności z wcześniejszym kodem:
bool   rotate3();                        // alias do rotateAfterSend()

// Nazwa pliku-snapshota do uploadu: "/D_<MAC>_UP_<epoch>.txt"
String makeUploadSnapshotPath();

} // namespace DataFiles
