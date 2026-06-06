#include "buddy.h"
#include "buddy_common.h"
#include <M5Cardputer.h>
#include <string.h>

// Canvas compartilhado definido no .ino (M5Canvas herda de LovyanGFX,
// mesma familia de API do TFT_eSprite do original).
extern M5Canvas spr;

// Espelha PersonaState no .ino
enum { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

// ──────────────── geometria compartilhada ────────────────
// Paisagem 240x135: o buddy vive na metade esquerda (0..134), mesma
// geometria do original (que era retrato 135 de largura). A 2x o corpo
// alcanca y~126, igual ao home screen original.
const int BUDDY_X_CENTER  = 67;
const int BUDDY_CANVAS_W  = 135;
const int BUDDY_Y_BASE    = 30;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

// ──────────────── cores compartilhadas (RGB565, como o original) ─────
const uint16_t BUDDY_BG     = 0x0000;
const uint16_t BUDDY_HEART  = 0xF810;
const uint16_t BUDDY_DIM    = 0x8410;
const uint16_t BUDDY_YEL    = 0xFFE0;
const uint16_t BUDDY_WHITE  = 0xFFFF;
const uint16_t BUDDY_CYAN   = 0x07FF;
const uint16_t BUDDY_GREEN  = 0x07E0;
const uint16_t BUDDY_PURPLE = 0xA01F;
const uint16_t BUDDY_RED    = 0xF800;
const uint16_t BUDDY_BLUE   = 0x041F;

// ──────────────── helpers de render compartilhados ────────────────
// Identico ao original, trocando TFT_eSPI* por LovyanGFX* (base comum de
// M5Canvas e M5Cardputer.Display).
static lgfx::LovyanGFX* _tgt = nullptr;
// 2x na tela inicial, 1x em modo peek (stats/info). A 2x as linhas sao
// trimadas e re-centradas para o padding nao empurrar arte para fora.
static uint8_t _scale = 2;

static lgfx::LovyanGFX* tgt() {
  if (!_tgt) _tgt = &spr;
  return _tgt;
}

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  if (_scale > 1) {
    while (len && line[len-1] == ' ') len--;
    while (len && *line == ' ')       { line++; len--; }
  }
  int w = len * BUDDY_CHAR_W * _scale;
  int x = BUDDY_X_CENTER - w / 2 + xOff * _scale;
  tgt()->setTextColor(color, BUDDY_BG);
  tgt()->setCursor(x, yPx);
  for (int i = 0; i < len; i++) tgt()->print(line[i]);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
  tgt()->setTextSize(_scale);
  int yBase = BUDDY_Y_BASE * _scale - (_scale - 1) * 14;
  for (uint8_t i = 0; i < nLines; i++) {
    buddyPrintLine(lines[i], yBase + (yOffset + i * BUDDY_CHAR_H) * _scale, color, xOff);
  }
}

// Species passam coords 1x (relativas a BUDDY_X_CENTER / BUDDY_Y_OVERLAY);
// a transformacao aqui mantem os 18 arquivos agnosticos de escala.
void buddySetCursor(int x, int y) {
  tgt()->setCursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * _scale, y * _scale);
}
void buddySetColor(uint16_t fg)   { tgt()->setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s)    { tgt()->setTextSize(_scale); tgt()->print(s); }

// ──────────────── registro de species ────────────────
extern const Species CAPYBARA_SPECIES;
extern const Species DUCK_SPECIES;
extern const Species GOOSE_SPECIES;
extern const Species BLOB_SPECIES;
extern const Species CAT_SPECIES;
extern const Species DRAGON_SPECIES;
extern const Species OCTOPUS_SPECIES;
extern const Species OWL_SPECIES;
extern const Species PENGUIN_SPECIES;
extern const Species TURTLE_SPECIES;
extern const Species SNAIL_SPECIES;
extern const Species GHOST_SPECIES;
extern const Species AXOLOTL_SPECIES;
extern const Species CACTUS_SPECIES;
extern const Species ROBOT_SPECIES;
extern const Species RABBIT_SPECIES;
extern const Species MUSHROOM_SPECIES;
extern const Species CHONK_SPECIES;

static const Species* SPECIES_TABLE[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES,
  &CAT_SPECIES, &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES,
  &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
  &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES,
  &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t currentSpeciesIdx = 0;

// ──────────────── estado do tick ────────────────
static uint32_t tickCount  = 0;
static uint32_t nextTickAt = 0;
static const uint32_t TICK_MS = 200;

#include "stats.h"

void buddyInit() {
  tickCount = 0;
  nextTickAt = 0;
  uint8_t saved = speciesIdxLoad();
  if (saved < N_SPECIES) currentSpeciesIdx = saved;
}

void buddySetSpeciesIdx(uint8_t idx) {
  if (idx < N_SPECIES) currentSpeciesIdx = idx;
}

void buddySetSpecies(const char* name) {
  for (uint8_t i = 0; i < N_SPECIES; i++) {
    if (strcmp(SPECIES_TABLE[i]->name, name) == 0) {
      currentSpeciesIdx = i;
      return;
    }
  }
}

const char* buddySpeciesName() { return SPECIES_TABLE[currentSpeciesIdx]->name; }
uint8_t buddySpeciesCount()    { return N_SPECIES; }
uint8_t buddySpeciesIdx()      { return currentSpeciesIdx; }

void buddyNextSpecies() {
  currentSpeciesIdx = (currentSpeciesIdx + 1) % N_SPECIES;
  speciesIdxSave(currentSpeciesIdx);
}

// So redesenha quando o tickCount muda (animacao a 5 fps, loop a ~60) ou
// quando estado/species mudam — mesmo gating do original.
static uint8_t lastDrawnState = 0xFF;
static uint8_t lastDrawnSpecies = 0xFF;
void buddyInvalidate() { lastDrawnState = 0xFF; }

void buddySetPeek(bool peek) {
  uint8_t s = peek ? 1 : 2;
  if (s == _scale) return;
  _scale = s;
  buddyInvalidate();
}

void buddyTick(uint8_t personaState) {
  uint32_t now = millis();
  bool ticked = false;
  if ((int32_t)(now - nextTickAt) >= 0) {
    nextTickAt = now + TICK_MS;
    tickCount++;
    ticked = true;
  }

  if (personaState >= 7) personaState = B_IDLE;
  if (!ticked && personaState == lastDrawnState
              && currentSpeciesIdx == lastDrawnSpecies) {
    return;
  }
  lastDrawnState = personaState;
  lastDrawnSpecies = currentSpeciesIdx;

  // Limpa a faixa do buddy (metade esquerda) — a 2x o corpo chega a
  // y~126; o fillRect clipa na altura do canvas.
  spr.fillRect(0, 0, BUDDY_CANVAS_W,
               (BUDDY_Y_BASE + 5 * BUDDY_CHAR_H + 12) * _scale, BUDDY_BG);

  const Species* sp = SPECIES_TABLE[currentSpeciesIdx];
  if (sp->states[personaState]) sp->states[personaState](tickCount);
}
