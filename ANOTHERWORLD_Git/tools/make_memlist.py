# Luc Dion 31-10-2026 — licence MIT, voir LICENSE. Outil ANOTHERWORLD (préparation de la carte SD).
# Extrait de l'exécutable Amiga la table des ressources (format MEMLIST.BIN, 20 octets par entrée) et
# l'écrit en MEMLIST.BIN. Vérifie chaque entrée contre la taille des fichiers banque.
# usage : python -I make_memlist.py exe position_connue dossier_sortie dossier_banques...
import os, struct, sys

exe = open(sys.argv[1], "rb").read()
known = int(sys.argv[2], 0)
out = sys.argv[3]
banks = {}
for d in sys.argv[4:]:
    for f in os.listdir(d):
        if f.lower().startswith("bank"):
            banks[int(f[4:], 16)] = os.path.getsize(os.path.join(d, f))

def rec(o):
    st, ty = exe[o], exe[o + 1]
    bank = exe[o + 7]
    off, packed, size = struct.unpack_from(">III", exe, o + 8)
    return st, ty, bank, off, packed, size

def plausible(o):
    st, ty, bank, off, packed, size = rec(o)
    if st == 0xFF: return False
    if st > 2 or ty > 6: return False
    if bank == 0: return packed == 0 and size == 0 and off == 0
    if bank > 13 or packed > size: return False
    return bank not in banks or off + packed <= banks[bank]

start = known
while start - 20 >= 0 and plausible(start - 20): start -= 20
o = start
entries = []
while plausible(o):
    entries.append(rec(o)); o += 20
end_ok = exe[o] == 0xFF
print("table start=0x%X entries=%d end marker=%s known entry index=0x%X" % (start, len(entries), end_ok, (known - start) // 20))
missing = sorted({e[2] for e in entries if e[2] and e[2] not in banks})
types = {}
for e in entries: types[e[1]] = types.get(e[1], 0) + 1
print("types:", types, "missing banks:", missing)
for i in (0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x11, 0x7D, 0x7E, 0x7F):
    if i < len(entries): print("  0x%02X type=%d bank=%02X off=%6d packed=%6d size=%6d" % ((i,) + entries[i][1:]))
if end_ok:
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "MEMLIST.BIN"), "wb").write(exe[start:o + 20])
    print("wrote", os.path.join(out, "MEMLIST.BIN"), o + 20 - start, "bytes")
