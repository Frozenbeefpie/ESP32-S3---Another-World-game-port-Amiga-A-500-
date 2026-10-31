# Luc Dion 31-10-2026 — licence MIT, voir LICENSE. Outil ANOTHERWORLD (préparation de la carte SD).
# Écrit MEMLIST.BIN depuis une position donnée de l'exécutable, puis décompresse chaque entrée (même
# algorithme que aw_res.cpp) et vérifie la somme de contrôle.
# usage : python -I check_res.py exe start dossier_sortie dossier_banques...
import os, struct, sys

exe = open(sys.argv[1], "rb").read()
start = int(sys.argv[2], 0)
out = sys.argv[3]
bankdirs = sys.argv[4:]

def bankpath(n):
    for d in bankdirs:
        for f in os.listdir(d):
            if f.lower() == "bank%02x" % n: return os.path.join(d, f)
    return None

o = start
while exe[o] != 0xFF: o += 20
tbl = exe[start:o + 20]
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "MEMLIST.BIN"), "wb").write(tbl)
n = len(tbl) // 20
print("MEMLIST.BIN: %d entries, %d bytes" % (n, len(tbl)))

def unpack(src, dst_size):
    p = len(src) - 4
    be = lambda q: struct.unpack_from(">I", src, q)[0]
    size = be(p); p -= 4
    crc = be(p); p -= 4
    bits = be(p); p -= 4
    crc ^= bits
    dst = bytearray(size)
    d = size - 1
    left = size
    st = {"p": p, "bits": bits, "crc": crc}
    def bit():
        b = st["bits"] & 1
        st["bits"] >>= 1
        if st["bits"] == 0:
            w = be(st["p"]); st["p"] -= 4
            st["crc"] ^= w
            b = w & 1
            st["bits"] = (w >> 1) | 0x80000000
        return b
    def getb(k):
        v = 0
        for _ in range(k): v = (v << 1) | bit()
        return v
    while left > 0:
        if not bit():
            if not bit(): kind, a, b = "L", 3, 0
            else: kind, a, b = "C", 8, 2
        else:
            c = getb(2)
            if c == 3: kind, a, b = "L", 8, 8
            elif c == 2: kind, a, b = "C", 12, getb(8) + 1
            elif c == 1: kind, a, b = "C", 10, 4
            else: kind, a, b = "C", 9, 3
        if kind == "L":
            cnt = min(getb(a) + b + 1, left); left -= cnt
            for _ in range(cnt): dst[d] = getb(8); d -= 1
        else:
            cnt = min(b, left); left -= cnt
            off = getb(a)
            for _ in range(cnt): dst[d] = dst[d + off]; d -= 1
    return bytes(dst), st["crc"] == 0

ok = bad = miss = 0
types = {}
for i in range(n - 1):
    r = tbl[i * 20:i * 20 + 20]
    ty, bank = r[1], r[7]
    off, packed, size = struct.unpack(">III", r[8:20])
    if bank == 0 or size == 0: continue
    types[ty] = types.get(ty, 0) + 1
    path = bankpath(bank)
    if not path:
        miss += 1; print("  0x%02X type %d : bank%02X missing" % (i, ty, bank)); continue
    data = open(path, "rb").read()[off:off + packed]
    if packed == size: ok += 1; continue
    _, good = unpack(data, size)
    if good: ok += 1
    else: bad += 1; print("  0x%02X type %d bank%02X : BAD CRC" % (i, ty, bank))
print("types:", types)
print("ok=%d bad=%d missing=%d" % (ok, bad, miss))
for i in (0x11, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x29, 0x2A, 0x2B, 0x7D, 0x7E, 0x7F):
    if i < n - 1:
        r = tbl[i * 20:i * 20 + 20]
        print("  0x%02X type=%d bank=%02X size=%d" % (i, r[1], r[7], struct.unpack(">I", r[16:20])[0]))
