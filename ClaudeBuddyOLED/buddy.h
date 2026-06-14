#pragma once
#include <stdint.h>

// Multi-species ASCII buddy renderer (porte Heltec V3 / SSD1306 128x64).
// Cada species vive no seu proprio <nome>.cpp e expoe 7 funcoes de estado
// na ordem do PersonaState: sleep, idle, busy, attention, celebrate,
// dizzy, heart.
void buddyInit();
// Avanca a animacao (5 fps interno) e desenha a species atual no display.
// O chamador limpa o framebuffer e chama display.display().
void buddyTick(uint8_t personaState);
// Desloca o pet `d` px p/ baixo do topo padrão (0=topo, >0=centralizado).
void buddySetYShift(int d);
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Funcao de estado por species: recebe o tickCount global e desenha o
// buddy + overlays do estado atual no framebuffer compartilhado.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;   // ignorado no OLED mono — mantido p/ compat
  StateFn states[7];    // indexado por PersonaState (0=sleep .. 6=heart)
};
