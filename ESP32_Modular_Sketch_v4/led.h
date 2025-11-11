#pragma once
#include <Arduino.h>
static const int LEDrgb_PIN = 33;
static const int LED_PIN = 34;
namespace Led {
  void begin();
  void loop();
  void setFastBlink(bool on);
  void setSlowBlink(bool on);
  void setSolid(bool on);
}
