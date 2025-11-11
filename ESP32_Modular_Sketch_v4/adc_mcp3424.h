#pragma once
#include <Arduino.h>

void startMCP3424();   // init I2C + reset układów
void pomiarMCP3424();  // odczyt CH1..CH8 z filtrowaniem + auto-gain (ustawia weADC1..8)
