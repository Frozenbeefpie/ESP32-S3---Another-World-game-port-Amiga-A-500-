// ANOTHERWORLD — lecteur natif du moteur Another World sur ESP32-S3 (carte ESP32-CoCo3-PRO).
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
// Moteur réécrit (machine virtuelle à bytecode, polygones, mixeur 4 voies) d'après la description publique
// du format des données. AUCUNE donnée du jeu n'est incluse : les fichiers originaux de la version DOS
// (MEMLIST.BIN, BANK01..BANK0D) se placent dans le dossier /AW de la carte SD.

#pragma once
#include <stdint.h>

#define AW_VERSION       "0.1.12"
#define AW_DIR           "/AW"

// écran du jeu : 320 x 200, 16 couleurs (un octet par pixel dans nos pages, valeurs 0..15)
#define AW_W             320
#define AW_H             200
#define AW_PAGE_SIZE     (AW_W * AW_H)

// carte SD (mêmes broches que l'ESP32-CoCo3-PRO, mode 1 bit)
#define SD_MMC_CMD       38
#define SD_MMC_CLK       39
#define SD_MMC_D0        40

// son : I2S PDM, gauche GPIO 47, droite GPIO 12 (comme audio.cpp du CoCo3-PRO). AW_AUDIO_STEREO 0 : mono
// sur GPIO 47 seul (4 voies mélangées à parts égales), pour une carte dont la voie droite n'est pas câblée
// (cas du circuit de test) ; 1 : stéréo sur 47 + 12.
#define AW_AUDIO_STEREO  0
#define AW_AUDIO_RATE    48000u
#define AW_AUDIO_L_GPIO  47
#define AW_AUDIO_R_GPIO  12

// parties du jeu : numéros de ressource >= 16000 demandés par le bytecode
#define AW_PART_FIRST    16000   // écran de protection (sauté)
#define AW_PART_INTRO    16001
#define AW_PART_LAST     16009   // écran du code d'accès

#define AW_LOG(...)      Serial.printf(__VA_ARGS__)
