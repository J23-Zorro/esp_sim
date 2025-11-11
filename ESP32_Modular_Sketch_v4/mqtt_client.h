#pragma once
#include <Arduino.h>
namespace Mqtt {
  void begin();
  void loop();
  bool publish(const char* topic, const char* payload);
}
