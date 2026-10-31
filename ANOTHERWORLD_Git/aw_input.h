// ANOTHERWORLD — entrées : clavier USB (EspUsbHost) et port série (banc de test).
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#pragma once
#include <stdint.h>

struct AwInput
{
  bool left, right, up, down, action;
  bool pause;          // bascule (touche P)
  bool code;           // touche C : écran du code d'accès (impulsion, remise à 0 par le lecteur)
  char lastChar;       // lettre ou 8 (retour arrière) pour l'écran du code d'accès
  uint16_t partReq;    // F5..F11 : partie 16001..16007 (0 = aucune)
};

extern AwInput g_awIn;

void AwIn_Init(void);
void AwIn_Poll(void);   // à chaque trame : port série, relâchement des touches série
