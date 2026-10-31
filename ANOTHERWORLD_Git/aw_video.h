// ANOTHERWORLD — vidéo : 4 pages 320x200 en 16 couleurs, polygones, palettes, sortie VGA 6 bits.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#pragma once
#include <stdint.h>
#include <stddef.h>

class VGA;

bool AwVid_Init(VGA *vga);
void AwVid_SelectPage(uint8_t id);                          // instruction 0x0D (page de dessin)
void AwVid_FillPage(uint8_t id, uint8_t color);             // 0x0E
void AwVid_CopyPage(uint8_t src, uint8_t dst, int16_t vscroll);   // 0x0F
void AwVid_RequestPalette(uint8_t num);                     // 0x0B (appliquée à l'affichage suivant)
void AwVid_Show(uint8_t id);                                // 0x10 (après la pause du bytecode)
void AwVid_DrawShape(const uint8_t *seg, uint16_t off, uint16_t zoom, int16_t x, int16_t y);
void AwVid_DrawString(uint8_t color, uint16_t x, uint16_t y, uint16_t id);   // 0x12
void AwVid_LoadBitmap(const uint8_t *planar, size_t size);  // image 4 plans -> page 0
void AwVid_ResetPages(void);                                // état d'allumage des pointeurs de page
void AwVid_LoadStrings(void);                               // /AW/STRINGS.TXT (facultatif)

// rendu des couleurs (touche F2) : CHECKER_FRC = damier alterné à 60 Hz (défaut), CHECKER = tramage linéaire
// en damier, BAYER = tramage 4x4,
// OKLAB = couleur ou damier le plus
// proche en OKLab, NEAREST = couleur seule la plus proche en OKLab, RAW = arrondi simple
enum { AW_COLOR_CHECKER_FRC = 0, AW_COLOR_CHECKER, AW_COLOR_BAYER, AW_COLOR_OKLAB, AW_COLOR_OKLAB_NEAREST, AW_COLOR_RAW, AW_COLOR_COUNT };
void AwVid_CycleColorMode(void);
void AwVid_AdjustGamma(int d);                              // touches - et = : luminosité (pas de 0,05)
void AwVid_AdjustHTotal(int d);                             // touches [ et ] : longueur de ligne VGA

// diagnostic par le port série : g = vidage des 4 pages, t = trace des textes
void AwVid_DumpPages(void);
extern volatile bool g_awTraceStr;
const char *AwVid_ColorModeName(void);

// écran de messages (cartes SD absente, fichiers manquants…) : lignes de texte blanc sur bleu
void AwVid_Message(const char *const *lines, int n);
