#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "ble_bridge.h"
#include "cmds.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // muda quando as linhas mudam — UI reseta o scroll
  char     promptId[40];     // permission request pendente; vazio = sem prompt
  char     promptTool[20];
  char     promptHint[128];
  // Metricas de uso do plano (5h / semanal), enviadas pela ponte CLI.
  // pace 127 = sem dado.
  bool     usageValid;
  int16_t  u5Pct;            // -1 = sem dado
  char     u5Left[8];
  int8_t   u5Pace;
  int16_t  uwPct;
  char     uwLeft[8];
  int8_t   uwPace;
};

// ---------------------------------------------------------------------------
// Tres modos, em ordem de prioridade:
//   demo   → cicla cenarios fake a cada 8s, ignora dados reais
//   live   → JSON chegou nos ultimos 30s por USB ou BLE
//   asleep → sem dados, tudo zerado, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Keepalive do desktop e ~10s; 1.5x de folga.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Time-sync do desktop: epoch UTC vai pro relogio do sistema, offset do
// fuso fica guardado para exibir hora local.
static bool    _timeValid = false;
static int32_t _tzOffset  = 0;
inline bool dataTimeValid() { return _timeValid; }
inline void dataLocalTime(struct tm* out) {
  time_t local = time(nullptr) + _tzOffset;
  gmtime_r(&local, out);
}

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (cmdHandle(doc)) { _lastLiveMs = millis(); return; }

  // Desktop envia {"time":[epoch_sec, tz_offset_sec]} ao conectar.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    struct timeval tv = { (time_t)t[0].as<uint32_t>(), 0 };
    settimeofday(&tv, nullptr);
    _tzOffset = (int32_t)t[1];
    _timeValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject u = doc["usage"];
  if (!u.isNull()) {
    out->u5Pct  = u["u5"] | -1;
    out->uwPct  = u["uw"] | -1;
    const char* l5 = u["u5l"]; const char* lw = u["uwl"];
    strncpy(out->u5Left, l5 ? l5 : "--", sizeof(out->u5Left)-1); out->u5Left[sizeof(out->u5Left)-1]=0;
    strncpy(out->uwLeft, lw ? lw : "--", sizeof(out->uwLeft)-1); out->uwLeft[sizeof(out->uwLeft)-1]=0;
    out->u5Pace = u["u5p"] | 127;
    out->uwPace = u["uwp"] | 127;
    out->usageValid = (out->u5Pct >= 0 || out->uwPct >= 0);
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);
  // O ring buffer BLE e drenado manualmente — nao e um Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
