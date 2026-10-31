// ANOTHERWORLD — son : mixeur 4 voies (échantillons 8 bits signés), lecteur de musique, sortie I2S PDM.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
//
// Ressource son : u16 longueur (mots), u16 longueur de boucle (mots), 4 octets, puis les données. La
// boucle, si présente, suit la partie jouée une fois. Voies 0 et 3 à gauche, 1 et 2 à droite (Amiga),
// avec un peu de diaphonie pour le casque. Le séquenceur de musique avance dans la tâche audio, compté en
// échantillons ; la marque de synchronisation (note 0xFFFD) est écrite dans la variable 0xF4 du bytecode.
#include <Arduino.h>
#include <math.h>
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aw_config.h"
#include "aw_sound.h"
#include "aw_res.h"
#include "aw_vm.h"

struct AwVoice
{
  const int8_t *data;
  uint32_t len, loopPos, loopLen;    // en octets
  uint32_t pos, inc;                 // 16.16
  uint8_t  vol;                      // 0..63
  bool     on;
};

struct AwInstr { const uint8_t *data; uint16_t vol; };

struct AwMusic
{
  bool     on;
  const uint8_t *patterns;
  uint8_t  order[0x80];
  uint16_t numOrder, curOrder, row;  // row : position dans le motif (octets, pas de 16)
  uint32_t tickSamples, countdown;
  AwInstr  instr[15];
};

static AwVoice  s_voice[4];
static AwMusic  s_mus;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static i2s_chan_handle_t s_tx = nullptr;
static bool     s_stereo = true;
static volatile bool s_mute = false;
static volatile bool s_interp = false;               // interpolation linéaire (touche F4) ; Amiga : sans
static uint16_t s_freq[40];

static inline uint16_t Be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void AwSnd_SetMute(bool on) { s_mute = on; }
bool AwSnd_Mute(void) { return s_mute; }
void AwSnd_SetInterp(bool on) { s_interp = on; }
bool AwSnd_Interp(void) { return s_interp; }

// ---------------------------------------------------------------------------------------------------
// voix (appelées sous s_mux)
static void VoiceStart(int ch, const uint8_t *res, uint32_t freq, uint8_t vol)
{
  AwVoice &v = s_voice[ch & 3];
  v.data    = (const int8_t *)(res + 8);
  v.len     = (uint32_t)Be16(res) * 2;
  v.loopLen = (uint32_t)Be16(res + 2) * 2;
  v.loopPos = v.loopLen ? v.len : 0;
  v.pos     = 0;
  v.inc     = (uint32_t)(((uint64_t)freq << 16) / AW_AUDIO_RATE);
  v.vol     = vol > 63 ? 63 : vol;
  v.on      = v.len + v.loopLen > 1;
}

static void VoicesOff(void) { for (int i = 0; i < 4; i++) s_voice[i].on = false; }

// ---------------------------------------------------------------------------------------------------
// séquenceur : 4 voies x 4 octets par ligne, motifs de 1024 octets (64 lignes)
static void MusicVoice(int ch, const uint8_t *d)
{
  const uint16_t note = Be16(d), arg = Be16(d + 2);
  if (note == 0xFFFD) { g_awVar[AW_VAR_MUS_MARK] = (int16_t)arg; return; }
  const uint8_t *smp = nullptr;
  uint8_t vol = 0;
  const int n = arg >> 12;
  if (n && s_mus.instr[n - 1].data)
  {
    smp = s_mus.instr[n - 1].data;
    int v = s_mus.instr[n - 1].vol;
    const int fx = (arg >> 8) & 15, amt = arg & 0xFF;
    if (fx == 5) v = v + amt > 63 ? 63 : v + amt;      // volume plus fort
    else if (fx == 6) v = v - amt < 0 ? 0 : v - amt;   // volume plus faible
    vol = (uint8_t)v;
    s_voice[ch].vol = vol;
  }
  if (note == 0xFFFE) s_voice[ch].on = false;
  else if (note != 0 && smp) VoiceStart(ch, smp, 7159092u / (note * 2u), vol);
}

static void MusicTick(void)
{
  const uint8_t *row = s_mus.patterns + (uint32_t)s_mus.order[s_mus.curOrder] * 1024 + s_mus.row;
  for (int ch = 0; ch < 4; ch++) MusicVoice(ch, row + ch * 4);
  s_mus.row += 16;
  if (s_mus.row >= 1024)
  {
    s_mus.row = 0;
    if (++s_mus.curOrder >= s_mus.numOrder) { s_mus.on = false; VoicesOff(); }
  }
}

static void MusicSetDelay(uint16_t d)
{
  uint32_t ms = (uint32_t)d * 60u / 7050u;
  if (ms == 0) ms = 1;
  s_mus.tickSamples = ms * AW_AUDIO_RATE / 1000u;
}

// ---------------------------------------------------------------------------------------------------
#define AW_CHUNK 128
static int16_t s_out[AW_CHUNK * 2];
static int32_t s_curL[AW_CHUNK], s_curR[AW_CHUNK];

// « Plancher » (même principe que la chaîne de sortie du CoCo3-PRO) : un PDM à 50 % de densité au
// repos s'entend comme un souffle. On ajoute donc au signal un décalage juste suffisant pour qu'il reste
// >= 0, et 0 correspond à une densité de 0 % (aucune commutation). Le décalage voit AW_FLOOR_LA blocs
// d'avance (sortie retardée de 8 x 128 échantillons, 21 ms) : il monte en rampe sur ~19 ms avant le début
// d'un son au lieu d'un saut de 2,7 ms (le saut donnait un « toc » qui masquait l'attaque), puis
// redescend lentement (0,4 s) pour ne pas s'entendre.
#define AW_FLOOR_REL 0.99145f        // exp(-128 / (0,4 s x 48000)) par bloc
#define AW_FLOOR_LA  8               // blocs d'avance
#define AW_FLOOR_UP  (32768.0f / (AW_FLOOR_LA - 1))   // montée maximale par bloc
struct AwFloor { int32_t hist[AW_FLOOR_LA + 1][AW_CHUNK]; int32_t need[AW_FLOOR_LA + 1]; int head; float off; };
static AwFloor s_flL, s_flR;

static void FloorRun(AwFloor &F, const int32_t *cur, int16_t *out, int stride)
{
  int32_t mn = 0;
  for (int i = 0; i < AW_CHUNK; i++) if (cur[i] < mn) mn = cur[i];
  memcpy(F.hist[F.head], cur, sizeof F.hist[0]);
  F.need[F.head] = -mn;
  F.head = (F.head + 1) % (AW_FLOOR_LA + 1);
  const int o = F.head;                                // bloc le plus ancien : celui qui sort maintenant
  float tgt = F.off * AW_FLOOR_REL - 48.0f;           // exponentielle + petite pente : atteint vraiment 0
  if (tgt < 0) tgt = 0;
  for (int j = 0; j <= AW_FLOOR_LA; j++)               // fin de rampe >= besoin des blocs à venir, moins la montée permise
  {
    const float lim = F.need[(o + j) % (AW_FLOOR_LA + 1)] - (j > 1 ? (j - 1) * AW_FLOOR_UP : 0.0f);
    if (tgt < lim) tgt = lim;
  }
  const float a = F.off, d = (tgt - a) / AW_CHUNK;
  const int32_t *src = F.hist[o];
  for (int i = 0; i < AW_CHUNK; i++)
  {
    int32_t v = src[i] + (int32_t)(a + d * (i + 1));
    if (v < 0) v = 0; else if (v > 65535) v = 65535;
    out[i * stride] = (int16_t)(v - 32768);
  }
  F.off = tgt;
}

static void Render(void)
{
  portENTER_CRITICAL(&s_mux);
  for (int f = 0; f < AW_CHUNK; f++)
  {
    if (s_mus.on)
    {
      if (s_mus.countdown == 0) { MusicTick(); s_mus.countdown = s_mus.tickSamples; }
      s_mus.countdown--;
    }
    int32_t mix[4] = { 0, 0, 0, 0 };
    for (int ch = 0; ch < 4; ch++)
    {
      AwVoice &v = s_voice[ch];
      if (!v.on) continue;
      uint32_t i = v.pos >> 16;
      const uint32_t end = v.loopLen ? v.loopPos + v.loopLen : v.len;
      if (i >= end)
      {
        if (!v.loopLen) { v.on = false; continue; }
        v.pos -= v.loopLen << 16;
        i = v.pos >> 16;
      }
      int32_t s = v.data[i];                           // échantillon tenu, comme le Paula de l'Amiga (son vif)
      if (s_interp)                                    // ou interpolation linéaire (plus doux, plus sourd)
      {
        uint32_t i1 = i + 1;
        if (i1 >= end) i1 = v.loopLen ? v.loopPos : i;
        s += ((v.data[i1] - s) * (int32_t)((v.pos >> 8) & 0xFF)) >> 8;
      }
      mix[ch] = s * v.vol;                             // -8128..8001
      v.pos += v.inc;
    }
    const int32_t l = mix[0] + mix[3], r = mix[1] + mix[2];
    s_curL[f] = s_mute ? 0 : (3 * l + r) / 2;          // ~ +-32000
    s_curR[f] = s_mute ? 0 : (3 * r + l) / 2;
  }
  portEXIT_CRITICAL(&s_mux);
  if (s_stereo) { FloorRun(s_flL, s_curL, s_out, 2); FloorRun(s_flR, s_curR, s_out + 1, 2); }
  else
  {
    for (int f = 0; f < AW_CHUNK; f++) s_curL[f] = (s_curL[f] + s_curR[f]) / 2;
    FloorRun(s_flL, s_curL, s_out, 1);
  }
}

static void AudioTask(void *)
{
  for (;;)
  {
    Render();
    size_t w;
    i2s_channel_write(s_tx, s_out, (s_stereo ? 2 : 1) * AW_CHUNK * sizeof(int16_t), &w, portMAX_DELAY);
  }
}

static bool PdmCreate(bool stereo)
{
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 4;
  cc.dma_frame_num = AW_CHUNK;
  if (i2s_new_channel(&cc, &s_tx, nullptr) != ESP_OK) { s_tx = nullptr; return false; }
  i2s_pdm_tx_config_t pc = {
#ifdef I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG
    .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AW_AUDIO_RATE),
    .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
#else
    .clk_cfg  = I2S_PDM_TX_CLK_DEFAULT_CONFIG(AW_AUDIO_RATE),
    .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
#endif
    .gpio_cfg = {
      .clk  = I2S_GPIO_UNUSED,
      .dout = (gpio_num_t)AW_AUDIO_L_GPIO,
#if SOC_I2S_PDM_MAX_TX_LINES > 1
      .dout2 = stereo ? (gpio_num_t)AW_AUDIO_R_GPIO : I2S_GPIO_UNUSED,
#endif
      .invert_flags = { .clk_inv = false },
    },
  };
#if SOC_I2S_HW_VERSION_2
  pc.slot_cfg.hp_en = false;                          // garder le continu : nécessaire au plancher
  if (stereo) pc.slot_cfg.line_mode = I2S_PDM_TX_TWO_LINE_DAC;   // gauche sur dout, droite sur dout2
#endif
  if (i2s_channel_init_pdm_tx_mode(s_tx, &pc) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK)
  {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }
  return true;
}

bool AwSnd_Init(void)
{
  // fréquences des échantillons d'effets : 40 demi-tons à partir de 3327 Hz
  for (int i = 0; i < 40; i++) s_freq[i] = (uint16_t)lroundf(3327.0f * powf(2.0f, i / 12.0f));
  memset(s_voice, 0, sizeof s_voice);
  memset(&s_mus, 0, sizeof s_mus);
  s_stereo = AW_AUDIO_STEREO && PdmCreate(true);
  if (!s_stereo && !PdmCreate(false)) { AW_LOG("[SND] I2S PDM init failed\n"); return false; }
  AW_LOG("[SND] I2S PDM %s, %u Hz\n", s_stereo ? "stereo (GPIO 47 L + 12 R)" : "mono (GPIO 47)", AW_AUDIO_RATE);
  xTaskCreatePinnedToCore(AudioTask, "awAudio", 4096, nullptr, 10, nullptr, 0);
  return true;
}

// ---------------------------------------------------------------------------------------------------
void AwSnd_PlaySound(uint16_t res, uint8_t freqIdx, uint8_t vol, uint8_t channel)
{
  if (res >= g_awMemCount) return;
  const AwMemEntry &e = g_awMem[res];
  if (e.state != AW_ST_LOADED || e.type != AW_RT_SOUND) return;
  portENTER_CRITICAL(&s_mux);
  if (vol == 0) s_voice[channel & 3].on = false;
  else VoiceStart(channel, e.buf, s_freq[freqIdx < 40 ? freqIdx : 39], vol);
  portEXIT_CRITICAL(&s_mux);
}

void AwSnd_StopAll(void)
{
  portENTER_CRITICAL(&s_mux);
  s_mus.on = false;
  VoicesOff();
  portEXIT_CRITICAL(&s_mux);
}

// module : u16 tempo, 15 x (u16 ressource, u16 volume), à 0x3E u16 nombre de positions, à 0x40 la table
// des motifs (128 octets), à 0xC0 les motifs
static void MusicLoad(uint16_t res, uint16_t delay, uint8_t pos)
{
  if (res >= g_awMemCount) return;
  const AwMemEntry &e = g_awMem[res];
  if (e.state != AW_ST_LOADED || e.type != AW_RT_MUSIC) return;
  const uint8_t *m = e.buf;
  AwInstr ins[15];
  for (int i = 0; i < 15; i++)
  {
    const uint16_t r = Be16(m + 2 + 4 * i);
    ins[i].data = nullptr;
    ins[i].vol = Be16(m + 4 + 4 * i);
    if (r && r < g_awMemCount && g_awMem[r].state == AW_ST_LOADED && g_awMem[r].type == AW_RT_SOUND)
    {
      ins[i].data = g_awMem[r].buf;
      memset(g_awMem[r].buf + 8, 0, 4);                // début d'échantillon muet (évite un clic)
    }
  }
  portENTER_CRITICAL(&s_mux);
  memcpy(s_mus.instr, ins, sizeof ins);
  memcpy(s_mus.order, m + 0x40, 0x80);
  s_mus.numOrder = Be16(m + 0x3E);
  s_mus.curOrder = pos;
  s_mus.row = 0;
  s_mus.patterns = m + 0xC0;
  MusicSetDelay(delay ? delay : Be16(m));
  s_mus.countdown = 0;
  s_mus.on = s_mus.numOrder > 0 && pos < s_mus.numOrder;
  portEXIT_CRITICAL(&s_mux);
}

void AwSnd_PlayMusic(uint16_t res, uint16_t delay, uint8_t pos)
{
  if (res) { MusicLoad(res, delay, pos); return; }
  portENTER_CRITICAL(&s_mux);
  if (delay) MusicSetDelay(delay);
  else { s_mus.on = false; VoicesOff(); }
  portEXIT_CRITICAL(&s_mux);
}
