#!/usr/bin/env python3
"""BoundHash (moneta BOUND) - modello di riferimento per il bitstream.

Riscritto da docs/boundhash.md del repository github.com/Sonic-Bound/Bound (commit e3549ec).
In piu' rispetto al riferimento ufficiale: la divisione host/FPGA (midstate dei primi 64 byte
dell'header) e la generazione di rtl/bound_const.vh e dei vettori per la simulazione.

  python3 bound_ref.py --selftest          confronta con i vettori di tests/test_boundhash.cpp
  python3 bound_ref.py --const             scrive ../rtl/bound_const.vh
  python3 bound_ref.py --vec LINES N       N vettori per il testbench (mid tail nonce hash)
  python3 bound_ref.py HEADERHEX [LINES]
"""
import hashlib, struct, sys, os

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
K = [
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]
IV = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]

def rr(x, n): return ((x >> n) | (x << (32 - n))) & M32

def compress(st, blk):
    """Una compressione SHA-256: st = 8 parole, blk = 64 byte."""
    w = list(struct.unpack(">16I", blk))
    for i in range(16, 64):
        s0 = rr(w[i-15], 7) ^ rr(w[i-15], 18) ^ (w[i-15] >> 3)
        s1 = rr(w[i-2], 17) ^ rr(w[i-2], 19) ^ (w[i-2] >> 10)
        w.append((w[i-16] + s0 + w[i-7] + s1) & M32)
    a, b, c, d, e, f, g, h = st
    for i in range(64):
        t1 = (h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) & M32
        t2 = ((rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) & M32
        a, b, c, d, e, f, g, h = (t1 + t2) & M32, a, b, c, (d + t1) & M32, e, f, g
    return [(x + y) & M32 for x, y in zip(st, [a, b, c, d, e, f, g, h])]

def sha256(d): return hashlib.sha256(d).digest()

def tag_mid(tag):
    """Stato SHA-256 dopo il blocco SHA256(tag) || SHA256(tag): costante dell'algoritmo."""
    t = sha256(tag.encode())
    return compress(IV, t + t)

MID_SEED = tag_mid("BOUND/BoundHash")
MID_RES = tag_mid("BOUND/BoundHash-result")

def tagged_hash(tag, data):
    t = sha256(tag.encode())
    return sha256(t + t + data)

def header_mid(header):
    """Lavoro dell'host: stato dopo i primi 64 byte dell'header (uguale per tutti i nonce)."""
    return compress(MID_SEED, header[:64])

def rotr(x, n): return ((x >> n) | (x << (64 - n))) & M64
def blamka(x, y): return (x + y + 2 * (x & M32) * (y & M32)) & M64

def g(v, a, b, c, d):
    v[a] = blamka(v[a], v[b]); v[d] = rotr(v[d] ^ v[a], 32)
    v[c] = blamka(v[c], v[d]); v[b] = rotr(v[b] ^ v[c], 24)
    v[a] = blamka(v[a], v[b]); v[d] = rotr(v[d] ^ v[a], 16)
    v[c] = blamka(v[c], v[d]); v[b] = rotr(v[b] ^ v[c], 63)

def rnd(v):
    g(v,0,4,8,12); g(v,1,5,9,13); g(v,2,6,10,14); g(v,3,7,11,15)
    g(v,0,5,10,15); g(v,1,6,11,12); g(v,2,7,8,13); g(v,3,4,9,14)

def fill_line(x, i):
    t = list(x); t[15] ^= i; rnd(t)
    return [t[k] ^ x[k] for k in range(16)]

def words(d): return [int.from_bytes(d[8*k:8*k+8], "little") for k in range(len(d) // 8)]

def bound_hash_split(mid, tail16, lines=4096):
    """Come lo calcola l'FPGA: mid = header_mid(), tail16 = header[64:80] (12 byte + nonce)."""
    pad = tail16 + b"\x80" + bytes(39) + (144 * 8).to_bytes(8, "big")
    seed = struct.pack(">8I", *compress(mid, pad))
    start = []
    for k in range(4):
        start += words(sha256(seed + bytes([k])))
    mem = [fill_line(start, 0)]
    mem.append(fill_line(mem[0], 1))
    for i in range(2, lines):
        prev = mem[i-1]
        ref = ((prev[0] & M32) * (i - 1)) >> 32
        mem.append(fill_line([prev[k] ^ mem[ref][k] for k in range(16)], i))
    v = [start[k] ^ mem[lines-1][k] for k in range(16)]
    for r in range(lines):
        line = mem[v[0] & (lines - 1)]
        for k in range(16): v[k] ^= line[k]
        p = v[0] * (v[1] | 1)
        v[2] = (v[2] + (p >> 64)) & M64
        v[3] ^= p & M64
        v[15] ^= r
        rnd(v)
        for k in range(16): line[k] = (line[k] + v[k]) & M64
    out = seed + b"".join(w.to_bytes(8, "little") for w in v)
    return tagged_hash("BOUND/BoundHash-result", out)

def bound_hash(header, lines=4096):
    assert len(header) == 80
    return bound_hash_split(header_mid(header), header[64:], lines)

def display(d): return d[::-1].hex()

def vector_header(n):
    return ((1).to_bytes(4, "little") + sha256(b"prev %d" % n) + sha256(b"merkle %d" % n)
            + (1800000000 + n).to_bytes(4, "little") + (0x1E03FFFF).to_bytes(4, "little")
            + n.to_bytes(4, "little"))

def hx(st): return "".join("%08x" % x for x in st)

def main(a):
    here = os.path.dirname(os.path.abspath(__file__))
    if a[1:2] == ["--const"]:
        with open(os.path.join(here, "..", "rtl", "bound_const.vh"), "w") as f:
            f.write("// generato da model/bound_ref.py --const: stati SHA-256 dopo il blocco tag||tag\n")
            f.write("localparam [255:0] MID_SEED = 256'h%s; // \"BOUND/BoundHash\" (serve all'host)\n" % hx(MID_SEED))
            f.write("localparam [255:0] MID_RES  = 256'h%s; // \"BOUND/BoundHash-result\"\n" % hx(MID_RES))
        return 0
    if a[1:2] == ["--vec"]:
        lines, n = int(a[2]), int(a[3])
        for i in range(n):
            h = vector_header(i if i < 3 else i * 7919 + 3)   # i primi 3 sono gli ufficiali
            print(hx(header_mid(h)), h[64:76].hex(), "%08x" % int.from_bytes(h[76:80], "little"),
                  display(bound_hash(h, lines)))
        return 0
    if a[1:2] == ["--selftest"]:
        exp = [l.split() for l in open(os.path.join(here, "vettori_ufficiali.txt")) if l.strip() and l[0] != "#"]
        ok = True
        for lines, n, want in exp:
            got = display(bound_hash(vector_header(int(n)), int(lines)))
            print(lines, n, got, "OK" if got == want else "DIVERSO da " + want); ok &= got == want
        print("SELFTEST", "OK" if ok else "FALLITO"); return 0 if ok else 1
    h = bytes.fromhex(a[1]); print(display(bound_hash(h, int(a[2]) if len(a) > 2 else 4096))); return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
