// ANOTHERWORLD — lecteur natif du moteur Another World sur ESP32-S3 (carte ESP32-CoCo3-PRO).
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
// Voir aw_config.h, README.md et LISEZMOI.TXT.
//
// Cœur 1 : machine virtuelle + tracé (loop). Cœur 0 : tâche audio, hôte USB. Le VGA est en DMA.
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include "aw_config.h"
#include "aw_res.h"
#include "aw_video.h"
#include "aw_sound.h"
#include "aw_vm.h"
#include "aw_input.h"
#include "VGA.h"
#include "Mode.h"
#include "PinConfig.h"

// VGA 6 bits de la carte : B B X G G X R R (mêmes broches que l'émulateur)
static const PinConfig k_pins(-1, -1, 8, 9, -1,  -1, -1, -1, 6, 7, -1,  -1, -1, -1, 4, 5,  2, 1);
// mode 320x240 4:3 du CoCo3-PRO (MODE_COCO_320x240 de main.cpp) : 326 pixels utiles, mais 480 lignes réelles
// (même synchro) : chaque ligne du jeu occupe 2 lignes VGA tramées séparément, motif deux fois plus fin
// palier avant 9 (au lieu de 8) : ligne de 400 points comme le 640x480 standard (registre ht_width = 399),
// sinon l'écran LCD échantillonne en dérivant et le damier forme des barres verticales (réglé avec [ et ])
static const Mode k_mode(9, 40, 24, 320 + 6, 16, 2, 29, 480, 12587500, 0, 0, 1);

static VGA *s_vga = nullptr;
static bool s_running = false;

static void Halt(const char *const *lines, int n)
{
  for (int i = 0; i < n; i++) AW_LOG("[AW] %s\n", lines[i]);
  AwVid_Message(lines, n);
  for (;;) delay(1000);
}

void setup()
{
  Serial.begin(921600);
  delay(200);
  AW_LOG("\n[AW] ANOTHERWORLD %s - Another World engine player for ESP32-CoCo3-PRO\n", AW_VERSION);
  AW_LOG("[AW] PSRAM %u bytes free, internal %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  s_vga = new VGA();
  s_vga->usePsram = true;
  s_vga->bufferCount = 4;                              // 2 paires de tampons (tramage temporel, aw_video.cpp)
  if (!s_vga->init(k_pins, k_mode, 8)) { AW_LOG("[AW] VGA init failed\n"); for (;;) delay(1000); }
  s_vga->start();
  if (!AwVid_Init(s_vga)) { AW_LOG("[AW] not enough PSRAM for video pages\n"); for (;;) delay(1000); }

  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 12) || SD_MMC.cardType() == CARD_NONE)
  {
    static const char *const msg[] = { "ANOTHERWORLD " AW_VERSION, "", "No SD card.", "",
                                       "Put the DOS game files in /AW :", "MEMLIST.BIN, BANK01 ... BANK0D" };
    Halt(msg, 6);
  }
  if (!AwRes_Init())
  {
    // diagnostic : ce que la carte contient réellement (racine et /AW)
    static char buf[18][48];
    static const char *msg[18];
    int n = 0;
    snprintf(buf[n++], 48, "ANOTHERWORLD " AW_VERSION);
    snprintf(buf[n++], 48, "%.47s", AwRes_LastError());
    snprintf(buf[n++], 48, "Need /AW/MEMLIST.BIN + BANK01..0D");
    const char *dirs[2] = { "/", AW_DIR };
    for (int d = 0; d < 2 && n < 18; d++)
    {
      snprintf(buf[n++], 48, "Content of %s :", dirs[d]);
      File dir = SD_MMC.open(dirs[d]);
      int shown = 0;
      for (File f = dir ? dir.openNextFile() : File(); f && n < 18; f = dir.openNextFile())
      {
        if (shown++ < 6) snprintf(buf[n++], 48, "  %.28s %s %u", f.name(), f.isDirectory() ? "<DIR>" : "", (unsigned)f.size());
        f.close();
      }
      if (!dir && n < 18) snprintf(buf[n++], 48, "  (folder not found)");
      else if (shown > 6 && n < 18) snprintf(buf[n++], 48, "  ... %d entries", shown);
      if (dir) dir.close();
    }
    for (int i = 0; i < n; i++) msg[i] = buf[i];
    Halt(msg, n);
  }
  AwVid_LoadStrings();

  AwSnd_Init();
  AwIn_Init();
  AwVm_Init();
  if (!AwVm_StartPart(AW_PART_INTRO))
  {
    static char err[96];
    snprintf(err, sizeof err, "%s", AwRes_LastError());
    static const char *const msg[] = { "ANOTHERWORLD " AW_VERSION, "", "Cannot load the introduction:", err };
    Halt(msg, 4);
  }
  s_running = true;
  AW_LOG("[AW] running. Keys: arrows, Space = action, P = pause, C = code, F2 = colors, F3 = mute, F5-F11 = parts\n");
}

void loop()
{
  if (!s_running) { delay(100); return; }
  if (!AwVm_RunFrame())
  {
    static char err[96];
    snprintf(err, sizeof err, "%s", AwRes_LastError());
    static const char *const msg[] = { "ANOTHERWORLD " AW_VERSION, "", "Cannot load the next part:", err };
    s_running = false;
    Halt(msg, 4);
  }
}
