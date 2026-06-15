#pragma once
#include <stdint.h>

// Atualização de firmware por WiFi (ArduinoOTA), pra não depender do cabo USB.
//
// SOB DEMANDA: o WiFi NÃO fica ligado no uso normal — Bluedroid + WiFi STA
// coexistem mal no ESP32-S3 e a recepção BLE trava com o tempo. O WiFi só
// sobe no MODO OTA (otaStart, via long-press na tela Info) e cai sozinho após
// ~5 min sem gravação. Fora disso o rádio é 100% do BLE.
//
// Credenciais ficam em secrets.h (fora do git). Sem SSID, o OTA não fica
// disponível e o resto do firmware roda normal.
//
// Pra gravar sem fio (com o MODO OTA ligado na placa):
//   arduino-cli upload -p <IP-da-placa> --upload-field password="" \
//     --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED

void otaInit(const char* hostname);  // registra handlers (NÃO liga WiFi)
void otaStart();                     // entra no modo OTA (sobe WiFi STA)
void otaStop();                      // sai do modo OTA (desliga WiFi)
void otaToggle();                    // alterna o modo OTA
void otaHandle();                    // chamar todo loop (no-op se inativo)
bool otaAvailable();                 // há credenciais configuradas?
bool otaActive();                    // modo OTA ligado?
const char* otaStatus();             // texto curto p/ a tela (IP / "wifi..." / "off")
int  otaProgress();                  // -1 fora de update; 0..100 durante a gravação
