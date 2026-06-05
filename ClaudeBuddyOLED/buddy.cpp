#include "buddy.h"
#include "buddy_common.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>

extern Adafruit_SSD1306 display;

// Espelha PersonaState no .ino
enum { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

// ──────────────── geometria compartilhada ────────────────
// OLED 128x64: arte de 12 col x 5 linhas (72x40 px) centrada, corpo em
// y=5..45, particulas (Zzz, !, coracoes) acima em y=0..5 (as primeiras
// linhas das species costumam ser vazias, entao ha mais folga visual).
// As duas linhas de status do .ino vivem em y=45 e y=54 — o case 3D
// cobre os ultimos ~2px do vidro, entao nada util fica em y>=62.
const int BUDDY_X_CENTER  = 64;
const int BUDDY_CANVAS_W  = 128;
const int BUDDY_Y_BASE    = 5;
const int BUDDY_Y_OVERLAY = 0;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

// ──────────────── cores compartilhadas ────────────────
// Mono: qualquer cor != 0 vira branco. Os valores RGB565 originais sao
// mantidos so para as species compilarem sem mudanca.
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

static inline uint16_t monoColor(uint16_t c) {
  return c ? SSD1306_WHITE : SSD1306_BLACK;
}

// ──────────────── helpers de render compartilhados ────────────────
void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  int w = len * BUDDY_CHAR_W;
  int x = BUDDY_X_CENTER - w / 2 + xOff;
  display.setTextColor(monoColor(color));   // bg transparente: espacos nao apagam
  display.setCursor(x, yPx);
  display.print(line);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
  display.setTextSize(1);
  for (uint8_t i = 0; i < nLines; i++) {
    buddyPrintLine(lines[i], BUDDY_Y_BASE + yOffset + i * BUDDY_CHAR_H, color, xOff);
  }
}

void buddySetCursor(int x, int y) { display.setCursor(x, y); }
void buddySetColor(uint16_t fg)   { display.setTextColor(monoColor(fg)); }
void buddyPrint(const char* s)    { display.setTextSize(1); display.print(s); }

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

// No OLED o framebuffer inteiro e redesenhado e enviado a cada frame pelo
// .ino, entao nao ha gating de redraw como no original — so o avanco da
// animacao e limitado a TICK_MS.
void buddyTick(uint8_t personaState) {
  uint32_t now = millis();
  if ((int32_t)(now - nextTickAt) >= 0) {
    nextTickAt = now + TICK_MS;
    tickCount++;
  }
  if (personaState >= 7) personaState = B_IDLE;
  const Species* sp = SPECIES_TABLE[currentSpeciesIdx];
  if (sp->states[personaState]) sp->states[personaState](tickCount);
}
