#include "adc_values.h"

double weADC1=0, weADC2=0, weADC3=0, weADC4=0, weADC5=0, weADC6=0, weADC7=0, weADC8=0;
double weADC1licz=0, weADC2licz=0, weADC3licz=0, weADC4licz=0, weADC5licz=0, weADC6licz=0, weADC7licz=0, weADC8licz=0;
uint8_t mamADC = 0;

AdcCal adcCal = {{
  1,1,1,1,1,1,1,1
}, {
  0,0,0,0,0,0,0,0
}};

void pomiarADClicz() {
  // zabezpieczenie na wypadek A=0
  for (int i=0;i<8;++i) if (adcCal.A[i]==0) adcCal.A[i]=1;
  weADC1licz = adcCal.A[0]*weADC1 + adcCal.B[0];
  weADC2licz = adcCal.A[1]*weADC2 + adcCal.B[1];
  weADC3licz = adcCal.A[2]*weADC3 + adcCal.B[2];
  weADC4licz = adcCal.A[3]*weADC4 + adcCal.B[3];
  weADC5licz = adcCal.A[4]*weADC5 + adcCal.B[4];
  weADC6licz = adcCal.A[5]*weADC6 + adcCal.B[5];
  weADC7licz = adcCal.A[6]*weADC7 + adcCal.B[6];
  weADC8licz = adcCal.A[7]*weADC8 + adcCal.B[7];
  mamADC = 1;
}
