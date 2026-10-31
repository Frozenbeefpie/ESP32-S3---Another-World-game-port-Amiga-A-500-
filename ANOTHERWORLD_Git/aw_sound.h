// ANOTHERWORLD — son : mixeur 4 voies (échantillons 8 bits signés), lecteur de musique, sortie I2S PDM.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#pragma once
#include <stdint.h>

bool AwSnd_Init(void);
void AwSnd_PlaySound(uint16_t res, uint8_t freqIdx, uint8_t vol, uint8_t channel);   // instruction 0x18
void AwSnd_PlayMusic(uint16_t res, uint16_t delay, uint8_t pos);                     // 0x1A
void AwSnd_StopAll(void);                                                            // sons + musique
void AwSnd_SetMute(bool on);
bool AwSnd_Mute(void);
void AwSnd_SetInterp(bool on);                              // F4 : interpolation linéaire des échantillons
bool AwSnd_Interp(void);
