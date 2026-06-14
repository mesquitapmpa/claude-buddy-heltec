#pragma once
#include <stdint.h>

// Atualização de firmware por WiFi (ArduinoOTA), pra não depender do cabo USB.
// Coexiste com o BLE (ESP32-S3 faz WiFi+BLE). Não bloqueia o loop: a STA
// conecta em background e o ArduinoOTA só sobe quando o WiFi estiver pronto.
//
// Credenciais ficam em secrets.h (fora do git). Sem secrets.h válido, o OTA
// fica inerte e o resto do firmware roda normal.
//
// Pra gravar sem fio (com o firmware OTA já rodando):
//   arduino-cli upload -p <IP-da-placa> \
//     --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED

void otaInit(const char* hostname);  // configura WiFi STA + handlers
void otaHandle();                    // chamar todo loop (não bloqueia)
bool otaWifiUp();                    // STA conectada?
const char* otaStatus();             // texto curto p/ a tela (IP / "wifi..." / "off")
int  otaProgress();                  // -1 fora de update; 0..100 durante a gravação
