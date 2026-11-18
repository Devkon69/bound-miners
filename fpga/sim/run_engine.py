#!/usr/bin/env python3
"""bound_engine (3 core, 64 righe) contro il modello: i candidati devono essere tutti e soli i nonce
   con i 32 bit alti dell'hash <= thresh, tra quelli che i core hanno finito in ciascun lavoro."""
import os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here); rtl = os.path.join(root, "rtl")
sys.path.insert(0, os.path.join(root, "model")); import bound_ref as m
cfg = dict(AW=6, NCORE=3, NULTRA=1, RDLAT=int(os.environ.get("RDLAT", 1)), BM2=int(os.environ.get("BM2", 0)),
           MULLAT=int(os.environ.get("MULLAT", 2)))
THRESH = 0x1fffffff
heads = [m.vector_header(101), m.vector_header(202)]
bases = [0x00000000, 0x12345678]
with open(os.path.join(here, "jobs.txt"), "w") as f:
    for h, b in zip(heads, bases): f.write("%s %s %08x\n" % (m.hx(m.header_mid(h)), h[64:76].hex(), b))
    f.write("%08x\n" % THRESH)
out = os.path.join(here, "tb_engine.vvp")
subprocess.check_call(["iverilog", "-g2005", "-I", rtl, "-o", out] + ["-Ptb_engine.%s=%d" % kv for kv in cfg.items()] +
    [os.path.join(here, "tb_engine.v")] + [os.path.join(rtl, f) for f in ("bound_engine.v", "bound_core.v", "bound_ram.v", "bound_sha256.v")])
subprocess.check_call(["vvp", "-n", out], cwd=here, stdout=subprocess.DEVNULL)
cands = {0: {}, 1: {}}; counts = []
for l in open(os.path.join(here, "engine.log")):
    p = l.split()
    if p[0] == "C": cands[int(p[1])][int(p[2], 16)] = int(p[3], 16)
    else: counts.append(int(p[1]))
ok = counts[0] == counts[1] and counts[0] >= 40
lines = 1 << cfg["AW"]; tot = 0
for j in (0, 1):
    exp = {}
    # ogni core ha fatto almeno 12 nonce di fila nel lavoro: controllo quelli, e che ogni candidato sia vero
    for c in range(cfg["NCORE"]):
        for k in range(12):
            nonce = (bases[j] + (c << 30) + k) & 0xffffffff
            h = heads[j][:76] + nonce.to_bytes(4, "little")
            v = int(m.display(m.bound_hash(h, lines)), 16)
            if (v >> 224) <= THRESH: exp[nonce] = (v >> 232) << 40
    for nonce, hi in exp.items():
        if cands[j].get(nonce) != hi: print("lavoro", j, "manca il candidato %08x" % nonce); ok = False
    for nonce, hi in cands[j].items():
        h = heads[j][:76] + nonce.to_bytes(4, "little")
        v = int(m.display(m.bound_hash(h, lines)), 16)
        if (v >> 232) != (hi >> 40) or (v >> 224) > THRESH: print("lavoro", j, "candidato falso %08x" % nonce); ok = False
    tot += len(cands[j]); print("lavoro", j, ":", len(cands[j]), "candidati,", len(exp), "attesi nei primi 12 nonce per core")
print("hash contati alla fine:", counts)
print("PASS" if ok and tot > 0 else "FAIL", cfg); sys.exit(0 if ok and tot > 0 else 1)
