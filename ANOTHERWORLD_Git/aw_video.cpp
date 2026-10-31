// ANOTHERWORLD — vidéo : 4 pages 320x200 en 16 couleurs, polygones, palettes, sortie VGA 6 bits.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
//
// Pages : un octet par pixel (couleur 0..15). Trois « pointeurs » comme dans le moteur d'origine :
// page de dessin, page affichée (0xFE) et page de travail suivante (0xFF), échangées par l'affichage.
// Couleurs de tracé : 0..15 = couleur pleine, 0x10 = surbrillance (pixel | 8), > 0x10 = recopie de la
// page 0 (fond). Le VGA de la carte n'a que 2 bits par composante (64 couleurs, comme le CoCo 3) : les
// palettes 12 bits du jeu sont réduites, avec un tramage 2x2 facultatif (touche F2).
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include "aw_config.h"
#include "aw_video.h"
#include "aw_res.h"
#include "VGA.h"
#include "glcdfont.h"                 // police 5x7 d'Adafruit_GFX (licence BSD)
#include <esp_intr_alloc.h>
#include <soc/lcd_cam_struct.h>
#include <soc/interrupts.h>

#define VGA_X0 3                      // la ligne VGA fait 326 pixels (mode 4:3 du CoCo3-PRO)
#define VGA_Y0 40                     // 2 x 200 lignes centrées dans 480 (chaque ligne du jeu = 2 lignes VGA)

static VGA     *s_vga = nullptr;
static uint8_t *s_page[4];
static uint8_t  s_draw = 0, s_front = 2, s_back = 1;   // indices de page
static uint8_t  s_palReq = 0xFF;
static uint16_t s_pal[16];                             // 0x0RGB
static uint8_t  s_lut[16][16];                         // [phase de trame (y & 3) * 4 + (x & 3)][couleur] -> octet VGA
static uint8_t  s_colorMode = AW_COLOR_CHECKER_FRC;
static void StartFlip(void);
static float    s_gamma = 1.0f;                        // luminosité : composante Amiga ^ s_gamma (touches - et =)

// ---------------------------------------------------------------------------------------------------
// palette -> octets VGA (bits 7-6 rouge, 4-3 vert, 1-0 bleu, comme gime_video.cpp), 4 niveaux par composante.
// OKLAB : pour chaque couleur 12 bits, la couleur VGA seule ou le damier de deux couleurs VGA dont le
// mélange (moyenne en lumière linéaire) est le plus proche dans l'espace perceptuel OKLab, plus 0,3 x
// l'écart entre les deux (un damier trop contrasté se voit). Choix validé sur des vidages réels de l'intro.
// OKLAB_NEAREST : couleur VGA seule la plus proche en OKLab. RAW : arrondi de chaque composante.
static inline uint8_t Vga(int r, int g, int b) { return (uint8_t)(b | (g << 3) | (r << 6)); }

static inline float Lin(float v) { return powf(v, 2.2f); }

static void OkLab(float r, float g, float b, float *o)
{
  const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
  const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
  const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
  o[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  o[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  o[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

struct AwPair { float lab[3]; float contrast; uint8_t a, b; };   // a, b : indices 0..63 (r*16 + g*4 + b)
static AwPair *s_pairs = nullptr;
static int     s_pairCount = 0;
static uint8_t s_vgaOf[64];

static bool InitPairs(void)
{
  s_pairs = (AwPair *)heap_caps_malloc(64 * 65 / 2 * sizeof(AwPair), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_pairs) s_pairs = (AwPair *)heap_caps_malloc(64 * 65 / 2 * sizeof(AwPair), MALLOC_CAP_SPIRAM);
  if (!s_pairs) return false;
  float lin[64][3], lab[64][3];
  for (int c = 0; c < 64; c++)
  {
    const int r = c >> 4, g = (c >> 2) & 3, b = c & 3;
    s_vgaOf[c] = Vga(r, g, b);
    lin[c][0] = Lin(r / 3.0f); lin[c][1] = Lin(g / 3.0f); lin[c][2] = Lin(b / 3.0f);
    OkLab(lin[c][0], lin[c][1], lin[c][2], lab[c]);
  }
  s_pairCount = 0;
  for (int i = 0; i < 64; i++)
    for (int j = i; j < 64; j++)
    {
      AwPair &p = s_pairs[s_pairCount++];
      OkLab((lin[i][0] + lin[j][0]) / 2, (lin[i][1] + lin[j][1]) / 2, (lin[i][2] + lin[j][2]) / 2, p.lab);
      const float d0 = lab[i][0] - lab[j][0], d1 = lab[i][1] - lab[j][1], d2 = lab[i][2] - lab[j][2];
      p.contrast = sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
      p.a = (uint8_t)i; p.b = (uint8_t)j;
    }
  return true;
}

// BAYER : tramage ordonné 4x4 par composante, en lumière linéaire (niveaux VGA 0, 1/3, 2/3, 1 linéarisés
// gamma 2,2). Chaque bloc 4x4 mélange les deux niveaux qui encadrent la composante : les 16 niveaux de
// l'Amiga sont rendus en moyenne, teinte comprise (marrons, gris-bleu), au prix d'une fine texture.
// CHECKER : même principe, mais seulement 3 états par intervalle : niveau bas, damier 50 % ou niveau haut.
// Le damier (période 2 pixels) est le motif le plus fin ; les couleurs proches d'un niveau VGA restent unies.
// composante Amiga 0..15 -> 0..1, avec la correction de luminosité (> 1 : plus sombre, comme un moniteur CRT)
static inline float Chan(int v4) { return powf(v4 / 15.0f, s_gamma); }

static uint8_t BayerLevel(int v4, int ph, bool checker)
{
  static const uint8_t k_b4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
  const float thr = checker ? ((((ph >> 2) ^ ph) & 1) ? 0.75f : 0.25f) : (k_b4[ph] + 0.5f) / 16.0f;
  const float v = Lin(Chan(v4));
  for (int k = 0; k < 3; k++)
  {
    const float lo = Lin(k / 3.0f), hi = Lin((k + 1) / 3.0f);
    if (v <= hi) return (uint8_t)(k + ((v - lo) / (hi - lo) > thr ? 1 : 0));
  }
  return 3;
}

static void BuildLut(void)
{
  if (s_colorMode == AW_COLOR_RAW)
  {
    for (int i = 0; i < 16; i++)
    {
      const uint16_t c = s_pal[i];
      const uint8_t v = Vga((int)lroundf(Chan((c >> 8) & 15) * 3), (int)lroundf(Chan((c >> 4) & 15) * 3), (int)lroundf(Chan(c & 15) * 3));
      for (int ph = 0; ph < 16; ph++) s_lut[ph][i] = v;
    }
    return;
  }
  if (s_colorMode <= AW_COLOR_BAYER)
  {
    const bool ck = s_colorMode != AW_COLOR_BAYER;
    for (int ph = 0; ph < 16; ph++)
      for (int i = 0; i < 16; i++)
      {
        const uint16_t c = s_pal[i];
        s_lut[ph][i] = Vga(BayerLevel((c >> 8) & 15, ph, ck), BayerLevel((c >> 4) & 15, ph, ck), BayerLevel(c & 15, ph, ck));
      }
    return;
  }
  if (!s_pairs && !InitPairs()) { s_colorMode = AW_COLOR_RAW; BuildLut(); return; }
  const float lam = (s_colorMode == AW_COLOR_OKLAB) ? 0.3f : 0.0f;
  for (int i = 0; i < 16; i++)
  {
    float t[3];
    OkLab(Lin(Chan((s_pal[i] >> 8) & 15)), Lin(Chan((s_pal[i] >> 4) & 15)), Lin(Chan(s_pal[i] & 15)), t);
    float best = 1e9f;
    int bi = 0;
    for (int k = 0; k < s_pairCount; k++)
    {
      const AwPair &p = s_pairs[k];
      if (lam == 0.0f && p.a != p.b) continue;         // OKLAB_NEAREST : couleurs seules
      const float d0 = p.lab[0] - t[0], d1 = p.lab[1] - t[1], d2 = p.lab[2] - t[2];
      const float cost = sqrtf(d0 * d0 + d1 * d1 + d2 * d2) + lam * p.contrast;
      if (cost < best) { best = cost; bi = k; }
    }
    const uint8_t a = s_vgaOf[s_pairs[bi].a], b = s_vgaOf[s_pairs[bi].b];
    for (int ph = 0; ph < 16; ph++) s_lut[ph][i] = (((ph >> 2) ^ ph) & 1) ? b : a;   // damier
  }
}

static void ApplyPalette(uint8_t num)
{
  if (!g_awSegPal || num >= 32) return;
  const uint8_t *p = g_awSegPal + num * 32;
  for (int i = 0; i < 16; i++) s_pal[i] = (uint16_t)(((p[2 * i] & 0x0F) << 8) | p[2 * i + 1]);
  BuildLut();
}

// appelée depuis la tâche USB : le tableau est reconstruit au prochain affichage (cœur 1)
static volatile bool s_lutDirty = false;
void AwVid_CycleColorMode(void) { s_colorMode = (uint8_t)((s_colorMode + 1) % AW_COLOR_COUNT); s_lutDirty = true; }
void AwVid_AdjustGamma(int d)
{
  float g = s_gamma + d * 0.05f;
  if (g < 0.60f) g = 0.60f; else if (g > 2.00f) g = 2.00f;
  s_gamma = g;
  s_lutDirty = true;
  AW_LOG("[VID] gamma %.2f\n", g);
}
const char *AwVid_ColorModeName(void)
{
  static const char *const k_names[AW_COLOR_COUNT] = { "checker frc", "checker", "bayer 4x4", "oklab", "oklab nearest", "raw" };
  return k_names[s_colorMode];
}

// ---------------------------------------------------------------------------------------------------
static void ClearVga(uint8_t v)
{
  for (int b = 0; b < s_vga->bufferCount; b++)
  {
    for (int y = 0; y < (int)s_vga->mode.vRes; y++) memset(s_vga->dmaBuffer->getLineAddr8(y, b), v, s_vga->mode.hRes);
    s_vga->dmaBuffer->flush(b);
  }
}

bool AwVid_Init(VGA *vga)
{
  s_vga = vga;
  for (int i = 0; i < 4; i++)
  {
    s_page[i] = (uint8_t *)heap_caps_malloc(AW_PAGE_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_page[i]) return false;
    memset(s_page[i], 0, AW_PAGE_SIZE);
  }
  for (int i = 0; i < 16; i++) s_pal[i] = (uint16_t)(i * 0x111);   // gris en attendant la 1re palette
  BuildLut();
  ClearVga(0);
  StartFlip();
  AwVid_ResetPages();
  return true;
}

static uint8_t PageIndex(uint8_t id)
{
  if (id <= 3) return id;
  if (id == 0xFE) return s_front;
  if (id == 0xFF) return s_back;
  return 0;
}

void AwVid_ResetPages(void)
{
  s_back = 1;
  s_front = 2;
  s_draw = PageIndex(0xFE);
}

void AwVid_SelectPage(uint8_t id) { s_draw = PageIndex(id); }

void AwVid_FillPage(uint8_t id, uint8_t color) { memset(s_page[PageIndex(id)], color & 15, AW_PAGE_SIZE); }

void AwVid_CopyPage(uint8_t src, uint8_t dst, int16_t vscroll)
{
  if (src == dst) return;
  if (src >= 0xFE || !(src & 0x80))                    // copie simple
  {
    // 0xFE / 0xFF désignent la page affichée / suivante : ne pas leur appliquer le masque du bit 6
    const uint8_t s = PageIndex(src >= 0xFE ? src : (src & 0xBF)), d = PageIndex(dst);
    if (s != d) memcpy(s_page[d], s_page[s], AW_PAGE_SIZE);
    return;
  }
  const uint8_t s = PageIndex(src & 3), d = PageIndex(dst);   // copie avec défilement vertical
  if (s == d || vscroll < -199 || vscroll > 199) return;
  const uint8_t *from = s_page[s];
  uint8_t *to = s_page[d];
  int h = AW_H;
  if (vscroll < 0) { h += vscroll; from -= vscroll * AW_W; }
  else             { h -= vscroll; to += vscroll * AW_W; }
  memmove(to, from, (size_t)h * AW_W);
}

void AwVid_RequestPalette(uint8_t num) { s_palReq = num; }

// Tampons VGA : 2 paires (0-1 et 2-3). La paire affichée alterne ses deux tampons à chaque synchro verticale
// (interruption LCD_CAM) ; une nouvelle image est écrite dans l'autre paire, puis les paires sont échangées.
// CHECKER_FRC : le second tampon porte le damier inversé, chaque pixel alterne entre les deux niveaux à 60 Hz
// (comme le FRC des dalles LCD 6 bits) : la teinte moyenne reste, le motif fixe disparaît. Autres modes :
// deux tampons identiques.
static volatile uint8_t s_pair = 0, s_sub = 0;
static volatile uint32_t s_vsyncs = 0;
static bool s_noFlip = false;                          // interruption absente : bascule directe, sans FRC

static void VsyncIsr(void *)
{
  LCD_CAM.lc_dma_int_clr.val = LCD_CAM.lc_dma_int_st.val;
  s_vsyncs++;
  s_sub ^= 1;
  s_vga->dmaBuffer->attachBuffer((s_pair << 1) | s_sub);
}

static void StartFlip(void)
{
  intr_handle_t h;
  if (esp_intr_alloc(ETS_LCD_CAM_INTR_SOURCE, 0, VsyncIsr, nullptr, &h) != ESP_OK) { s_noFlip = true; AW_LOG("[VID] vsync interrupt failed\n"); return; }
  LCD_CAM.lc_dma_int_clr.val = 0xFFFFFFFF;
  LCD_CAM.lc_dma_int_ena.lcd_vsync_int_ena = 1;
  delay(60);
  if (s_vsyncs) AW_LOG("[VID] vsync flip on (%u in 60 ms)\n", (unsigned)s_vsyncs);
  else { s_noFlip = true; LCD_CAM.lc_dma_int_ena.lcd_vsync_int_ena = 0; AW_LOG("[VID] no vsync interrupt, plain flip\n"); }
}

static void Present(const uint8_t *page)
{
  const int b0 = (s_pair ^ 1) << 1;                    // paire cachée
  const int sh = s_colorMode == AW_COLOR_CHECKER_FRC ? 1 : 0;
  for (int yy = 0; yy < AW_H * 2; yy++)                // yy : ligne VGA, la trame suit yy (2 lignes par ligne du jeu)
  {
    const uint8_t *src = page + (yy >> 1) * AW_W;
    uint8_t *da = s_vga->dmaBuffer->getLineAddr8(VGA_Y0 + yy, b0) + VGA_X0;
    uint8_t *db = s_vga->dmaBuffer->getLineAddr8(VGA_Y0 + yy, b0 + 1) + VGA_X0;
    const uint8_t (*l)[16] = &s_lut[(yy & 3) << 2];    // 4 phases de la ligne (x & 3)
    const uint8_t *m0 = l[sh], *m1 = l[(1 + sh) & 3], *m2 = l[(2 + sh) & 3], *m3 = l[(3 + sh) & 3];
    for (int x = 0; x < AW_W; x += 4)
    {
      da[x]     = l[0][src[x]];     db[x]     = m0[src[x]];
      da[x + 1] = l[1][src[x + 1]]; db[x + 1] = m1[src[x + 1]];
      da[x + 2] = l[2][src[x + 2]]; db[x + 2] = m2[src[x + 2]];
      da[x + 3] = l[3][src[x + 3]]; db[x + 3] = m3[src[x + 3]];
    }
  }
  s_vga->dmaBuffer->flush(b0);                         // vidage du cache PSRAM
  s_vga->dmaBuffer->flush(b0 + 1);
  s_pair ^= 1;                                         // affichée dès la prochaine synchro verticale
  if (s_noFlip) s_vga->dmaBuffer->attachBuffer(b0);
}

// longueur de ligne VGA (pixels, synchro et paliers compris), réglable en direct : touches [ et ]
void AwVid_AdjustHTotal(int d)
{
  const uint32_t v = LCD_CAM.lcd_ctrl1.lcd_ht_width + d;
  LCD_CAM.lcd_ctrl1.lcd_ht_width = v;
  LCD_CAM.lcd_user.lcd_update = 1;
  AW_LOG("[VGA] ht_width %u (base %u)\n", (unsigned)v, (unsigned)s_vga->mode.totalHorizontal());
}

void AwVid_Show(uint8_t id)
{
  if (id != 0xFE)
  {
    if (id == 0xFF) { const uint8_t t = s_front; s_front = s_back; s_back = t; }
    else s_front = PageIndex(id);
  }
  if (s_palReq != 0xFF) { ApplyPalette(s_palReq); s_palReq = 0xFF; }
  else if (s_lutDirty) { BuildLut(); AW_LOG("[VID] colors: %s\n", AwVid_ColorModeName()); }
  s_lutDirty = false;
  Present(s_page[s_front]);
}

// ---------------------------------------------------------------------------------------------------
// tracé : une ligne horizontale ou un point dans la page de dessin
static void Span(int y, int xa, int xb, uint8_t color)
{
  if (xa > xb) { const int t = xa; xa = xb; xb = t; }
  if (xa > AW_W - 1 || xb < 0) return;
  if (xa < 0) xa = 0;
  if (xb > AW_W - 1) xb = AW_W - 1;
  uint8_t *p = s_page[s_draw] + y * AW_W + xa;
  const int n = xb - xa + 1;
  if (color < 0x10) memset(p, color, n);
  else if (color == 0x10) { for (int i = 0; i < n; i++) p[i] = (p[i] & 7) | 8; }
  else if (s_draw != 0) memcpy(p, s_page[0] + y * AW_W + xa, n);
}

static void Plot(int x, int y, uint8_t color)
{
  if (x < 0 || x >= AW_W || y < 0 || y >= AW_H) return;
  Span(y, x, x, color);
}

// pente en 16.16 comme le moteur d'origine (inverse 0x4000/dy tronqué), pour des contours identiques
static int32_t Slope(int dx, int dy)
{
  const int32_t inv = (dy > 0) ? 0x4000 / dy : 0x4000;
  return dx * inv * 4;
}

#define AW_MAX_POINTS 64

// Polygone convexe : la liste des sommets décrit le bord droit (de 0 vers le milieu) puis le bord gauche
// (de la fin vers le milieu) ; les deux bords avancent ensemble, une paire de sommets par segment.
static void FillPolygon(const uint8_t *p, uint8_t color, uint16_t zoom, int cx, int cy)
{
  const int bbw = p[0] * zoom / 64, bbh = p[1] * zoom / 64, n = p[2];
  p += 3;
  if (n < 2 || n > AW_MAX_POINTS) return;
  int16_t px[AW_MAX_POINTS], py[AW_MAX_POINTS];
  for (int i = 0; i < n; i++) { px[i] = (int16_t)(p[2 * i] * zoom / 64); py[i] = (int16_t)(p[2 * i + 1] * zoom / 64); }

  if (bbw == 0 && bbh == 1 && n == 4) { Plot(cx, cy, color); return; }

  const int x0 = cx - bbw / 2, y0 = cy - bbh / 2;
  if (x0 > AW_W - 1 || cx + bbw / 2 < 0 || y0 > AW_H - 1 || cy + bbh / 2 < 0) return;

  int i = 0, j = n - 1;
  uint32_t ea = (uint32_t)(px[j] + x0) << 16;          // bord parcouru à reculons
  uint32_t eb = (uint32_t)(px[i] + x0) << 16;          // bord parcouru en avant
  i++; j--;
  int y = y0;
  for (int left = n - 2; left > 0; left -= 2)
  {
    const int32_t sa = Slope(px[j] - px[j + 1], py[j] - py[j + 1]);
    const int h = py[i] - py[i - 1];
    const int32_t sb = Slope(px[i] - px[i - 1], h);
    i++; j--;
    ea = (ea & 0xFFFF0000u) | 0x7FFF;
    eb = (eb & 0xFFFF0000u) | 0x8000;
    if (h <= 0) { ea += sa; eb += sb; continue; }
    for (int k = h; k > 0; k--)
    {
      if (y >= 0) Span(y, (int16_t)(ea >> 16), (int16_t)(eb >> 16), color);
      ea += sa; eb += sb;
      if (++y > AW_H - 1) return;
    }
  }
}

static const uint8_t *s_shapeSeg = nullptr;

static void DrawShapeAt(const uint8_t *p, uint8_t color, uint16_t zoom, int x, int y, int depth);

// groupe de formes : origine relative, puis enfants (adresse/2, position, couleur facultative)
static void DrawGroup(const uint8_t *p, uint16_t zoom, int x, int y, int depth)
{
  x -= p[0] * zoom / 64;
  y -= p[1] * zoom / 64;
  const int n = p[2];
  p += 3;
  for (int k = 0; k <= n; k++)
  {
    const uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    const int ox = x + p[2] * zoom / 64, oy = y + p[3] * zoom / 64;
    p += 4;
    uint8_t col = 0xFF;
    if (w & 0x8000) { col = p[0] & 0x7F; p += 2; }
    DrawShapeAt(s_shapeSeg + (uint32_t)(w & 0x7FFF) * 2, col, zoom, ox, oy, depth + 1);
  }
}

static void DrawShapeAt(const uint8_t *p, uint8_t color, uint16_t zoom, int x, int y, int depth)
{
  if (depth > 16) return;
  const uint8_t h = *p++;
  if (h >= 0xC0)
  {
    if (color & 0x80) color = h & 0x3F;                // couleur prise dans la forme
    FillPolygon(p, color, zoom, x, y);
  }
  else if ((h & 0x3F) == 2) DrawGroup(p, zoom, x, y, depth);
}

void AwVid_DrawShape(const uint8_t *seg, uint16_t off, uint16_t zoom, int16_t x, int16_t y)
{
  if (!seg) return;
  s_shapeSeg = seg;
  DrawShapeAt(seg + off, 0xFF, zoom, x, y, 0);
}

// ---------------------------------------------------------------------------------------------------
// image plein écran : 4 plans de 8000 octets (40 octets par ligne), plan 0 = bit de poids faible
void AwVid_LoadBitmap(const uint8_t *src, size_t size)
{
  if (size < 32000) return;
  uint8_t *dst = s_page[0];
  for (int k = 0; k < 8000; k++)
  {
    const uint8_t b0 = src[k], b1 = src[8000 + k], b2 = src[16000 + k], b3 = src[24000 + k];
    for (int bit = 7; bit >= 0; bit--)
      *dst++ = (uint8_t)(((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1) | (((b2 >> bit) & 1) << 2) | (((b3 >> bit) & 1) << 3));
  }
}

// ---------------------------------------------------------------------------------------------------
// Texte. Les chaînes du jeu font partie de son exécutable et ne sont pas fournies : on les lit, si
// présentes, dans /AW/STRINGS.TXT, une par ligne « id texte » (id en hexadécimal, \n = saut de ligne).
#define AW_MAX_STRINGS 256
struct AwString { uint16_t id; char *txt; };
static AwString s_str[AW_MAX_STRINGS];
static int s_strCount = 0;

void AwVid_LoadStrings(void)
{
  File f = SD_MMC.open(AW_DIR "/STRINGS.TXT", FILE_READ);
  if (!f) { AW_LOG("[VID] no %s/STRINGS.TXT (game texts will be blank)\n", AW_DIR); return; }
  char line[640];
  while (s_strCount < AW_MAX_STRINGS && f.available())
  {
    const size_t n = f.readBytesUntil('\n', line, sizeof line - 1);
    line[n] = 0;
    char *end;
    const long id = strtol(line, &end, 16);
    if (end == line || id < 0 || id > 0xFFFF) continue;
    if (*end == '=') end++;                            // « id=texte » : les espaces du texte sont gardés
    else while (*end == ' ' || *end == '\t') end++;    // « id texte »
    char *txt = (char *)malloc(strlen(end) + 1);
    if (!txt) break;
    char *o = txt;
    for (const char *s = end; *s && *s != '\r'; s++)
    {
      if (s[0] == '\\' && s[1] == 'n') { *o++ = '\n'; s++; }
      else *o++ = *s;
    }
    *o = 0;
    s_str[s_strCount].id = (uint16_t)id;
    s_str[s_strCount].txt = txt;
    s_strCount++;
  }
  f.close();
  AW_LOG("[VID] %d strings loaded\n", s_strCount);
}

static void DrawChar(uint8_t *page, int x, int y, uint8_t c, uint8_t color)
{
  for (int col = 0; col < 5; col++)
  {
    const uint8_t bits = font[c * 5 + col];
    for (int row = 0; row < 8; row++)
    {
      const int px = x + 1 + col, py = y + row;
      if ((bits >> row) & 1 && px >= 0 && px < AW_W && py >= 0 && py < AW_H) page[py * AW_W + px] = color;
    }
  }
}

volatile bool g_awTraceStr = false;

// diagnostic (touche série g) : les 4 pages, la palette et le tableau de sortie, en hexadécimal
void AwVid_DumpPages(void)
{
  AW_LOG("[SHOT] BEGIN part=%u draw=%u front=%u back=%u colors=%s\n", g_awCurPart, s_draw, s_front, s_back, AwVid_ColorModeName());
  AW_LOG("[SHOT] PAL");
  for (int i = 0; i < 16; i++) AW_LOG(" %03X", s_pal[i]);
  AW_LOG("\n[SHOT] LUT");
  for (int i = 0; i < 16; i++) AW_LOG(" %02X", s_lut[0][i]);
  AW_LOG("\n");
  static const char k_hex[] = "0123456789ABCDEF";
  char line[AW_W + 1];
  for (int p = 0; p < 4; p++)
    for (int y = 0; y < AW_H; y++)
    {
      const uint8_t *src = s_page[p] + y * AW_W;
      for (int x = 0; x < AW_W; x++) line[x] = k_hex[src[x] & 15];
      line[AW_W] = 0;
      AW_LOG("[SHOT] P%d %03d %s\n", p, y, line);
    }
  AW_LOG("[SHOT] END\n");
}

// x en colonnes de 8 pixels, y en pixels
void AwVid_DrawString(uint8_t color, uint16_t x, uint16_t y, uint16_t id)
{
  const char *txt = nullptr;
  for (int i = 0; i < s_strCount; i++) if (s_str[i].id == id) { txt = s_str[i].txt; break; }
  if (g_awTraceStr) AW_LOG("[STR] id=0x%03X x=%u y=%u color=0x%02X page=%u len=%d\n", id, x, y, color, s_draw, txt ? (int)strlen(txt) : -1);
  if (!txt)
  {
    static uint32_t s_warned[0x400 / 32];              // un seul message par texte manquant
    if (id < 0x400 && !(s_warned[id >> 5] & (1u << (id & 31))))
    {
      s_warned[id >> 5] |= 1u << (id & 31);
      AW_LOG("[VID] string 0x%03X missing\n", id);
    }
    return;
  }
  const uint16_t x0 = x;
  for (const char *s = txt; *s; s++)
  {
    if (*s == '\n') { y += 8; x = x0; continue; }
    DrawChar(s_page[s_draw], x * 8, y, (uint8_t)*s, color & 15);
    x++;
  }
}

// ---------------------------------------------------------------------------------------------------
void AwVid_Message(const char *const *lines, int n)
{
  if (!s_vga) return;
  const uint8_t bg = 0x02, fg = 0xDB;                  // bleu, blanc
  for (int b = 0; b < s_vga->bufferCount; b++)         // tous les tampons : l'alternance reste invisible
  {
    for (int y = 0; y < (int)s_vga->mode.vRes; y++) memset(s_vga->dmaBuffer->getLineAddr8(y, b), bg, s_vga->mode.hRes);
    for (int l = 0; l < n; l++)
    {
      const int y0 = 30 + l * 12;
      int x0 = VGA_X0 + 12;
      for (const char *s = lines[l]; *s && x0 < (int)s_vga->mode.hRes - 6; s++, x0 += 6)
        for (int col = 0; col < 5; col++)
        {
          const uint8_t bits = font[(uint8_t)*s * 5 + col];
          for (int row = 0; row < 8; row++)
            if ((bits >> row) & 1 && 2 * (y0 + row) + 1 < (int)s_vga->mode.vRes)   // lignes doublées (480)
            {
              s_vga->dmaBuffer->getLineAddr8(2 * (y0 + row), b)[x0 + col] = fg;
              s_vga->dmaBuffer->getLineAddr8(2 * (y0 + row) + 1, b)[x0 + col] = fg;
            }
        }
    }
    s_vga->dmaBuffer->flush(b);
  }
}
