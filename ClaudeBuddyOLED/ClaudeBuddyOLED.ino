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

// ──────────────── pinos Heltec WiFi Kit 32 V3 ────────────────
#define PIN_VEXT     36   // LOW = liga alimentacao do OLED
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_BTN       0   // botao PRG (LOW = pressionado)
#define PIN_LED      35   // LED branco onboard (HIGH = aceso)

const int W = 128, H = 64;
Adafruit_SSD1306 display(W, H, &Wire, PIN_OLED_RST);

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

static void drawPasskey() {
  display.setTextSize(1);
  display.setCursor(7, 4);
  display.print("PAREAMENTO BLUETOOTH");
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  display.setTextSize(2);
  display.setCursor((W - 6 * 12) / 2, 24);
  display.print(b);
  display.setTextSize(1);
  display.setCursor(13, 52);
  display.print("digite no desktop");
}

static void drawApproval() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  display.printf("aprovar? %lus", (unsigned long)waited);
  display.drawFastHLine(0, 10, W, SSD1306_WHITE);

  // Nome da ferramenta grande se couber em 10 colunas (size 2)
  int toolLen = strlen(tama.promptTool);
  display.setTextSize(toolLen <= 10 ? 2 : 1);
  display.setCursor(0, 14);
  display.print(tama.promptTool);
  display.setTextSize(1);

  // Hint em ate 2 linhas de 21 colunas
  display.setCursor(0, 34);
  display.printf("%.21s", tama.promptHint);
  if (strlen(tama.promptHint) > 21) {
    display.setCursor(0, 43);
    display.printf("%.21s", tama.promptHint + 21);
  }

  display.setCursor(0, 56);
  if (responseSent) {
    display.print("enviado...");
  } else {
    display.print("curto=SIM  longo=NAO");
  }
}

static void drawStatusLine() {
  display.setTextSize(1);
  display.setCursor(0, 56);
  if (dataConnected() || dataDemo()) {
    display.printf("%.21s", tama.msg);
  } else if (bleConnected()) {
    display.print("conectado, sem dados");
  } else {
    display.printf("%.13s  livre", btName);
  }
}

static void drawHome() {
  buddyTick(activeState);
  drawStatusLine();
}

static void drawStats() {
  display.setTextSize(1);
  int y = 0;
  auto ln = [&](const char* fmt, ...) {
    char b[24]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    display.setCursor(0, y); display.print(b); y += 9;
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
  display.setCursor(0, 56);
  display.printf("longo=demo %s", dataDemo() ? "ON" : "off");
}

static void drawInfo() {
  display.setTextSize(1);
  int y = 0;
  auto ln = [&](const char* fmt, ...) {
    char b[24]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    display.setCursor(0, y); display.print(b); y += 9;
  };
  ln("%s", btName);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  ln("%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  y += 2;
  ln("link %s%s", dataScenarioName(), bleSecure() ? " (cripto)" : "");
  uint32_t up = millis() / 1000;
  ln("up %luh%02lum  heap %uK", up/3600, (up/60)%60, ESP.getFreeHeap()/1024);
  y += 2;
  if (!bleConnected()) {
    ln("Claude desktop >");
    ln("Developer >");
    ln("Hardware Buddy");
  } else {
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("ult. msg %lus", (unsigned long)age);
    ln("estado %s", stateNames[activeState]);
  }
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

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 500000);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);

  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  startBt();
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
  if (!screenOff && !inPrompt && !dataConnected() && !pk
      && now - lastInteractMs > SCREEN_OFF_MS) {
    sleepScreen();
  }

  // Render a 10 fps (animacao interna do pet e 5 fps)
  static uint32_t nextRender = 0;
  if (!screenOff && (int32_t)(now - nextRender) >= 0) {
    nextRender = now + 100;
    display.clearDisplay();
    if (pk)                       drawPasskey();
    else if (tama.promptId[0])    drawApproval();
    else if (screen == SCR_HOME)  drawHome();
    else if (screen == SCR_STATS) drawStats();
    else                          drawInfo();
    display.display();
  }

  delay(screenOff ? 50 : 10);
}
