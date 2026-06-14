#pragma once
#include <stdint.h>

// Leitura de bateria da Heltec WiFi Kit 32 V3.
//   VBAT --[390k]-- GPIO1(ADC) --[100k]-- GND, habilitado por GPIO37 (HIGH).
// Fator do divisor = (390+100)/100 = 4.9. GPIO37 fica em LOW em repouso
// para não drenar a bateria pelo divisor (HIGH=liga, medido na placa).

void  batteryInit();      // configura os pinos do ADC
void  batteryPoll();      // amostra + suaviza (chamar a cada ~5 s)
bool  batteryValid();     // já há uma leitura boa?
float batteryVolts();     // tensão suavizada (V)
int   batteryPercent();   // 0..100 pela curva de descarga LiPo
bool  batteryLow();       // <= 12% (aviso)
bool  batteryCharging();  // heurística: tensão alta = USB/carregando
