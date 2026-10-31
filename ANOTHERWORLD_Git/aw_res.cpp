// ANOTHERWORLD — ressources : table MEMLIST.BIN, banques BANKxx, décompression, parties du jeu.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include "aw_config.h"
#include "aw_res.h"
#include "aw_video.h"

AwMemEntry g_awMem[AW_MAX_ENTRIES];
int        g_awMemCount = 0;

const uint8_t *g_awSegPal = nullptr, *g_awSegCode = nullptr, *g_awSegPoly = nullptr, *g_awSegPoly2 = nullptr;
uint32_t       g_awSegCodeSize = 0;
uint16_t       g_awCurPart = 0;
uint16_t       g_awNextPart = 0;
bool           g_awAmiga = false;

static char s_err[96] = "";
const char *AwRes_LastError(void) { return s_err; }

// numéros des ressources de chaque partie 16000..16009 : palette, bytecode, polygones, polygones communs
// (0 = aucun). Indices de la table MEMLIST.BIN, nécessaires pour lire les fichiers originaux.
static const uint8_t k_parts[10][4] =
{
  { 0x14, 0x15, 0x16, 0x00 },   // 16000 protection
  { 0x17, 0x18, 0x19, 0x00 },   // 16001 introduction
  { 0x1A, 0x1B, 0x1C, 0x11 },   // 16002
  { 0x1D, 0x1E, 0x1F, 0x11 },   // 16003
  { 0x20, 0x21, 0x22, 0x11 },   // 16004
  { 0x23, 0x24, 0x25, 0x00 },   // 16005
  { 0x26, 0x27, 0x28, 0x11 },   // 16006
  { 0x29, 0x2A, 0x2B, 0x11 },   // 16007
  { 0x7D, 0x7E, 0x7F, 0x00 },   // 16008 code d'accès
  { 0x7D, 0x7E, 0x7F, 0x00 },   // 16009 code d'accès
};

static inline uint32_t Be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// ---------------------------------------------------------------------------------------------------
// Décompression ByteKiller. Le flux de bits est lu à reculons (mots de 32 bits gros-boutistes, depuis la
// fin) et la sortie est écrite elle aussi de la fin vers le début. Fin du flux : taille, CRC, premier mot.
struct BkCtx
{
  const uint8_t *src, *srcMin;
  uint8_t *dst, *dstMin;
  uint32_t bits, crc;
  int32_t  left;
  bool     bad;
};

static int BkBit(BkCtx &c)
{
  int b = c.bits & 1;
  c.bits >>= 1;
  if (c.bits == 0)                                 // mot épuisé (seul restait le bit sentinelle)
  {
    if (c.src < c.srcMin) { c.bad = true; return 0; }
    const uint32_t w = Be32(c.src);
    c.src -= 4;
    c.crc ^= w;
    b = w & 1;
    c.bits = (w >> 1) | 0x80000000u;
  }
  return b;
}

static uint32_t BkBits(BkCtx &c, int n)
{
  uint32_t v = 0;
  while (n--) v = (v << 1) | (uint32_t)BkBit(c);
  return v;
}

static void BkLiteral(BkCtx &c, int nbits, int base)
{
  int32_t n = (int32_t)BkBits(c, nbits) + base + 1;
  if (n > c.left) n = c.left;
  c.left -= n;
  while (n--)
  {
    if (c.dst < c.dstMin) { c.bad = true; return; }
    *c.dst-- = (uint8_t)BkBits(c, 8);
  }
}

static void BkCopy(BkCtx &c, int nbits, int32_t n)
{
  if (n > c.left) n = c.left;
  c.left -= n;
  const uint32_t off = BkBits(c, nbits);
  while (n--)
  {
    if (c.dst < c.dstMin) { c.bad = true; return; }
    *c.dst = c.dst[off];
    c.dst--;
  }
}

bool AwUnpack(uint8_t *dst, uint32_t dstSize, const uint8_t *src, uint32_t srcSize)
{
  if (srcSize < 12) return false;
  BkCtx c;
  const uint8_t *p = src + srcSize - 4;
  const uint32_t outSize = Be32(p); p -= 4;
  if (outSize > dstSize) return false;
  c.crc = Be32(p); p -= 4;
  c.bits = Be32(p); p -= 4;
  c.crc ^= c.bits;
  c.src = p; c.srcMin = src;
  c.dst = dst + outSize - 1; c.dstMin = dst;
  c.left = (int32_t)outSize;
  c.bad = false;
  while (c.left > 0 && !c.bad)
  {
    if (!BkBit(c))
    {
      if (!BkBit(c)) BkLiteral(c, 3, 0);           // 1 à 8 octets bruts
      else           BkCopy(c, 8, 2);              // copie de 2 octets, distance sur 8 bits
    }
    else
    {
      switch (BkBits(c, 2))
      {
        case 3:  BkLiteral(c, 8, 8); break;        // 9 à 264 octets bruts
        case 2:  { const int32_t n = (int32_t)BkBits(c, 8) + 1; BkCopy(c, 12, n); } break;
        case 1:  BkCopy(c, 10, 4); break;
        default: BkCopy(c, 9, 3); break;
      }
    }
  }
  return !c.bad && c.crc == 0;
}

// ---------------------------------------------------------------------------------------------------
bool AwRes_Init(void)
{
  File f = SD_MMC.open(AW_DIR "/MEMLIST.BIN", FILE_READ);
  if (!f) { snprintf(s_err, sizeof s_err, "%s/MEMLIST.BIN not found", AW_DIR); return false; }
  g_awMemCount = 0;
  uint8_t r[20];
  while (g_awMemCount < AW_MAX_ENTRIES && f.read(r, 20) == 20)
  {
    if (r[0] == 0xFF) break;                       // fin de table
    AwMemEntry &e = g_awMem[g_awMemCount++];
    e.state  = AW_ST_FREE;
    e.type   = r[1];
    e.bank   = r[7];
    e.offset = Be32(r + 8);
    e.packed = Be32(r + 12);
    e.size   = Be32(r + 16);
    e.buf    = nullptr;
  }
  f.close();
  File b1 = SD_MMC.open(AW_DIR "/BANK01", FILE_READ);
  if (b1) { g_awAmiga = b1.size() == 244674 || b1.size() == 244868; b1.close(); }
  AW_LOG("[RES] MEMLIST.BIN : %d entries, %s data\n", g_awMemCount, g_awAmiga ? "Amiga" : "DOS");
  if (g_awMemCount < 0x80) { snprintf(s_err, sizeof s_err, "MEMLIST.BIN too short (%d entries)", g_awMemCount); return false; }
  return true;
}

static void FreeEntry(AwMemEntry &e)
{
  if (e.buf) heap_caps_free(e.buf);
  e.buf = nullptr;
  e.state = AW_ST_FREE;
}

static bool LoadEntry(int num)
{
  if (num <= 0 || num >= g_awMemCount) return false;
  AwMemEntry &e = g_awMem[num];
  if (e.state == AW_ST_LOADED) return true;
  if (e.bank == 0 || e.size == 0) return false;

  char path[32];
  snprintf(path, sizeof path, AW_DIR "/BANK%02X", e.bank);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { snprintf(s_err, sizeof s_err, "%s not found", path); AW_LOG("[RES] %s\n", s_err); return false; }

  uint8_t *out = (uint8_t *)heap_caps_malloc(e.size, MALLOC_CAP_SPIRAM);
  uint8_t *in = (e.packed == e.size) ? out : (uint8_t *)heap_caps_malloc(e.packed, MALLOC_CAP_SPIRAM);
  bool ok = out && in && f.seek(e.offset) && f.read(in, e.packed) == e.packed;
  f.close();
  if (ok && in != out && !AwUnpack(out, e.size, in, e.packed))
    AW_LOG("[RES] entry 0x%02X : bad checksum after unpacking\n", num);   // on garde le résultat
  if (in && in != out) heap_caps_free(in);
  if (!ok)
  {
    if (out) heap_caps_free(out);
    snprintf(s_err, sizeof s_err, "cannot read entry 0x%02X (%s)", num, path);
    AW_LOG("[RES] %s\n", s_err);
    return false;
  }

  if (e.type == AW_RT_BITMAP)                      // image de fond : copiée en page 0, jamais gardée
  {
    AwVid_LoadBitmap(out, e.size);
    heap_caps_free(out);
    return true;
  }
  e.buf = out;
  e.state = AW_ST_LOADED;
  return true;
}

void AwRes_FreeVolatile(void)
{
  for (int i = 0; i < g_awMemCount; i++)
    if (g_awMem[i].type <= AW_RT_BITMAP) FreeEntry(g_awMem[i]);
}

bool AwRes_SetupPart(uint16_t part)
{
  if (part < AW_PART_FIRST || part > AW_PART_LAST) return false;
  const uint8_t *ids = k_parts[part - AW_PART_FIRST];
  if (part != g_awCurPart)
  {
    for (int i = 0; i < g_awMemCount; i++) FreeEntry(g_awMem[i]);
    for (int k = 0; k < 4; k++)
      if (ids[k] && !LoadEntry(ids[k])) return false;
  }
  g_awSegPal      = g_awMem[ids[0]].buf;
  g_awSegCode     = g_awMem[ids[1]].buf;
  g_awSegCodeSize = g_awMem[ids[1]].size;
  g_awSegPoly     = g_awMem[ids[2]].buf;
  g_awSegPoly2    = ids[3] ? g_awMem[ids[3]].buf : nullptr;
  g_awCurPart = part;
  AW_LOG("[RES] part %u loaded, PSRAM free %u\n", part, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

void AwRes_Request(uint16_t num)
{
  if (num >= AW_PART_FIRST) { g_awNextPart = num; return; }   // appliqué avant la trame suivante
  LoadEntry(num);
}
