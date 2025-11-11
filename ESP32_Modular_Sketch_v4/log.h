#pragma once
#include <Arduino.h>

namespace Log {
  void begin();
  void setMaxSize(size_t bytes);
  void setMaxFiles(int n);
  void printf(const char* level, const char* fmt, ...);
  void heartbeat(); //I możesz wołać Log::heartbeat(); w loop()
}

#define LOGI(fmt, ...) Log::printf("INFO", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) Log::printf("WARN", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) Log::printf("ERR ", fmt, ##__VA_ARGS__)
