#!/usr/bin/env python3
"""Simula bound_core con iverilog contro model/bound_ref.py.
   python3 sim/run_core.py                 tutte le configurazioni a 2 e 64 righe + mainnet (4096)
   python3 sim/run_core.py AW RDLAT BM2 MULLAT NVEC     una sola"""
import os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here); rtl = os.path.join(root, "rtl")

def run(aw, rdlat, bm2, mullat, nvec):
    vec = subprocess.check_output([sys.executable, os.path.join(root, "model", "bound_ref.py"), "--vec", str(1 << aw), str(nvec)])
    open(os.path.join(here, "vec.txt"), "wb").write(vec)
    out = os.path.join(here, "tb_core.vvp")
    subprocess.check_call(["iverilog", "-g2005", "-I", rtl, "-o", out,
        "-Ptb_core.AW=%d" % aw, "-Ptb_core.RDLAT=%d" % rdlat, "-Ptb_core.BM2=%d" % bm2,
        "-Ptb_core.MULLAT=%d" % mullat, "-Ptb_core.NVEC=%d" % nvec,
        os.path.join(here, "tb_core.v")] + [os.path.join(rtl, f) for f in ("bound_core.v", "bound_ram.v", "bound_sha256.v")])
    txt = subprocess.check_output(["vvp", "-n", out], cwd=here).decode()
    print(txt.strip()); return "PASS" in txt

if len(sys.argv) > 1:
    sys.exit(0 if run(*map(int, sys.argv[1:6])) else 1)
ok = True
for aw in (1, 6):
    for rdlat in (1, 2):
        for bm2 in (0, 1):
            for mullat in (1, 2, 3):
                ok &= run(aw, rdlat, bm2, mullat, 12)
ok &= run(12, 1, 0, 2, 4)      # mainnet: i 3 vettori ufficiali + 1
ok &= run(12, 2, 1, 3, 3)
print("TUTTO PASS" if ok else "QUALCOSA FALLISCE"); sys.exit(0 if ok else 1)
