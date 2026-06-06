/*
 * ClaudeBuddyCardputer.ino — porte do claude-desktop-buddy (Anthropic)
 * para o M5Stack Cardputer Adv (StampS3A/ESP32-S3, ST7789 240x135,
 * teclado QWERTY 56 teclas, IMU BMI270, alto-falante).
 *
 * Layout paisagem: pet colorido a 2x na metade esquerda (mesma geometria
 * do original), painel de status/aprovacao a direita. Em relacao ao
 * porte Heltec, voltam: cores, IMU (shake = tonto, de bruco = soneca),
 * beeps e bateria. GIFs ficam para a v2 (precisam de teste visual).
 *
 * Teclas:
 *   Y / Enter   aprovar prompt        N / Del   negar prompt
 *   Tab / Space proxima tela          S         trocar pet (home)
 *   D           modo demo (stats)     B         brilho
 *
 * Pareamento: Claude desktop ou ponte CLI (claude-buddy-bridge/) — o
 * dispositivo anuncia "Claude-XXXX" e mostra passkey de 6 digitos.
 *
 * Build: arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer .
 * Libs: M5Cardputer, M5Unified, M5GFX, ArduinoJson.
 */

#include <M5Cardputer.h>
#include <esp_mac.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "buddy.h"
#include "stats.h"
#include "data.h"

const int W = 240, H = 135;
const int PANEL_X = 142;            // painel direito comeca aqui
const int PANEL_W = W - PANEL_X;    // 98 px = 16 colunas size 1

M5Canvas spr(&M5Cardputer.Display);

// Cores (RGB565), mesmas familias do original
const uint16_t C_BG    = 0x0000;
const uint16_t C_TEXT  = 0xFFFF;
const uint16_t C_DIM   = 0x8410;
const uint16_t C_BODY  = 0xFD20;    // laranja claude
const uint16_t C_HOT   = 0xFA20;    // alertas / negar
const uint16_t C_GREEN = 0x07E0;
const uint16_t C_YEL   = 0xFFE0;

// Anuncia como "Claude-XXXX" (ultimos 2 bytes do MAC BT)
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
uint8_t  brightLevel    = 3;        // 0..4
const uint32_t SCREEN_OFF_MS = 2UL * 60UL * 1000UL;

// IMU
float    accelBaseline = 1.0f;
uint32_t lastShakeCheck = 0;
bool     napping = false;
uint32_t napStartMs = 0;

static void applyBrightness() {
  M5Cardputer.Display.setBrightness(40 + brightLevel * 50);
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5Cardputer.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    applyBrightness();
    screenOff = false;
  }
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

// IMU: de bruco = eixo Z dominante e negativo (tela para baixo)
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

// ──────────────── telas ────────────────

static void fmtTokens(char* out, size_t n, uint32_t v) {
  if (v >= 1000000)   snprintf(out, n, "%lu.%luM", (unsigned long)(v/1000000), (unsigned long)((v/100000)%10));
  else if (v >= 1000) snprintf(out, n, "%lu.%luK", (unsigned long)(v/1000), (unsigned long)((v/100)%10));
  else                snprintf(out, n, "%lu", (unsigned long)v);
}

static void drawPasskey() {
  spr.fillSprite(C_BG);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setCursor(54, 20);  spr.print("PAREAMENTO BLUETOOTH");
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setTextSize(3);
  spr.setTextColor(C_TEXT, C_BG);
  spr.setCursor((W - 6 * 18) / 2, 54);
  spr.print(b);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setCursor(66, 100); spr.print("digite no desktop");
}

// Painel direito em modo aprovacao. O hint (descricao humana + comando,
// ate 120 chars vindos da ponte) ocupa 6 linhas; mais que isso vira
// paginas que trocam sozinhas a cada 3s — legivel de longe sem teclar.
static void drawApproval() {
  spr.setTextSize(1);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(waited >= 10 ? C_HOT : C_DIM, C_BG);
  spr.setCursor(PANEL_X, 4);
  spr.printf("aprovar? %lus", (unsigned long)waited);
  spr.setTextColor(C_TEXT, C_BG);
  spr.setCursor(PANEL_X, 16);
  spr.printf("%.16s", tama.promptTool);

  const int COLS = 16, ROWS = 6, PAGE = COLS * ROWS;
  int hlen = strlen(tama.promptHint);
  int nPages = (hlen + PAGE - 1) / PAGE;
  if (nPages < 1) nPages = 1;
  int page = (int)((millis() - promptArrivedMs) / 3000) % nPages;
  spr.setTextColor(C_DIM, C_BG);
  for (int i = 0; i < ROWS; i++) {
    int off = page * PAGE + i * COLS;
    if (off >= hlen) break;
    spr.setCursor(PANEL_X, 30 + i * 10);
    spr.printf("%.16s", tama.promptHint + off);
  }
  if (nPages > 1) {
    spr.setTextColor(C_DIM, C_BG);
    spr.setCursor(W - 24, 92);
    spr.printf("%d/%d", page + 1, nPages);
  }

  if (responseSent) {
    spr.setTextColor(C_DIM, C_BG);
    spr.setCursor(PANEL_X, 112); spr.print("enviado...");
  } else {
    spr.setTextColor(C_GREEN, C_BG);
    spr.setCursor(PANEL_X, 104); spr.print("Y/Enter = sim");
    spr.setTextColor(C_HOT, C_BG);
    spr.setCursor(PANEL_X, 116); spr.print("N/Del   = nao");
  }
}

// Painel direito normal: nome, spinner, transcript, resumo
static void drawPanel() {
  spr.setTextSize(1);
  spr.setTextColor(C_BODY, C_BG);
  spr.setCursor(PANEL_X, 4);
  if (ownerName()[0]) spr.printf("%.16s", petName());
  else                spr.printf("%.16s", btName);

  // msg do spinner em 2 linhas
  spr.setTextColor(C_TEXT, C_BG);
  const char* m = (dataConnected() || dataDemo()) ? tama.msg
                  : (bleConnected() ? "sem dados" : "aguardando BLE");
  spr.setCursor(PANEL_X, 18); spr.printf("%.16s", m);
  if (strlen(m) > 16) { spr.setCursor(PANEL_X, 28); spr.printf("%.16s", m + 16); }

  // transcript: 4 linhas mais recentes
  spr.setTextColor(C_DIM, C_BG);
  int y = 44;
  for (int i = 0; i < tama.nLines && i < 4; i++) {
    spr.setCursor(PANEL_X, y);
    spr.printf("%.16s", tama.lines[i]);
    y += 10;
  }

  // resumo
  char t[12];
  fmtTokens(t, sizeof(t), tama.tokensToday);
  spr.setTextColor(tama.sessionsWaiting ? C_HOT : C_DIM, C_BG);
  spr.setCursor(PANEL_X, 104);
  if (tama.sessionsWaiting) spr.printf("%u sess  %u!", tama.sessionsTotal, tama.sessionsWaiting);
  else                      spr.printf("%u sess", tama.sessionsTotal);
  spr.setTextColor(C_DIM, C_BG);
  spr.setCursor(PANEL_X, 116);
  spr.printf("hoje %s tk", t);
}

static void drawHome() {
  // O canvas inteiro e limpo a cada render, entao o gating interno do
  // buddyTick (que so redesenha no tick de 200ms) apagaria o pet entre
  // ticks — invalida para forcar o redraw em todo frame.
  buddyInvalidate();
  buddyTick(activeState);
  spr.drawFastVLine(138, 4, H - 8, C_DIM);
  if (tama.promptId[0]) drawApproval();
  else                  drawPanel();
}

static void drawStats() {
  spr.setTextSize(1);
  int y = 4;
  auto ln = [&](uint16_t c, const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setTextColor(c, C_BG); spr.setCursor(6, y); spr.print(b); y += 11;
  };
  if (ownerName()[0]) ln(C_BODY, "%s's %s", ownerName(), petName());
  else                ln(C_BODY, "%s (%s)", petName(), buddySpeciesName());
  y += 3;
  ln(C_TEXT, "Lv %u   mood %u/4   energia %u/5",
     stats().level, statsMoodTier(), statsEnergyTier());
  ln(C_TEXT, "fed %u/10", statsFedProgress());
  ln(C_DIM,  "aprovados %u   negados %u", stats().approvals, stats().denials);
  char t1[12], t2[12];
  fmtTokens(t1, sizeof(t1), stats().tokens);
  fmtTokens(t2, sizeof(t2), tama.tokensToday);
  ln(C_DIM,  "tokens %s   hoje %s", t1, t2);
  uint16_t vel = statsMedianVelocity();
  if (vel) ln(C_DIM, "resposta media %us", vel);
  uint32_t nap = stats().napSeconds;
  ln(C_DIM,  "soneca %luh%02lum", nap/3600, (nap/60)%60);
  spr.setTextColor(C_DIM, C_BG);
  spr.setCursor(6, H - 12);
  spr.printf("D demo %s   TAB tela", dataDemo() ? "ON" : "off");
}

static void drawInfo() {
  spr.setTextSize(1);
  int y = 4;
  auto ln = [&](uint16_t c, const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setTextColor(c, C_BG); spr.setCursor(6, y); spr.print(b); y += 11;
  };
  ln(C_TEXT, "%s", btName);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  ln(C_DIM, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  y += 3;
  ln(C_DIM, "link %s%s", dataScenarioName(), bleSecure() ? " (cripto)" : "");
  int pct = M5Cardputer.Power.getBatteryLevel();
  bool chg = (int)M5Cardputer.Power.isCharging() == 1;
  ln(chg ? C_GREEN : C_DIM, "bateria %d%%%s", pct, chg ? " carregando" : "");
  uint32_t up = millis() / 1000;
  ln(C_DIM, "up %luh%02lum   heap %uK", up/3600, (up/60)%60, ESP.getFreeHeap()/1024);
  y += 3;
  if (!bleConnected()) {
    ln(C_TEXT, "PARA CONECTAR");
    ln(C_DIM, "desktop: Developer >");
    ln(C_DIM, "  Hardware Buddy");
    ln(C_DIM, "CLI: buddyctl start");
  } else {
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln(C_DIM, "ult. msg %lus   %s", (unsigned long)age, stateNames[activeState]);
  }
}

// ──────────────── teclado ────────────────
static void handleKey(char c) {
  if (screenOff) { wake(); return; }    // tecla que acorda nao age
  wake();
  bool inPrompt = tama.promptId[0] && !responseSent;

  if (inPrompt && (c == 'y' || c == '\n')) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
    sendCmd(cmd);
    responseSent = true;
    uint32_t tookS = (millis() - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    beep(2400, 60);
    if (tookS < 5) triggerOneShot(P_HEART, 2000);
    return;
  }
  if (inPrompt && (c == 'n' || c == '\b')) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
    sendCmd(cmd);
    responseSent = true;
    statsOnDenial();
    beep(600, 60);
    return;
  }

  switch (c) {
    case '\t':
    case ' ':
      beep(1800, 30);
      screen = (screen + 1) % SCR_COUNT;
      break;
    case 's':
      if (screen == SCR_HOME) { beep(1400, 30); buddyNextSpecies(); buddyInvalidate(); }
      break;
    case 'd':
      if (screen == SCR_STATS) { beep(1400, 30); dataSetDemo(!dataDemo()); }
      break;
    case 'b':
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      break;
  }
}

static void pollKeyboard() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
  for (char c : st.word) handleKey(tolower(c));
  if (st.enter) handleKey('\n');
  if (st.del)   handleKey('\b');
  if (st.tab)   handleKey('\t');
  if (st.space) handleKey(' ');
}

// ──────────────── setup / loop ────────────────

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);     // true = teclado
  M5Cardputer.Display.setRotation(1);
  applyBrightness();

  spr.setColorDepth(16);
  spr.createSprite(W, H);
  spr.setTextWrap(false);

  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  startBt();
  lastInteractMs = millis();

  // Splash
  spr.fillSprite(C_BG);
  spr.setTextSize(2);
  if (ownerName()[0]) {
    char line[40];
    snprintf(line, sizeof(line), "%s's", ownerName());
    spr.setTextColor(C_TEXT, C_BG);
    spr.setCursor((W - strlen(line) * 12) / 2, 44);  spr.print(line);
    spr.setTextColor(C_BODY, C_BG);
    spr.setCursor((W - strlen(petName()) * 12) / 2, 70); spr.print(petName());
  } else {
    spr.setTextColor(C_BODY, C_BG);
    spr.setCursor((W - 6 * 12) / 2, 48); spr.print("Hello!");
    spr.setTextSize(1);
    spr.setTextColor(C_DIM, C_BG);
    spr.setCursor((W - 15 * 6) / 2, 76); spr.print("a buddy appears");
  }
  spr.setTextSize(1);
  spr.pushSprite(0, 0);
  delay(1500);

  Serial.printf("buddy: ASCII '%s', BLE '%s'\n", buddySpeciesName(), btName);
}

void loop() {
  M5Cardputer.update();
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Chegada de prompt: beep, acorda, volta pra HOME
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      wake();
      beep(1200, 80);
      screen = SCR_HOME;
    }
  }
  bool inPrompt = tama.promptId[0] && !responseSent;

  // IMU: shake → tonto
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    M5.Imu.update();
    if (!screenOff && !napping && checkShake()
        && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
    }
  }

  // IMU: de bruco → soneca (energia recarrega), com histerese
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down) { if (faceDownFrames < 20) faceDownFrames++; }
    else      { if (faceDownFrames > -10) faceDownFrames--; }
  }
  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5Cardputer.Display.setBrightness(8);
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // Passkey / conexao acordam a tela
  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;
  static bool wasConnected = false;
  if (dataConnected() && !wasConnected) wake();
  wasConnected = dataConnected();

  pollKeyboard();

  // Tela apaga apos inatividade sem desktop conectado
  if (!screenOff && !inPrompt && !dataConnected() && !pk && !napping
      && now - lastInteractMs > SCREEN_OFF_MS) {
    M5Cardputer.Display.setBrightness(0);
    screenOff = true;
  }

  // Render a ~20 fps (animacao interna do pet e 5 fps)
  static uint32_t nextRender = 0;
  if (!screenOff && !napping && (int32_t)(now - nextRender) >= 0) {
    nextRender = now + 50;
    if (pk) {
      drawPasskey();
    } else {
      spr.fillSprite(C_BG);
      if (screen == SCR_HOME)       drawHome();
      else if (screen == SCR_STATS) drawStats();
      else                          drawInfo();
    }
    spr.pushSprite(0, 0);
  }

  delay(screenOff ? 50 : 10);
}
