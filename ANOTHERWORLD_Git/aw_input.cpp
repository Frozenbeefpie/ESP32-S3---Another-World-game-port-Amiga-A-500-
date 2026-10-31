// ANOTHERWORLD — entrées : clavier USB (EspUsbHost) et port série (banc de test).
// Luc Dion 31-10-2026 — licence MIT, voir LICENSE.
//
// Clavier : flèches = déplacement, Espace / Entrée / Ctrl = action, P = pause, C = code d'accès,
// F2 = couleurs (checker frc / checker / bayer 4x4 / oklab / oklab nearest / raw), F3 = son coupé, F4 = interpolation du son,
// F5..F11 = parties 16001..16007,
// - et = = luminosité (plus sombre / plus clair), [ et ] = longueur de ligne VGA (barres verticales sur écran LCD).
// Série (COM5) : w a s d = flèches, espace = action, p, c, x = couleurs, m = son, i = interpolation, 1..7 = parties, - = [ ] ; une touche
// série reste « enfoncée » 200 ms (la répétition du terminal la prolonge).
#include <Arduino.h>
#include "aw_config.h"
#include "aw_input.h"
#include "aw_video.h"
#include "aw_sound.h"
#include "src/EspUsbHost/EspUsbHost.h"

AwInput g_awIn;

static EspUsbHost *s_usb = nullptr;
static volatile bool s_kLeft, s_kRight, s_kUp, s_kDown, s_kSpace, s_kEnter, s_kCtrl;

// touches série : échéance de relâchement (millis)
static uint32_t s_sLeft, s_sRight, s_sUp, s_sDown, s_sAction;

static void OnKeyboard(const EspUsbHostKeyboardEvent &ev)
{
  s_kCtrl = (ev.modifiers & 0x11) != 0;                // Ctrl gauche ou droit
  const bool down = ev.pressed;
  switch (ev.keycode)
  {
    case 0x4F: s_kRight = down; break;
    case 0x50: s_kLeft = down; break;
    case 0x51: s_kDown = down; break;
    case 0x52: s_kUp = down; break;
    case 0x2C: s_kSpace = down; break;
    case 0x28: case 0x58: s_kEnter = down; break;
    default: break;
  }
  if (!down) return;
  if (ev.keycode == 0x13) g_awIn.pause = !g_awIn.pause;          // P
  else if (ev.keycode == 0x3B) AwVid_CycleColorMode();             // F2
  else if (ev.keycode == 0x3C) AwSnd_SetMute(!AwSnd_Mute());       // F3
  else if (ev.keycode == 0x3D) { AwSnd_SetInterp(!AwSnd_Interp()); AW_LOG("[SND] interpolation %s\n", AwSnd_Interp() ? "on" : "off"); }   // F4
  else if (ev.keycode == 0x2D) AwVid_AdjustGamma(1);               // - : plus sombre
  else if (ev.keycode == 0x2E) AwVid_AdjustGamma(-1);              // = : plus clair
  else if (ev.keycode == 0x2F) AwVid_AdjustHTotal(-1);             // [
  else if (ev.keycode == 0x30) AwVid_AdjustHTotal(1);              // ]
  else if (ev.keycode >= 0x3E && ev.keycode <= 0x44) g_awIn.partReq = (uint16_t)(AW_PART_INTRO + ev.keycode - 0x3E);
  if (ev.keycode == 0x06) g_awIn.code = true;                      // C
  if (ev.keycode == 0x2A) g_awIn.lastChar = 8;                     // retour arrière
  else if (ev.ascii >= 'a' && ev.ascii <= 'z') g_awIn.lastChar = (char)ev.ascii;
  else if (ev.ascii >= 'A' && ev.ascii <= 'Z') g_awIn.lastChar = (char)(ev.ascii + 32);
}

void AwIn_Init(void)
{
  memset(&g_awIn, 0, sizeof g_awIn);
  s_usb = new EspUsbHost();
  s_usb->onKeyboard(OnKeyboard);
  s_usb->setHidInterfaceFilter(true);
  EspUsbHostConfig cfg;
  cfg.taskCore = 0;
  cfg.taskStackSize = 5120;
  cfg.hostTaskStackSize = 4608;
  if (!s_usb->begin(cfg)) AW_LOG("[USB] begin() failed: %s\n", s_usb->lastErrorName());
  else AW_LOG("[USB] host started\n");
}

void AwIn_Poll(void)
{
  const uint32_t now = millis(), hold = now + 200;
  while (Serial.available())
  {
    const char c = (char)Serial.read();
    switch (c)
    {
      case 'a': s_sLeft = hold; break;
      case 'd': s_sRight = hold; break;
      case 'w': s_sUp = hold; break;
      case 's': s_sDown = hold; break;
      case ' ': s_sAction = hold; break;
      case 'p': g_awIn.pause = !g_awIn.pause; break;
      case 'c': g_awIn.code = true; break;
      case 'x': AwVid_CycleColorMode(); break;
      case '-': AwVid_AdjustGamma(1); break;
      case '=': AwVid_AdjustGamma(-1); break;
      case '[': AwVid_AdjustHTotal(-1); break;
      case ']': AwVid_AdjustHTotal(1); break;
      case 'g': AwVid_DumpPages(); break;
      case 't': g_awTraceStr = !g_awTraceStr; AW_LOG("[STR] trace %s\n", g_awTraceStr ? "on" : "off"); break;
      case 'i': AwSnd_SetInterp(!AwSnd_Interp()); AW_LOG("[SND] interpolation %s\n", AwSnd_Interp() ? "on" : "off"); break;
      case 'm': AwSnd_SetMute(!AwSnd_Mute()); AW_LOG("[SND] mute %s\n", AwSnd_Mute() ? "on" : "off"); break;
      default:
        if (c >= '1' && c <= '7') g_awIn.partReq = (uint16_t)(AW_PART_INTRO + c - '1');
        break;
    }
  }
  g_awIn.left   = s_kLeft  || (int32_t)(s_sLeft - now) > 0;
  g_awIn.right  = s_kRight || (int32_t)(s_sRight - now) > 0;
  g_awIn.up     = s_kUp    || (int32_t)(s_sUp - now) > 0;
  g_awIn.down   = s_kDown  || (int32_t)(s_sDown - now) > 0;
  g_awIn.action = s_kSpace || s_kEnter || s_kCtrl || (int32_t)(s_sAction - now) > 0;
}
