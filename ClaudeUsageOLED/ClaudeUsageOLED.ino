/*
 * ClaudeUsageOLED.ino — monitor de uso do Claude (plano Pro/Max)
 * Placa: Heltec WiFi Kit 32 (V3)  — ESP32-S3 + OLED 0.96" 128x64 (SSD1306)
 *
 * Mostra no OLED, no estilo do widget de menu:
 *   - anel "5H"     : % da janela de sessao de 5 horas + tempo p/ resetar
 *   - anel "SEMANA" : % do limite semanal + tempo p/ resetar
 *   - ritmo (pace)  : uso% - tempo decorrido% (ex.: -33%)
 *   - cabecalho alterna CLAUDE MAX / SONNET x%
 *
 * Os dados vem de uma "ponte" rodando no seu PC (claude_usage_bridge.py),
 * que le o token do Claude Code e consulta a Anthropic. O ESP32 so fala
 * HTTP simples na rede local — nenhum token fica no microcontrolador.
 *
 * IDE Arduino:
 *   1. Boards Manager URL: https://resource.heltec.cn/download/package_heltec_esp32_index.json
 *   2. Instale "Heltec ESP32 Series Dev-boards" e selecione "WiFi Kit 32(V3)"
 *   3. Library Manager: instale "Heltec ESP32 Dev-Boards" e "ArduinoJson"
 *
 * Pinos do OLED ja definidos pela placa: SDA_OLED=17, SCL_OLED=18,
 * RST_OLED=21, Vext=36 (LOW = liga alimentacao do display).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "HT_SSD1306Wire.h"

// ========================= CONFIGURE AQUI =========================
const char* WIFI_SSID  = "SUA_REDE_WIFI";
const char* WIFI_PASS  = "SUA_SENHA";
const char* BRIDGE_URL = "http://192.168.1.50:8787/usage"; // IP do PC com a ponte
const uint32_t POLL_MS = 30000;   // consulta a ponte a cada 30 s
// ===================================================================

SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

struct Usage {
  bool   ok        = false;
  bool   stale     = false;
  float  fivePct   = -1, weekPct = -1, sonnetPct = -1;
  String fiveLeft  = "--", weekLeft = "--";
  int    fivePace  = 0, weekPace = 0;
  bool   hasFivePace = false, hasWeekPace = false;
  String error     = "aguardando";
} usage;

uint32_t lastPoll = 0;
uint32_t lastHeaderSwap = 0;
bool     headerAlt = false;

// ------------------------------------------------------------------ desenho
void drawGauge(int cx, int cy, int r, float pct) {
  // trilho pontilhado (anel de fundo)
  for (int a = 0; a < 360; a += 12) {
    float rad = (a - 90) * DEG_TO_RAD;
    display.setPixel(cx + lroundf(cosf(rad) * r), cy + lroundf(sinf(rad) * r));
  }
  if (pct < 0) return;                       // sem dados
  int sweep = lroundf(360.0f * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (pct > 0 && sweep < 6) sweep = 6;       // arco minimo visivel
  for (int a = 0; a <= sweep; a++) {         // arco de progresso, 3 px
    float rad = (a - 90) * DEG_TO_RAD;
    for (int rr = r - 1; rr <= r + 1; rr++) {
      display.setPixel(cx + lroundf(cosf(rad) * rr), cy + lroundf(sinf(rad) * rr));
    }
  }
}

String pctText(float pct) {
  if (pct < 0) return "--";
  if (pct >= 99.5f) return "100%";
  return String((int)lroundf(pct)) + "%";
}

String paceText(bool has, int pace) {
  if (!has) return "";
  return (pace > 0 ? " +" : " ") + String(pace) + "%";
}

String headerText() {
  if (headerAlt && usage.sonnetPct >= 0)
    return "SONNET " + pctText(usage.sonnetPct);
  return "CLAUDE  MAX";
}

void drawMain() {
  display.clear();
  display.setFont(ArialMT_Plain_10);

  // cabecalho + indicador de dados (cheio = ok, vazio = problema/atrasado)
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, headerText());
  if (usage.ok && !usage.stale) display.fillCircle(124, 5, 2);
  else                          display.drawCircle(124, 5, 2);

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(32, 11, "5H");
  display.drawString(96, 11, "SEMANA");

  drawGauge(32, 37, 13, usage.fivePct);
  drawGauge(96, 37, 13, usage.weekPct);
  display.drawString(32, 31, pctText(usage.fivePct));
  display.drawString(96, 31, pctText(usage.weekPct));

  // linha de baixo: tempo restante + ritmo (ex.: "3h00 -33%")
  display.drawString(32, 52, usage.fiveLeft + paceText(usage.hasFivePace, usage.fivePace));
  display.drawString(96, 52, usage.weekLeft + paceText(usage.hasWeekPace, usage.weekPace));

  display.display();
}

void drawMessage(const String& l1, const String& l2) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 18, l1);
  display.drawString(64, 34, l2);
  display.display();
}

// ------------------------------------------------------------------ dados
bool fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) {
    usage.ok = false; usage.error = "sem WiFi";
    return false;
  }
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(BRIDGE_URL)) { usage.ok = false; usage.error = "URL invalida"; return false; }

  int code = http.GET();
  if (code != 200) {
    usage.ok = false;
    usage.error = (code < 0) ? "ponte offline" : ("HTTP " + String(code));
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { usage.ok = false; usage.error = "JSON ruim"; return false; }

  if (!(doc["ok"] | false)) {
    usage.ok = false;
    usage.error = String((const char*)(doc["error"] | "erro na ponte"));
    return false;
  }

  usage.ok        = true;
  usage.stale     = doc["stale"] | false;
  usage.fivePct   = doc["five_hour_pct"]  | -1.0f;
  usage.weekPct   = doc["seven_day_pct"]  | -1.0f;
  usage.sonnetPct = doc["sonnet_pct"]     | -1.0f;
  usage.fiveLeft  = String((const char*)(doc["five_hour_left"] | "--"));
  usage.weekLeft  = String((const char*)(doc["seven_day_left"] | "--"));
  usage.hasFivePace = !doc["five_hour_pace"].isNull();
  usage.hasWeekPace = !doc["seven_day_pace"].isNull();
  usage.fivePace  = doc["five_hour_pace"] | 0;
  usage.weekPace  = doc["seven_day_pace"] | 0;
  return true;
}

// ------------------------------------------------------------------ setup/loop
void setup() {
  Serial.begin(115200);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);      // liga a alimentacao do OLED (LOW = ON)
  delay(100);

  pinMode(LED_BUILTIN, OUTPUT);

  display.init();
  display.setBrightness(180);
  drawMessage("Claude Usage", "conectando WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    drawMessage("WiFi ok", WiFi.localIP().toString());
    delay(800);
  } else {
    drawMessage("WiFi falhou", "verifique SSID/senha");
    delay(1500);
  }

  fetchUsage();
  lastPoll = millis();
}

void loop() {
  // reconecta WiFi se cair
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

  // consulta periodica
  if (millis() - lastPoll >= POLL_MS) {
    fetchUsage();
    lastPoll = millis();
  }

  // alterna o cabecalho a cada 4 s
  if (millis() - lastHeaderSwap >= 4000) {
    headerAlt = !headerAlt;
    lastHeaderSwap = millis();
  }

  // LED de alerta quando alguma janela passa de 90%
  bool alert = usage.ok && (usage.fivePct >= 90 || usage.weekPct >= 90);
  digitalWrite(LED_BUILTIN, (alert && (millis() / 500) % 2) ? HIGH : LOW);

  if (usage.ok) drawMain();
  else          drawMessage("Sem dados", usage.error);

  delay(100);
}
