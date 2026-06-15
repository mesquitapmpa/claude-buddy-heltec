#include "ota.h"
#include <Arduino.h>

// Credenciais opcionais. Se secrets.h não existir/estiver vazio, o OTA
// fica indisponível (sem SSID) e nada de WiFi sobe.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif
// OTA_PASSWORD é opcional (protege a gravação sem fio).

#include <WiFi.h>
#include <ArduinoOTA.h>

// IMPORTANTE: WiFi NÃO fica ligado no uso normal. A stack BLE aqui é
// Bluedroid, que coexiste mal com WiFi STA no ESP32-S3 — deixar o WiFi ligado
// o tempo todo faz a recepção BLE travar depois de um tempo (o link continua
// "conectado" mas os writes param de chegar). Então o WiFi só sobe quando o
// usuário entra no MODO OTA (long-press na tela Info); fora disso o rádio é
// 100% do BLE. O modo OTA também se desliga sozinho após alguns minutos sem
// gravação, pra nunca degradar a função principal por esquecimento.

static bool enabled = false;          // há SSID configurado?
static bool active  = false;          // modo OTA ligado (WiFi up)?
static bool otaUp   = false;          // ArduinoOTA.begin() já chamado?
static int  prog    = -1;             // progresso da gravação (-1 = ocioso)
static uint32_t startMs = 0;          // quando o modo OTA subiu
static char statusBuf[24] = "off";
static char host[24] = "buddy";

static const uint32_t OTA_IDLE_MS = 300000;  // 5 min sem gravar → desliga

void otaInit(const char* hostname) {
  enabled = (sizeof(WIFI_SSID) > 1);  // string não-vazia em compile-time
  if (hostname && *hostname) snprintf(host, sizeof(host), "%s", hostname);
  snprintf(statusBuf, sizeof(statusBuf), enabled ? "off" : "sem wifi");

  // Handlers não dependem de WiFi — registra agora, uma vez.
  ArduinoOTA.setHostname(host);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart  ([]()                              { prog = 0; });
  ArduinoOTA.onEnd    ([]()                              { prog = 100; });
  ArduinoOTA.onProgress([](unsigned int c, unsigned int t){ prog = t ? (int)((uint64_t)c * 100 / t) : 0; });
  ArduinoOTA.onError  ([](ota_error_t)                   { prog = -1; });
}

void otaStart() {
  if (!enabled || active) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  active = true; otaUp = false; prog = -1; startMs = millis();
  snprintf(statusBuf, sizeof(statusBuf), "wifi...");
}

void otaStop() {
  if (!active) return;
  ArduinoOTA.end();
  WiFi.disconnect(true);      // true: desliga o rádio WiFi (libera p/ o BLE)
  WiFi.mode(WIFI_OFF);
  active = false; otaUp = false; prog = -1;
  snprintf(statusBuf, sizeof(statusBuf), "off");
}

void otaToggle() { active ? otaStop() : otaStart(); }

void otaHandle() {
  if (!active) return;
  // Desliga sozinho se ficou ligado sem ninguém gravar (protege o BLE).
  if (prog < 0 && (uint32_t)(millis() - startMs) > OTA_IDLE_MS) { otaStop(); return; }
  if (WiFi.status() == WL_CONNECTED) {
    if (!otaUp) { ArduinoOTA.begin(); otaUp = true; }  // mDNS exige WiFi up
    ArduinoOTA.handle();
    snprintf(statusBuf, sizeof(statusBuf), "%s", WiFi.localIP().toString().c_str());
  } else {
    otaUp = false;
    snprintf(statusBuf, sizeof(statusBuf), "wifi...");
  }
}

bool otaAvailable()     { return enabled; }
bool otaActive()        { return active; }
const char* otaStatus() { return statusBuf; }
int  otaProgress()      { return prog; }
