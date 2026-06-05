#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "buddy.h"
#include "stats.h"

// Substitui o xfer.h do projeto original: trata os comandos do desktop
// (name/owner/species/unpair/status). O push de pastas/GIF (char_begin,
// file, chunk...) nao faz sentido num OLED mono 128x64 — esses comandos
// sao consumidos sem ack, e o desktop reporta a falha ao usuario apos o
// timeout (comportamento previsto no REFERENCE.md para dispositivos que
// nao aceitam arquivos).

static void _ackCmd(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n",
                     what, ok ? "true" : "false", (unsigned long)n);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

// Chamado pelo data.h quando o JSON recebido tem chave "cmd". Retorna true
// se foi tratado aqui (o chamador pula o parse de estado).
inline bool cmdHandle(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _ackCmd("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _ackCmd("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    // 0xFF no original significa "use o GIF instalado" — sem GIF aqui,
    // so indices validos de species ASCII sao aplicados.
    uint8_t idx = doc["idx"] | 0xFF;
    if (idx < buddySpeciesCount()) {
      buddySetSpeciesIdx(idx);
      speciesIdxSave(idx);
    }
    _ackCmd("species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _ackCmd("unpair", true);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Sem AXP/bateria na Heltec V3 alimentada por USB — o protocolo
    // permite omitir campos que nao existem.
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"sys\":{\"up\":%lu,\"heap\":%u},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(),
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  // "permission" e enviado pelo dispositivo, nunca recebido — deixa passar.
  if (strcmp(cmd, "permission") == 0) return false;

  // char_begin/file/chunk/file_end/char_end e desconhecidos: consome sem
  // ack (sem suporte a push de arquivos neste hardware).
  return true;
}
