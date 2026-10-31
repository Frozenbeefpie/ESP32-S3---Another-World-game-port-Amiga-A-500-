# Luc Dion 31-10-2026 — licence MIT, voir LICENSE. Outil ANOTHERWORLD (préparation de la carte SD).
# Table des textes : u16 numéro (adresse paire), texte (0x0D = saut de ligne), 0, mots nuls de remplissage.
# Reprend après une rupture : cherche la prochaine adresse paire où deux entrées valides se suivent.
# Écrit STRINGS.TXT (« id=texte », \n, accents Latin-1 -> CP437). N'affiche que numéros et comptes.
# usage : python -I make_strings.py exe debut fin sortie [ids voulus en hexadécimal...]
import struct, sys
d = open(sys.argv[1], "rb").read()
lo, hi, out = int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
want = [int(x, 16) for x in sys.argv[5:]]

def text_ok(t): return 1 <= len(t) <= 400 and all(c >= 32 or c in (9, 10, 13) for c in t)

def entry(o):
    if o + 3 > len(d) or o & 1: return None
    i = struct.unpack_from(">H", d, o)[0]
    e = d.find(b"\0", o + 2)
    if i == 0 or i >= 0x400 or e < 0: return None
    t = d[o + 2:e]
    if not text_ok(t): return None
    n = e + 1
    if n & 1: n += 1
    z = 0
    while n + 2 <= len(d) and d[n] == 0 and d[n + 1] == 0 and z < 3: n += 2; z += 1
    return i, t, n

ents, o, resync = {}, lo, 0
while o < hi:
    r = entry(o)
    if r and (entry(r[2]) or r[2] >= hi):
        if r[0] not in ents: ents[r[0]] = r[1]
        o = r[2]
    else:
        o += 2; resync += 1
lat = {0xE9: 0x82, 0xE8: 0x8A, 0xEA: 0x88, 0xEB: 0x89, 0xE0: 0x85, 0xE2: 0x83, 0xE7: 0x87, 0xEE: 0x8C,
       0xEF: 0x8B, 0xF4: 0x93, 0xF9: 0x97, 0xFB: 0x96, 0xFC: 0x81, 0xC9: 0x90, 0xC7: 0x80}
with open(out, "wb") as f:
    for i in sorted(ents):
        b = bytes(lat.get(c, c) for c in ents[i]).replace(b"\r", b"\x5cn").replace(b"\n", b"\x5cn").replace(b"\t", b" ")
        f.write(b"%X=" % i + b + b"\n")
print("%d entries, ids 0x%X..0x%X, skipped %d words" % (len(ents), min(ents), max(ents), resync))
if want: print("wanted found %d/%d, missing %s" % (sum(w in ents for w in want), len(want), [hex(w) for w in want if w not in ents]))
