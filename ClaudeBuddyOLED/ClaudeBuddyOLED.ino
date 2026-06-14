/*
 * ClaudeBuddyOLED.ino — porte do claude-desktop-buddy (Anthropic) para a
 * Heltec WiFi Kit 32 V3 (ESP32-S3, OLED 0.96" 128x64 SSD1306).
 *
 * Um "desk pet" que acompanha suas sessoes do Claude desktop via BLE
 * (Nordic UART Service): dorme quando nada acontece, acorda quando voce
 * trabalha, fica impaciente quando ha aprovacoes pendentes — e deixa
 * aprovar/negar a permissao direto pelo botao PRG da placa.
 *
 * Diferencas vs. o original (M5StickCPlus):
 *   - display TFT colorido 135x240 → OLED mono 128x64 (so pets ASCII,
 *     sem personagens GIF)
 *   - sem IMU: nada de shake/face-down
 *   - 2 botoes → 1 botao PRG: curto = aprovar / trocar tela,
 *     longo (700ms) = negar / trocar pet
 *
 * Pareamento: Claude desktop (macOS/Win) → Help > Troubleshooting >
 * Enable Developer Mode → Developer > Open Hardware Buddy… → Connect.
 * A placa mostra o passkey de 6 digitos; digite no desktop.
 *
 * Build (Arduino IDE): placa "Heltec WiFi Kit 32(V3)" do core esp32,
 * libs Adafruit SSD1306 + Adafruit GFX + ArduinoJson.
 */

#include <Wire.h>
#include <esp_mac.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "ble_bridge.h"
#include "buddy.h"
#include "stats.h"
#include "data.h"
#include "battery.h"
#include "ota.h"

// ──────────────── pinos Heltec WiFi Kit 32 V3 ────────────────
#define PIN_VEXT     36   // LOW = liga alimentacao do OLED
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_BTN       0   // botao PRG (LOW = pressionado)
#define PIN_LED      35   // LED branco onboard (HIGH = aceso)
// Bateria 1S (1200mAh): leitura no GPIO1, divisor habilitado por GPIO37
// (ver battery.cpp). Pinos dedicados da Heltec V3 — sem conflito com o resto.

// Portrait: o painel é 128x64 nativo, mas giramos 90° → área lógica 64x128.
// Troque PORTRAIT_ROT entre 1 e 3 para inverter (conector USB em cima/embaixo).
#define PORTRAIT_ROT 1
const int W = 64, H = 128;                                // lógico (pós-rotação)
Adafruit_SSD1306 display(128, 64, &Wire, PIN_OLED_RST);   // painel nativo

// Anuncia como "Claude-XXXX" (ultimos 2 bytes do MAC BT) para distinguir
// varias placas no picker do desktop.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

enum Screen { SCR_HOME, SCR_STATS, SCR_INFO, SCR_COUNT };

// Tipos definidos no corpo do .ino nao podem aparecer em assinaturas de
// funcao — o gerador de prototipos do Arduino as insere antes dos enums.
// Por isso baseState/activeState e os retornos usam uint8_t.
TamaState tama;
uint8_t   baseState    = P_SLEEP;
uint8_t   activeState  = P_SLEEP;
uint32_t  oneShotUntil = 0;
uint8_t   screen       = SCR_HOME;

char     lastPromptId[40] = "";
uint32_t promptArrivedMs  = 0;
bool     responseSent     = false;

bool     screenOff      = false;
uint32_t lastInteractMs = 0;
bool     swallowBtn     = false;
const uint32_t SCREEN_OFF_MS = 5UL * 60UL * 1000UL;  // 5 min sem nada → OLED off

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOff = false;
  }
}

static void sleepScreen() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  screenOff = true;
}

uint8_t derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(uint8_t s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// ──────────────── telas ────────────────

static void fmtTokens(char* out, size_t n, uint32_t v) {
  if (v >= 1000000)   snprintf(out, n, "%lu.%luM", (unsigned long)(v/1000000), (unsigned long)((v/100000)%10));
  else if (v >= 1000) snprintf(out, n, "%lu.%luK", (unsigned long)(v/1000), (unsigned long)((v/100)%10));
  else                snprintf(out, n, "%lu", (unsigned long)v);
}

// Desenha `s` numa única linha em y. Se couber nos W px, fica estático;
// senão rola horizontalmente da direita p/ esquerda (pausa nas pontas).
// Sem quebra de string: a frase inteira passa, uma linha só.
static void drawMarquee(const char* s, int y) {
  int px = (int)strlen(s) * 6;
  if (px <= W) { display.setCursor(0, y); display.print(s); return; }
  int span = px - W;
  const int HOLD = 12;                       // frames parado em cada ponta
  int cycle = span + 2 * HOLD;
  int step = (int)(millis() / 120) % cycle;  // ~1px/120ms
  int sx = step - HOLD;
  if (sx < 0) sx = 0; else if (sx > span) sx = span;
  display.setCursor(-sx, y);
  display.print(s);
}

static void drawPasskey() {
  display.setTextSize(1);
  display.setCursor(2, 2);  display.print("PAREAR");
  display.setCursor(2, 11); display.print("BLUETOOTH");
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  // 6 dígitos em duas linhas de 3 (size2 = 36px, cabe nos 64).
  display.setTextSize(2);
  display.setCursor((W - 3 * 12) / 2, 34); display.printf("%.3s", b);
  display.setCursor((W - 3 * 12) / 2, 56); display.print(b + 3);
  display.setTextSize(1);
  display.setCursor(2, 86);  display.print("digite no");
  display.setCursor(2, 95);  display.print("desktop");
}

// Minimalista: cabeçalho (aprovar? / tool + tempo), a pergunta numa única
// linha que rola na horizontal (a ponte manda a descrição humana do
// comando), e o rodapé com SIM/NAO. Legível de longe, sem quebra de string.
static void drawApproval() {
  display.setTextSize(1);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  display.setCursor(0, 0);  display.print("aprovar?");
  display.setCursor(0, 11); display.printf("%.7s %lus", tama.promptTool, (unsigned long)waited);
  display.drawFastHLine(0, 22, W, SSD1306_WHITE);

  drawMarquee(tama.promptHint, 44);   // pergunta numa linha, rolando

  display.drawFastHLine(0, 98, W, SSD1306_WHITE);
  if (responseSent) {
    display.setCursor(0, 110); display.print("enviado...");
  } else {
    display.setCursor(0, 106); display.print("curto=SIM");
    display.setCursor(0, 115); display.print("longo=NAO");
  }
}

// Duas linhas sob o pet (y=45 e 54 — o case cobre y>=62): atividade
// estilo spinner do CLI ("Matutando 23s 708tk", vinda da ponte no campo
// msg) + resumo de sessoes e tokens do dia.
static void drawStatusLine() {
  display.setTextSize(1);
  if (dataConnected() || dataDemo()) {
    drawMarquee(tama.msg, 50);          // atividade numa linha, rolando
    char t[12];
    fmtTokens(t, sizeof(t), tama.tokensToday);
    display.setCursor(0, 64);
    if (tama.sessionsWaiting > 0)
      display.printf("%u ses %u!", tama.sessionsTotal, tama.sessionsWaiting);
    else
      display.printf("%u sess", tama.sessionsTotal);
    display.setCursor(0, 73);
    display.printf("hoje %s", t);
  } else if (bleConnected()) {
    drawMarquee("conectado, sem dados", 64);
  }
  // Estado ocioso (sem desktop/BLE) é tratado em drawHome: só o pet.
}

// Aviso de pausa esperando o usuario no PC (pergunta/notificacao que o
// botao nao decide). Ocupa as duas linhas de status com ">>" + texto
// paginado a cada 3s; o pet continua acima (impaciente via attention).
static void drawNotice() {
  display.setTextSize(1);
  display.setCursor(0, 50); display.print(">>");
  drawMarquee(tama.notice, 62);          // aviso numa linha, rolando
  display.setCursor(0, 86); display.print("responda");
  display.setCursor(0, 95); display.print("no PC");
}

// Indicador de bateria no canto superior direito (corpo 14x7 + terminal),
// com o percentual numérico à esquerda. O preenchimento é proporcional ao %;
// pisca quando a carga está baixa. Em USB/carregando mostra um "raio".
static void drawBatteryIcon(int x, int y) {
  if (!batteryValid()) return;
  // Percentual à esquerda do ícone, alinhado à direita até x-2.
  int pct = batteryPercent(); if (pct < 0) pct = 0;
  char pb[6]; snprintf(pb, sizeof(pb), "%d%%", pct);
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(x - (int)strlen(pb) * 6 - 2, y); display.print(pb);
  display.drawRect(x, y, 14, 7, SSD1306_WHITE);
  display.drawFastVLine(x + 14, y + 2, 3, SSD1306_WHITE);   // terminal +
  if (batteryCharging()) {
    display.fillRect(x + 1, y + 1, 12, 5, SSD1306_BLACK);
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(x + 4, y); display.print('~');        // em carga
    return;
  }
  int fill = (int)((pct / 100.0f) * 12 + 0.5f);
  if (fill < 0) fill = 0; if (fill > 12) fill = 12;
  bool blink = batteryLow() && (millis() / 500) % 2;        // pisca se baixa
  if (!blink && fill > 0) display.fillRect(x + 1, y + 1, fill, 5, SSD1306_WHITE);
}

static void drawHome() {
  // Ocioso (sem desktop/BLE/aviso): minimalista — só o pet centralizado
  // verticalmente e a bateria no canto. Com atividade, pet sobe e abre
  // espaço pro status embaixo.
  bool live = dataConnected() || dataDemo() || bleConnected() || tama.notice[0];
  buddySetYShift(live ? 0 : 39);
  buddyTick(activeState);
  drawBatteryIcon(W - 16, 0);
  if (!live) return;
  if (tama.notice[0]) drawNotice();
  else                drawStatusLine();
}

// ───── modo de espera: aneis de uso do plano (5H + SEMANA) ─────
// Mesmo visual do ClaudeUsageOLED: trilho pontilhado + arco de progresso.
static void drawRing(int cx, int cy, int r, int pct) {
  for (int a = 0; a < 360; a += 12) {
    float rad = (a - 90) * DEG_TO_RAD;
    display.drawPixel(cx + lroundf(cosf(rad) * r), cy + lroundf(sinf(rad) * r), SSD1306_WHITE);
  }
  if (pct < 0) return;
  if (pct > 100) pct = 100;
  int sweep = lroundf(360.0f * pct / 100.0f);
  if (pct > 0 && sweep < 6) sweep = 6;     // arco minimo visivel
  for (int a = 0; a <= sweep; a++) {
    float rad = (a - 90) * DEG_TO_RAD;
    for (int rr = r - 1; rr <= r + 1; rr++)
      display.drawPixel(cx + lroundf(cosf(rad) * rr), cy + lroundf(sinf(rad) * rr), SSD1306_WHITE);
  }
}

static void drawRingPct(int cx, int cy, int pct) {
  char b[6];
  if (pct < 0) snprintf(b, sizeof(b), "--");
  else         snprintf(b, sizeof(b), "%d%%", pct);
  display.setCursor(cx - (int)strlen(b) * 3, cy - 3);
  display.print(b);
}

static void drawUsage() {
  display.setTextSize(1);
  char b[16];
  // Portrait: dois anéis empilhados (cx=32). 5H em cima, SEMANA embaixo.
  display.setCursor(26, 0); display.print("5H");        // 2 col centradas
  drawRing(32, 24, 14, tama.u5Pct);
  drawRingPct(32, 24, tama.u5Pct);
  if (tama.u5Pace != 127) snprintf(b, sizeof(b), "%s %+d%%", tama.u5Left, tama.u5Pace);
  else                    snprintf(b, sizeof(b), "%s", tama.u5Left);
  drawMarquee(b, 42);

  display.setCursor(14, 64); display.print("SEMANA");   // 6 col centradas
  drawRing(32, 88, 14, tama.uwPct);
  drawRingPct(32, 88, tama.uwPct);
  if (tama.uwPace != 127) snprintf(b, sizeof(b), "%s %+d%%", tama.uwLeft, tama.uwPace);
  else                    snprintf(b, sizeof(b), "%s", tama.uwLeft);
  drawMarquee(b, 106);
}

static void drawStats() {
  display.setTextSize(1);
  int y = 0;
  auto ln = [&](const char* fmt, ...) {
    char b[24]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    drawMarquee(b, y); y += 11;          // linhas longas rolam, sem quebra
  };
  if (ownerName()[0]) ln("%s's %s", ownerName(), petName());
  else                ln("%s (%s)", petName(), buddySpeciesName());
  y += 2;
  ln("Lv %u   mood %u/4", stats().level, statsMoodTier());
  ln("fed %u/10", statsFedProgress());
  ln("aprovados %u  neg %u", stats().approvals, stats().denials);
  char t1[12], t2[12];
  fmtTokens(t1, sizeof(t1), stats().tokens);
  fmtTokens(t2, sizeof(t2), tama.tokensToday);
  ln("tokens %s  hoje %s", t1, t2);
  uint16_t vel = statsMedianVelocity();
  if (vel) ln("resposta media %us", vel);
  char d[18]; snprintf(d, sizeof(d), "longo=demo %s", dataDemo() ? "ON" : "off");
  drawMarquee(d, 119);
}

static void drawInfo() {
  display.setTextSize(1);
  int y = 0;
  auto ln = [&](const char* fmt, ...) {
    char b[24]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    drawMarquee(b, y); y += 11;          // linhas longas rolam, sem quebra
  };
  ln("%s", btName);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  ln("%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  ln("link %s%s", dataScenarioName(), bleSecure() ? " (cripto)" : "");
  uint32_t up = millis() / 1000;
  ln("up %luh%02lum  h%uK", up/3600, (up/60)%60, ESP.getFreeHeap()/1024);
  if (batteryCharging())   ln("bat %.2fv carregando", batteryVolts());
  else if (batteryValid()) ln("bat %.2fv  %d%%", batteryVolts(), batteryPercent());
  else                     ln("bat: lendo...");
  ln("ota %s", otaStatus());
  if (!bleConnected()) {
    ln("emparelhar:");
    ln("Desktop>Dev>Buddy");
  } else {
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("ult. msg %lus", (unsigned long)age);
    ln("estado %s", stateNames[activeState]);
  }
}

// Tela de progresso da gravação OTA — prioridade máxima durante o update.
static void drawOTA() {
  display.setTextSize(1);
  display.setCursor(0, 18); display.print("OTA WiFi");
  int p = otaProgress(); if (p < 0) p = 0; if (p > 100) p = 100;
  display.drawRect(4, 46, 56, 12, SSD1306_WHITE);
  display.fillRect(6, 48, (int)(52 * p / 100.0f + 0.5f), 8, SSD1306_WHITE);
  char b[8]; snprintf(b, sizeof(b), "%d%%", p);
  display.setTextSize(2);
  display.setCursor((W - (int)strlen(b) * 12) / 2, 74); display.print(b);
  display.setTextSize(1);
  drawMarquee("gravando, nao desligue", 104);
}

// ──────────────── botao PRG ────────────────
enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
static uint8_t pollButton() {
  static bool     wasDown    = false;
  static uint32_t downAt     = 0;
  static bool     longFired  = false;
  bool down = digitalRead(PIN_BTN) == LOW;
  uint32_t now = millis();
  uint8_t evt = BTN_NONE;

  if (down && !wasDown) { downAt = now; longFired = false; }
  if (down && !longFired && now - downAt >= 700) { longFired = true; evt = BTN_LONG; }
  if (!down && wasDown && !longFired && now - downAt >= 30) evt = BTN_SHORT;
  wasDown = down;
  return evt;
}

// ──────────────── setup / loop ────────────────

void setup() {
  Serial.begin(115200);

  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);     // liga o OLED
  delay(50);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  batteryInit();
  for (int i = 0; i < 5; i++) batteryPoll();   // semeia uma leitura estavel

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 500000);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setRotation(PORTRAIT_ROT);     // 64x128 portrait
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);

  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  startBt();
  otaInit(btName);          // WiFi OTA (inerte sem secrets.h válido)
  lastInteractMs = millis();

  // Splash
  display.clearDisplay();
  display.setTextSize(2);
  if (ownerName()[0]) {
    char line[40];
    snprintf(line, sizeof(line), "%s's", ownerName());
    display.setCursor((W - strlen(line) * 12) / 2, 14); display.print(line);
    display.setCursor((W - strlen(petName()) * 12) / 2, 36); display.print(petName());
  } else {
    display.setCursor((W - 6 * 12) / 2, 14); display.print("Hello!");
    display.setTextSize(1);
    display.setCursor((W - 15 * 6) / 2, 40); display.print("a buddy appears");
  }
  display.setTextSize(1);
  display.display();
  delay(1500);

  Serial.printf("buddy: ASCII '%s', BLE '%s'\n", buddySpeciesName(), btName);
}

void loop() {
  uint32_t now = millis();

  otaHandle();              // WiFi OTA: conecta em background, grava sem fio
  if (otaProgress() >= 0 && screenOff) wake();   // acorda a tela p/ mostrar o update

  // Bateria: amostra a cada 5 s (cada leitura liga o divisor por ~3 ms).
  static uint32_t nextBatt = 0;
  if ((int32_t)(now - nextBatt) >= 0) { nextBatt = now + 5000; batteryPoll(); }

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Chegada de prompt: acorda, volta pra HOME, zera flag de resposta
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      wake();
      screen = SCR_HOME;
    }
  }
  bool inPrompt = tama.promptId[0] && !responseSent;

  // LED pulsa quando ha aprovacao esperando
  digitalWrite(PIN_LED, (activeState == P_ATTENTION && (now / 400) % 2) ? HIGH : LOW);

  // Passkey na tela = acorda
  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) wake();
  lastPasskey = pk;

  // Dados chegando tambem acordam a tela
  static bool wasConnected = false;
  if (dataConnected() && !wasConnected) wake();
  wasConnected = dataConnected();

  // Botao PRG
  uint8_t evt = pollButton();
  if (evt != BTN_NONE) {
    if (screenOff) {
      wake();                       // o toque que acorda nao executa acao
    } else if (inPrompt) {
      char cmd[96];
      if (evt == BTN_SHORT) {
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (now - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else {
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
      }
    } else if (evt == BTN_SHORT) {
      screen = (screen + 1) % SCR_COUNT;
      wake();
    } else {  // BTN_LONG fora de prompt
      if (screen == SCR_HOME)       buddyNextSpecies();
      else if (screen == SCR_STATS) dataSetDemo(!dataDemo());
      wake();
    }
  }

  // Desliga o OLED apos inatividade sem desktop conectado (burn-in).
  // Conectado, o pet fica visivel — e a razao de existir do buddy.
  if (!screenOff && !inPrompt && !dataConnected() && !pk && otaProgress() < 0
      && now - lastInteractMs > SCREEN_OFF_MS) {
    sleepScreen();
  }

  // Render a 10 fps (animacao interna do pet e 5 fps)
  static uint32_t nextRender = 0;
  if (!screenOff && (int32_t)(now - nextRender) >= 0) {
    nextRender = now + 100;
    display.clearDisplay();
    if (otaProgress() >= 0)       drawOTA();
    else if (pk)                  drawPasskey();
    else if (tama.promptId[0])    drawApproval();
    else if (screen == SCR_HOME) {
      // Modo de espera: nada rodando ha 20s → alterna pet dormindo e
      // aneis de uso do plano a cada 15s. Qualquer atividade volta.
      bool standby = tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
                  && tama.usageValid && dataConnected()
                  && now - lastInteractMs > 20000;
      if (standby && (now / 15000) % 2) drawUsage();
      else                              drawHome();
    }
    else if (screen == SCR_STATS) drawStats();
    else                          drawInfo();
    display.display();
  }

  delay(screenOff ? 50 : 10);
}
