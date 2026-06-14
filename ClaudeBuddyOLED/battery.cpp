#include "battery.h"
#include <Arduino.h>
#include <math.h>

#define VBAT_ADC     1     // GPIO1  — leitura do divisor (ADC1_CH0)
#define VBAT_CTRL    37    // GPIO37 — habilita o divisor (HIGH = liga; medido)
#define VBAT_FACTOR  4.9f  // (390k+100k)/100k — ajuste fino contra multímetro

static float vbat  = 0.0f; // tensão suavizada (V)
static float vslow = 0.0f; // baseline lenta p/ detectar tendência
static bool  have  = false;
static bool  charging = false;

void batteryInit() {
  analogReadResolution(12);
  pinMode(VBAT_CTRL, OUTPUT);
  digitalWrite(VBAT_CTRL, LOW);  // repouso: divisor desconectado (sem dreno)
}

// Uma leitura instantânea (média de N amostras), em volts.
static float readOnce() {
  digitalWrite(VBAT_CTRL, HIGH);  // conecta o divisor (HIGH = liga nesta placa)
  delay(5);                       // estabiliza
  uint32_t acc = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(VBAT_ADC);
  digitalWrite(VBAT_CTRL, LOW);   // corta o dreno
  return (acc / (float)N) * VBAT_FACTOR / 1000.0f;
}

void batteryPoll() {
  float v = readOnce();
  if (v < 1.0f) return;                       // leitura inválida (sem bateria?)
  if (!have) { vbat = vslow = v; have = true; return; }
  vbat  += (v - vbat) * 0.25f;                // suavização rápida
  vslow += (vbat - vslow) * 0.05f;            // baseline lenta (tendência)
  // Carregando = tensão subindo de forma consistente, ou perto do cheio (USB).
  // Hysterese p/ não piscar: liga subindo/cheio, desliga caindo/baixo.
  float trend = vbat - vslow;
  if (trend > 0.015f || vbat >= 4.18f)       charging = true;
  else if (trend < -0.005f || vbat < 4.10f)  charging = false;
}

bool  batteryValid() { return have; }
float batteryVolts() { return vbat; }
bool  batteryCharging() { return have && charging; }

// Curva de descarga LiPo (1 célula) — pares {tensão, %} em ordem decrescente.
int batteryPercent() {
  if (!have) return -1;
  static const float LUT[][2] = {
    {4.20f,100},{4.15f,95},{4.11f,90},{4.08f,85},{4.02f,80},{3.98f,75},
    {3.95f,70},{3.91f,65},{3.87f,60},{3.85f,55},{3.84f,50},{3.82f,45},
    {3.80f,40},{3.79f,35},{3.77f,30},{3.75f,25},{3.73f,20},{3.71f,15},
    {3.69f,10},{3.61f,5},{3.30f,0}
  };
  const int n = sizeof(LUT) / sizeof(LUT[0]);
  float v = vbat;
  if (v >= LUT[0][0])    return 100;
  if (v <= LUT[n-1][0])  return 0;
  for (int i = 0; i < n - 1; i++) {
    if (v <= LUT[i][0] && v > LUT[i+1][0]) {
      float f = (v - LUT[i+1][0]) / (LUT[i][0] - LUT[i+1][0]);
      return (int)lroundf(LUT[i+1][1] + f * (LUT[i][1] - LUT[i+1][1]));
    }
  }
  return 0;
}

bool batteryLow() {
  if (!have || batteryCharging()) return false;
  int p = batteryPercent();
  return p >= 0 && p <= 12;
}
