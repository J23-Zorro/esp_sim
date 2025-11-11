#include "led.h"
static bool fast=false, slow=false, solid=false;
void Led::begin(){ pinMode(LED_PIN, OUTPUT); }
void Led::setFastBlink(bool on){ fast=on; if(on){ slow=false; solid=false; } }
void Led::setSlowBlink(bool on){ slow=on; if(on){ fast=false; solid=false; } }
void Led::setSolid(bool on){ solid=on; if(on){ fast=false; slow=false; digitalWrite(LED_PIN, HIGH);} }
void Led::loop(){
  static unsigned long t=0;
  if (solid){ digitalWrite(LED_PIN, HIGH); return; }
  uint32_t iv = fast? 150 : (slow? 600 : 1000);
  if (millis()-t>iv){ t=millis(); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
}

