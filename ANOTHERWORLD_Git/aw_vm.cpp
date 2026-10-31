// ANOTHERWORLD — machine virtuelle : 64 fils d'exécution coopératifs, 256 variables 16 bits.
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
//
// Chaque trame : application des demandes (nouvelle partie, saut ou arrêt de fil, pause/reprise), mise à
// jour des variables de commande, puis exécution de chaque fil actif jusqu'à ce qu'il rende la main
// (instruction 0x06 ou 0x11). Octets de bytecode gros-boutistes. Opcode >= 0x80 : forme de fond à position
// fixe ; 0x40..0x7F : forme à position / zoom variables ; 0x00..0x1A : instructions.
#include <Arduino.h>
#include "aw_config.h"
#include "aw_vm.h"
#include "aw_res.h"
#include "aw_video.h"
#include "aw_sound.h"
#include "aw_input.h"

#define AW_THREADS   64
#define AW_NO_PC     0xFFFF   // fil inactif
#define AW_NO_REQ    0xFFFF   // pas de saut demandé
#define AW_KILL_REQ  0xFFFE   // arrêt demandé
#define AW_STACK     64
#define AW_MAX_OPS   200000   // garde-fou : un fil qui ne rend jamais la main

volatile int16_t g_awVar[256];

static uint16_t s_pc[AW_THREADS], s_reqPc[AW_THREADS];
static uint8_t  s_paused[AW_THREADS], s_reqPaused[AW_THREADS];
static uint16_t s_stack[AW_STACK];
static int      s_sp;
static uint32_t s_ip;           // position dans le bytecode du fil en cours
static bool     s_yield;
static bool     s_fault;
static uint32_t s_lastBlit = 0;

static inline uint8_t Fetch8(void)
{
  if (s_ip >= g_awSegCodeSize) { s_fault = true; return 0x06; }   // hors du segment : rendre la main
  return g_awSegCode[s_ip++];
}
static inline uint16_t Fetch16(void) { const uint16_t h = Fetch8(); return (uint16_t)((h << 8) | Fetch8()); }
static inline int16_t &Var(uint8_t i) { return (int16_t &)g_awVar[i]; }

void AwVm_Init(void)
{
  for (int i = 0; i < 256; i++) g_awVar[i] = 0;
  g_awVar[0x54] = 0x81;
  g_awVar[AW_VAR_RANDOM_SEED] = (int16_t)esp_random();
  // valeurs laissées par l'écran de protection (16000), que l'on saute
  g_awVar[0xBC] = 0x10;
  g_awVar[0xC6] = 0x80;
  g_awVar[0xF2] = g_awAmiga ? 6000 : 4000;
  g_awVar[0xDC] = 33;
}

bool AwVm_StartPart(uint16_t part)
{
  AwSnd_StopAll();
  g_awVar[0xE4] = 0x14;
  if (!AwRes_SetupPart(part)) return false;
  for (int i = 0; i < AW_THREADS; i++)
  {
    s_pc[i] = AW_NO_PC;
    s_reqPc[i] = AW_NO_REQ;
    s_paused[i] = s_reqPaused[i] = 0;
  }
  s_pc[0] = 0;
  return true;
}

// ---------------------------------------------------------------------------------------------------
static void OpJump(void) { s_ip = Fetch16(); }

static void OpCondJump(void)
{
  const uint8_t op = Fetch8();
  const int16_t b = Var(Fetch8());
  int16_t a;
  if (op & 0x80)      a = Var(Fetch8());
  else if (op & 0x40) a = (int16_t)Fetch16();
  else                a = Fetch8();
  bool t = false;
  switch (op & 7)
  {
    case 0: t = b == a; break;
    case 1: t = b != a; break;
    case 2: t = b > a;  break;
    case 3: t = b >= a; break;
    case 4: t = b < a;  break;
    case 5: t = b <= a; break;
    default: break;
  }
  const uint16_t dst = Fetch16();
  if (t) s_ip = dst;
}

static void OpThreadRange(void)
{
  const uint8_t first = Fetch8(), last = Fetch8() & 0x3F, mode = Fetch8();
  if (last < first || first >= AW_THREADS) return;
  for (int i = first; i <= last; i++)
  {
    if (mode == 2) s_reqPc[i] = AW_KILL_REQ;
    else if (mode < 2) s_reqPaused[i] = mode;
  }
}

static void FrameWait(void)
{
  // le bytecode règle la cadence : PAUSE_SLICES x 20 ms entre deux affichages
  const int32_t want = (int32_t)Var(AW_VAR_PAUSE_SLICES) * 20;
  const int32_t wait = want - (int32_t)(millis() - s_lastBlit);
  if (wait > 0) delay((uint32_t)wait);
  while (g_awIn.pause) { AwIn_Poll(); delay(20); }
  s_lastBlit = millis();
}

static void OpBlit(void)
{
  const uint8_t page = Fetch8();
  if (g_awIn.code)                                     // touche C : écran du code d'accès
  {
    g_awIn.code = false;
    if (g_awCurPart != AW_PART_FIRST && g_awCurPart != AW_PART_LAST) g_awNextPart = AW_PART_LAST;
  }
  FrameWait();
  Var(0xF7) = 0;
  AwVid_Show(page);
}

static void DrawBackgroundShape(uint8_t op)
{
  const uint16_t off = (uint16_t)(((op << 8) | Fetch8()) * 2);
  int16_t x = Fetch8(), y = Fetch8();
  const int16_t over = y - 199;
  if (over > 0) { y = 199; x += over; }
  AwVid_DrawShape(g_awSegPoly, off, 64, x, y);
}

static void DrawShape(uint8_t op)
{
  const uint16_t off = (uint16_t)(Fetch16() * 2);
  int16_t x = Fetch8();
  if (!(op & 0x20))
  {
    if (!(op & 0x10)) x = (int16_t)((x << 8) | Fetch8());     // mot immédiat
    else x = Var((uint8_t)x);                                  // variable
  }
  else if (op & 0x10) x += 0x100;                              // octet + 256
  int16_t y = Fetch8();
  if (!(op & 0x08))
  {
    if (!(op & 0x04)) y = (int16_t)((y << 8) | Fetch8());
    else y = Var((uint8_t)y);
  }
  const uint8_t *seg = g_awSegPoly;
  uint16_t zoom = 64;
  if (!(op & 0x02))
  {
    if (op & 0x01) zoom = (uint16_t)Var(Fetch8());             // zoom dans une variable
  }
  else
  {
    if (op & 0x01) seg = g_awSegPoly2;                         // formes communes, zoom 1:1
    else zoom = Fetch8();                                      // zoom immédiat
  }
  AwVid_DrawShape(seg, off, zoom, x, y);
}

static void Step(void)
{
  const uint8_t op = Fetch8();
  if (op & 0x80) { DrawBackgroundShape(op); return; }
  if (op & 0x40) { DrawShape(op); return; }
  switch (op)
  {
    case 0x00: { const uint8_t i = Fetch8(); Var(i) = (int16_t)Fetch16(); } break;
    case 0x01: { const uint8_t i = Fetch8(); Var(i) = Var(Fetch8()); } break;
    case 0x02: { const uint8_t i = Fetch8(); Var(i) += Var(Fetch8()); } break;
    case 0x03: { const uint8_t i = Fetch8(); Var(i) += (int16_t)Fetch16(); } break;
    case 0x04: { const uint16_t dst = Fetch16(); if (s_sp < AW_STACK) s_stack[s_sp++] = (uint16_t)s_ip; s_ip = dst; } break;
    case 0x05: if (s_sp > 0) s_ip = s_stack[--s_sp]; else s_fault = true; break;
    case 0x06: s_yield = true; break;
    case 0x07: OpJump(); break;
    case 0x08: { const uint8_t t = Fetch8(); const uint16_t dst = Fetch16(); if (t < AW_THREADS) s_reqPc[t] = dst; } break;
    case 0x09: { const uint8_t i = Fetch8(); if (--Var(i) != 0) OpJump(); else Fetch16(); } break;
    case 0x0A: OpCondJump(); break;
    case 0x0B: AwVid_RequestPalette((uint8_t)(Fetch16() >> 8)); break;
    case 0x0C: OpThreadRange(); break;
    case 0x0D: AwVid_SelectPage(Fetch8()); break;
    case 0x0E: { const uint8_t p = Fetch8(); AwVid_FillPage(p, Fetch8()); } break;
    case 0x0F: { const uint8_t s = Fetch8(); AwVid_CopyPage(s, Fetch8(), Var(AW_VAR_SCROLL_Y)); } break;
    case 0x10: OpBlit(); break;
    case 0x11: s_ip = AW_NO_PC; s_yield = true; break;
    case 0x12: { const uint16_t id = Fetch16(); const uint8_t x = Fetch8(), y = Fetch8(); AwVid_DrawString(Fetch8(), x, y, id); } break;
    case 0x13: { const uint8_t i = Fetch8(); Var(i) -= Var(Fetch8()); } break;
    case 0x14: { const uint8_t i = Fetch8(); Var(i) = (int16_t)((uint16_t)Var(i) & Fetch16()); } break;
    case 0x15: { const uint8_t i = Fetch8(); Var(i) = (int16_t)((uint16_t)Var(i) | Fetch16()); } break;
    case 0x16: { const uint8_t i = Fetch8(); Var(i) = (int16_t)((uint16_t)Var(i) << (Fetch16() & 15)); } break;
    case 0x17: { const uint8_t i = Fetch8(); Var(i) = (int16_t)((uint16_t)Var(i) >> (Fetch16() & 15)); } break;
    case 0x18: { const uint16_t r = Fetch16(); const uint8_t f = Fetch8(), v = Fetch8(); AwSnd_PlaySound(r, f, v, Fetch8()); } break;
    case 0x19:
    {
      const uint16_t r = Fetch16();
      if (r == 0) { AwSnd_StopAll(); AwRes_FreeVolatile(); }
      else AwRes_Request(r);
    }
    break;
    case 0x1A: { const uint16_t r = Fetch16(), d = Fetch16(); AwSnd_PlayMusic(r, d, Fetch8()); } break;
    default:
      AW_LOG("[VM] bad opcode 0x%02X at 0x%04X\n", op, (unsigned)(s_ip - 1));
      s_fault = true;
      break;
  }
}

// ---------------------------------------------------------------------------------------------------
static void UpdateInputVars(void)
{
  if (g_awCurPart == AW_PART_LAST && g_awIn.lastChar)   // écran du code : lettre en majuscule
  {
    Var(AW_VAR_LAST_KEYCHAR) = (int16_t)(g_awIn.lastChar == 8 ? 8 : (g_awIn.lastChar & ~0x20));
    g_awIn.lastChar = 0;
  }
  int16_t lr = 0, ud = 0, mask = 0;
  if (g_awIn.right) { lr = 1;  mask |= 1; }
  if (g_awIn.left)  { lr = -1; mask |= 2; }
  if (g_awIn.down)  { ud = 1;  mask |= 4; }
  Var(AW_VAR_HERO_POS_UP_DOWN) = g_awIn.up ? -1 : ud;
  if (g_awIn.up)    { ud = -1; mask |= 8; }
  Var(AW_VAR_HERO_JUMP_DOWN) = ud;
  Var(AW_VAR_HERO_LEFT_RIGHT) = lr;
  Var(AW_VAR_HERO_POS_MASK) = mask;
  Var(AW_VAR_HERO_ACTION) = g_awIn.action ? 1 : 0;
  if (g_awIn.action) mask |= 0x80;
  Var(AW_VAR_HERO_ACTION_MASK) = mask;
}

bool AwVm_RunFrame(void)
{
  if (g_awIn.partReq) { g_awNextPart = g_awIn.partReq; g_awIn.partReq = 0; }
  if (g_awNextPart)
  {
    const uint16_t p = g_awNextPart;
    g_awNextPart = 0;
    AW_LOG("[VM] part %u\n", p);
    if (!AwVm_StartPart(p)) return false;
  }
  for (int i = 0; i < AW_THREADS; i++)
  {
    s_paused[i] = s_reqPaused[i];
    if (s_reqPc[i] != AW_NO_REQ)
    {
      s_pc[i] = (s_reqPc[i] == AW_KILL_REQ) ? AW_NO_PC : s_reqPc[i];
      s_reqPc[i] = AW_NO_REQ;
    }
  }
  AwIn_Poll();
  UpdateInputVars();
  for (int t = 0; t < AW_THREADS; t++)
  {
    if (s_paused[t] || s_pc[t] == AW_NO_PC) continue;
    s_ip = s_pc[t];
    s_sp = 0;
    s_yield = false;
    s_fault = false;
    uint32_t n = 0;
    while (!s_yield && !s_fault)
    {
      Step();
      if (++n > AW_MAX_OPS) { AW_LOG("[VM] thread %d never yields (pc 0x%04X)\n", t, (unsigned)s_ip); s_fault = true; }
    }
    if (s_fault) { AW_LOG("[VM] thread %d stopped (part %u)\n", t, g_awCurPart); s_ip = AW_NO_PC; }
    s_pc[t] = (uint16_t)s_ip;
  }
  return true;
}
