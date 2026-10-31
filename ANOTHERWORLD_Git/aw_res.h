// ANOTHERWORLD — ressources : table MEMLIST.BIN, banques BANKxx, décompression, parties du jeu.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#pragma once
#include <stdint.h>

enum
{
  AW_RT_SOUND   = 0,   // échantillon 8 bits signé
  AW_RT_MUSIC   = 1,   // module de musique (4 voies)
  AW_RT_BITMAP  = 2,   // image plein écran 4 plans, copiée en page 0 dès son chargement
  AW_RT_PALETTE = 3,   // 32 palettes de 16 couleurs 12 bits
  AW_RT_CODE    = 4,   // bytecode d'une partie
  AW_RT_POLY    = 5,   // polygones de la partie
  AW_RT_POLY2   = 6,   // polygones communs (personnages)
};

enum { AW_ST_FREE = 0, AW_ST_LOADED = 1 };

struct AwMemEntry
{
  uint8_t  state;
  uint8_t  type;
  uint8_t  bank;
  uint32_t offset;     // position dans le fichier BANKxx
  uint32_t packed;     // taille compressée
  uint32_t size;       // taille décompressée
  uint8_t *buf;        // PSRAM, nullptr si non chargée
};

#define AW_MAX_ENTRIES 256

extern AwMemEntry g_awMem[AW_MAX_ENTRIES];
extern int        g_awMemCount;

// segments de la partie en cours
extern const uint8_t *g_awSegPal, *g_awSegCode, *g_awSegPoly, *g_awSegPoly2;
extern uint32_t       g_awSegCodeSize;
extern uint16_t       g_awCurPart;
extern uint16_t       g_awNextPart;   // 0 = aucune demande
extern bool           g_awAmiga;      // données de la version Amiga (BANK01 de 244674 / 244868 octets)

bool AwRes_Init(void);                 // lit /AW/MEMLIST.BIN ; faux si absent ou illisible
bool AwRes_SetupPart(uint16_t part);   // libère tout, charge palette + code + polygones
void AwRes_Request(uint16_t num);      // instruction 0x19 (chargement ou changement de partie)
void AwRes_FreeVolatile(void);         // libère sons, musiques (instruction 0x19 avec 0)
const char *AwRes_LastError(void);

// décompression « ByteKiller » (données lues depuis la fin) ; vrai si la somme de contrôle est bonne
bool AwUnpack(uint8_t *dst, uint32_t dstSize, const uint8_t *src, uint32_t srcSize);
