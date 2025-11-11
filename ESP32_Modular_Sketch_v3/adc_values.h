#pragma once
#include <Arduino.h>

static const int NUM_ADC_CHANNELS = 8;

// Surowe odczyty
extern double weADC1, weADC2, weADC3, weADC4, weADC5, weADC6, weADC7, weADC8;
// Przeliczone (A*x + B)
extern double weADC1licz, weADC2licz, weADC3licz, weADC4licz, weADC5licz, weADC6licz, weADC7licz, weADC8licz;

extern uint8_t mamADC; // 0/1 – sygnalizacja, że są świeże dane

struct AdcCal {
  float A[8];  // domyślnie 1
  float B[8];  // domyślnie 0
};
extern AdcCal adcCal;

// Przeliczenie: weADC?licz = A*weADC? + B
void pomiarADClicz();
