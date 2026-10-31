# Luc Dion 31-10-2026 — licence MIT, voir LICENSE. Outil ANOTHERWORLD (préparation de la carte SD).
# Lecture seule d'une image ADF AmigaDOS (OFS/FFS) : liste et extrait les fichiers.
# usage : python -I adf_extract.py image.adf dossier_sortie
import os, struct, sys

def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def s32(b, o): return struct.unpack_from(">i", b, o)[0]

def main(img, out):
    d = open(img, "rb").read()
    ffs = d[3] & 1
    blk = lambda n: d[n * 512:(n + 1) * 512]
    def name(b):
        l = b[432]
        return b[433:433 + l].decode("latin-1")
    def read_file(hdr):
        b = blk(hdr)
        size = u32(b, 324)
        data = bytearray()
        h = hdr
        while h and len(data) < size:
            hb = blk(h)
            n = u32(hb, 8)
            ptrs = [u32(hb, 308 - 4 * i) for i in range(n)]   # table de blocs, ordre inverse
            for p in ptrs:
                db = blk(p)
                if ffs: data += db
                else: data += db[24:24 + u32(db, 12)]
            h = u32(hb, 504)                                  # bloc d'extension
        return bytes(data[:size])
    def walk(dirblk, path):
        b = blk(dirblk)
        for i in range(72):
            e = u32(b, 24 + 4 * i)
            while e:
                eb = blk(e)
                t = s32(eb, 508)
                nm = name(eb)
                p = os.path.join(path, nm)
                if t == 2:
                    os.makedirs(os.path.join(out, p), exist_ok=True)
                    walk(e, p)
                elif t == -3:
                    data = read_file(e)
                    os.makedirs(os.path.join(out, path), exist_ok=True)
                    open(os.path.join(out, p), "wb").write(data)
                    print("%-30s %8d" % (p, len(data)))
                e = u32(eb, 496)                              # chaîne de hachage
    print("disk:", name(blk(880)), "FFS" if ffs else "OFS")
    walk(880, "")

main(sys.argv[1], sys.argv[2])
