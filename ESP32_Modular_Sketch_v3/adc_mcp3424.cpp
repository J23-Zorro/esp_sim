#include <Wire.h>
#include <MCP3424.h>
#include <algorithm>
#include "adc_values.h"
#include "log.h"
#include <esp_task_wdt.h>

#define I2C_SDA 3//33//12
#define I2C_SCL 2//32//14

MCP3424 adc1(0x68);
MCP3424 adc2(0x6E);

#define NUM_SAMPLES 10

struct ChannelData {
  double   value;
  _ConfReg config;
};

static double    samples[8][NUM_SAMPLES] = {{0}};
static int       sampleIndex[8] = {0};
static int       warmupLeft = NUM_SAMPLES-1;
static bool      bufferFilled = false;
static ChannelData data8[8];

void i2c_recover(int scl, int sda){
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  pinMode(sda, INPUT_PULLUP);
  for (int i=0;i<9;i++){ digitalWrite(scl, LOW); delayMicroseconds(5); digitalWrite(scl, HIGH); delayMicroseconds(5); }
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW); delayMicroseconds(5);
  digitalWrite(scl, HIGH); delayMicroseconds(5);
  digitalWrite(sda, HIGH); delayMicroseconds(5);
}
// użycie w setup:
// i2c_recover(I2C_SCL, I2C_SDA);
// Wire.begin(I2C_SDA, I2C_SCL, 100000);

void startMCP3424() {
  i2c_recover(I2C_SDA, I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  //Wire.setClock(100000);   // 100 kHz
  //Wire.setTimeOut(150);      // ważne: skraca ewentualne zwisy
  asm volatile ("nop"); asm volatile ("nop");
  adc1.generalCall(GC_RESET);
  asm volatile ("nop"); asm volatile ("nop");
  adc2.generalCall(GC_RESET);
  bufferFilled = false;
  warmupLeft   = NUM_SAMPLES-1;
  LOGI("MCP3424 init OK (0x68,0x6E) SDA=%d SCL=%d", I2C_SDA, I2C_SCL);
}





static double trimmedMeanMedianFilter(double newSample, double sampleBuffer[], int& index) {
  // cykliczny bufor
  sampleBuffer[index] = newSample;
  index = (index + 1) % NUM_SAMPLES;
  if (index == 0 && warmupLeft>0) {
    if (--warmupLeft == 0) bufferFilled = true;
  }

  double sorted[NUM_SAMPLES];
  memcpy(sorted, sampleBuffer, sizeof(sorted));
  std::sort(sorted, sorted + NUM_SAMPLES);

  // mediana (opcjonalnie do diagnostyki)
  // double median = sorted[NUM_SAMPLES/2];

  // odrzuć 2 skrajne z dołu i góry
  double sum = 0; int cnt = 0;
  for (int i=2; i<NUM_SAMPLES-2; ++i) { sum += sorted[i]; ++cnt; }
  return cnt ? (sum / cnt) : newSample;
}



static ConvStatus readWithTimeout(MCP3424& adc, Channel ch, double& out, uint32_t budgetMs) {
  unsigned long t0 = millis();
  ConvStatus st;
  do {
    st = adc.read(ch, out);               // biblioteka zwraca R_STATUS_OK / R_STATUS_NOTRDY / ...
    if (st == R_STATUS_OK) return st;
    delay(5);
    esp_task_wdt_reset();
    yield();
  } while ((millis() - t0) < budgetMs);
  return st;                              // ostatni status (zwykle NOTRDY timeout)
}

static void read_adc_channels(MCP3424& adc, ChannelData data[], int start_idx) {
  for (int i = (int)CH1; i <= (int)CH4; ++i) {
    const int idx = start_idx + (i - (int)CH1);
    _ConfReg& c = adc.creg[(Channel)i];

    // 1) Szybki strzał 12-bit (krótki budżet)
    c.bits = { GAINx1, R12B, ONE_SHOT, (Channel)i, 1 };
    double raw = 0;
    ConvStatus st = readWithTimeout(adc, c.bits.ch, raw, 80); // ~80ms budżetu zwykle wystarcza

    if (st == R_STATUS_OK) {
      double filtered = trimmedMeanMedianFilter(raw, samples[idx], sampleIndex[idx]);
      data[idx].value = filtered;
      data[idx].config = c;

      // 2) Auto-gain na podstawie 12-bit
      c.bits.pga = adc.findGain(filtered);

      // 3) 18-bit precyzyjny – polluj gotowość bez wieszania pętli
      c.bits.res = R18B;                  // 18 bit ≈ 267 ms
      st = readWithTimeout(adc, c.bits.ch, raw, 700); // budżet 400 ms

      if (st == R_STATUS_OK) {
        filtered = trimmedMeanMedianFilter(raw, samples[idx], sampleIndex[idx]);
        data[idx].value = filtered;
        data[idx].config = c;
        Serial.println("--18bits OK--"); // opcjonalnie
      } else {
        // Nie gotowe w czasie – zostaw wynik 12-bit jako „fallback”.
         Serial.println("--18bits TIMEOUT--"); // opcjonalnie
      }
    } else {
      // 12-bit też nie gotowy – nie truj logu, tylko zaznacz błąd dla kanału
      data[idx].value = NAN;
      data[idx].config = c;
       Serial.println("--ADC NOT READY--"); // opcjonalnie
    }
  }
  asm volatile ("nop"); asm volatile ("nop");
}


static void set_all_valuesADC(ChannelData data[]) {
  if (!isnan(data[0].value)) weADC1 = data[0].value;
  if (!isnan(data[1].value)) weADC2 = data[1].value;
  if (!isnan(data[2].value)) weADC3 = data[2].value;
  if (!isnan(data[3].value)) weADC4 = data[3].value;
  if (!isnan(data[4].value)) weADC5 = data[4].value;
  if (!isnan(data[5].value)) weADC6 = data[5].value;
  if (!isnan(data[6].value)) weADC7 = data[6].value;
  if (!isnan(data[7].value)) weADC8 = data[7].value;
}

static void show_all_values(ChannelData data[]) {
  char v[24], buff[512] = {0};
  char temp[64];
  for (int i=0;i<8;++i) {
    if (!isnan(data[i].value)) dtostrf(data[i].value, 8, 6, v);
    else                       strcpy(v, "  ERROR  ");
    sprintf(temp, "CH%d: g:%d b:%d val:%s ",
      i+1, 1 << data[i].config.bits.pga, 12 + (data[i].config.bits.res * 2), v);
    strcat(buff, temp);
  }
  Serial.println(buff);
}

void pomiarMCP3424() {
  read_adc_channels(adc1, data8, 0);// Kanały 1-4
  Serial.println("Pomiar wartości ANALOGOWYCH");
  //read_adc_channels(adc1, data8, 0);// Kanały 1-4
  read_adc_channels(adc2, data8, 4);// Kanały 5-8

// Wyświetlamy wyniki tylko wtedy, gdy bufory są w pełni napełnione
  if (bufferFilled) {
    set_all_valuesADC(data8);
    show_all_values(data8); // opcjonalnie: zostaw do diagnostyki
  }
  
  // Placeholder sekcji binary:
  Serial.println("Pomiar wartości BINARNYCH");
}
