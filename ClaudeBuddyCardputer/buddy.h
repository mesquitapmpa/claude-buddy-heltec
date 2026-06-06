#pragma once
#include <stdint.h>

// Multi-species ASCII buddy renderer (porte M5Stack Cardputer Adv).
// Cada species vive no seu proprio <nome>.cpp e expoe 7 funcoes de estado
// na ordem do PersonaState: sleep, idle, busy, attention, celebrate,
// dizzy, heart. Renderiza colorido no canvas M5GFX (paisagem 240x135,
// buddy ocupa a metade esquerda).
void buddyInit();
void buddyTick(uint8_t personaState);
void buddyInvalidate();
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Funcao de estado por species: recebe o tickCount global e desenha o
// buddy + overlays do estado atual no canvas compartilhado.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // indexado por PersonaState (0=sleep .. 6=heart)
};
