#include "ota.h"
#include <Arduino.h>

// Credenciais opcionais. Se secrets.h não existir/estiver vazio, o OTA
// fica desligado (WIFI_SSID vazio) e nada de WiFi sobe.
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

static bool enabled = false;       // há SSID configurado?
static bool otaUp   = false;       // ArduinoOTA.begin() já chamado?
static int  prog    = -1;          // progresso da gravação (-1 = ocioso)
static char statusBuf[24] = "off";

void otaInit(const char* hostname) {
  enabled = (sizeof(WIFI_SSID) > 1);   // string não-vazia em compile-time
  if (!enabled) { snprintf(statusBuf, sizeof(statusBuf), "off"); return; }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);                  // melhor coexistência com o BLE
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  ArduinoOTA.setHostname(hostname);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart  ([]()                              { prog = 0; });
  ArduinoOTA.onEnd    ([]()                              { prog = 100; });
  ArduinoOTA.onProgress([](unsigned int c, unsigned int t){ prog = t ? (int)((uint64_t)c * 100 / t) : 0; });
  ArduinoOTA.onError  ([](ota_error_t)                   { prog = -1; });

  snprintf(statusBuf, sizeof(statusBuf), "wifi...");
}

void otaHandle() {
  if (!enabled) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!otaUp) { ArduinoOTA.begin(); otaUp = true; }   // mDNS exige WiFi up
    ArduinoOTA.handle();
    snprintf(statusBuf, sizeof(statusBuf), "%s", WiFi.localIP().toString().c_str());
  } else {
    otaUp = false;                                       // re-arma no reconnect
    snprintf(statusBuf, sizeof(statusBuf), "wifi...");
  }
}

bool otaWifiUp()        { return enabled && WiFi.status() == WL_CONNECTED; }
const char* otaStatus() { return statusBuf; }
int  otaProgress()      { return prog; }
