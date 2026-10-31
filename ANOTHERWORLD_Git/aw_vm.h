// ANOTHERWORLD — machine virtuelle : 64 fils d'exécution coopératifs, 256 variables 16 bits.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#pragma once
#include <stdint.h>

enum
{
  AW_VAR_RANDOM_SEED      = 0x3C,
  AW_VAR_LAST_KEYCHAR     = 0xDA,
  AW_VAR_HERO_POS_UP_DOWN = 0xE5,
  AW_VAR_MUS_MARK         = 0xF4,
  AW_VAR_SCROLL_Y         = 0xF9,
  AW_VAR_HERO_ACTION      = 0xFA,
  AW_VAR_HERO_JUMP_DOWN   = 0xFB,
  AW_VAR_HERO_LEFT_RIGHT  = 0xFC,
  AW_VAR_HERO_POS_MASK    = 0xFD,
  AW_VAR_HERO_ACTION_MASK = 0xFE,
  AW_VAR_PAUSE_SLICES     = 0xFF,
};

extern volatile int16_t g_awVar[256];

void AwVm_Init(void);
bool AwVm_StartPart(uint16_t part);   // charge la partie et relance le fil 0
bool AwVm_RunFrame(void);             // une trame : demandes en attente, entrées, tous les fils
