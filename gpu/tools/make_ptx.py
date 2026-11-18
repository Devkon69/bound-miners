#!/usr/bin/env python3
"""Compila src/bound_kernel.cu in PTX con NVRTC (pip install nvidia-cuda-nvrtc-cu12) e scrive src/kernel_ptx.h.
Non serve il CUDA Toolkit ne' una GPU. Se c'e' ptxas (pip install nvidia-cuda-nvcc-cu12) controlla anche che il
PTX si assembli per le schede vere e stampa registri usati.   python3 tools/make_ptx.py"""
import ctypes, glob, os, site, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here)
# NVRTC_DIR = cartella con libnvrtc.so.12 di una versione precisa (12.4 -> PTX 8.4, driver NVIDIA >= 550);
# senza, quella installata con pip (12.9 -> PTX 8.8, serve un driver piu' recente)
libs = glob.glob(os.environ["NVRTC_DIR"] + "/libnvrtc.so.*") if os.environ.get("NVRTC_DIR") else \
       [p for sp in site.getsitepackages() for p in glob.glob(sp + "/nvidia/cuda_nvrtc/lib/libnvrtc.so.*")]
assert libs, "manca nvidia-cuda-nvrtc-cu12"
os.environ["LD_LIBRARY_PATH"] = os.path.dirname(libs[0]) + ":" + os.environ.get("LD_LIBRARY_PATH", "")
ctypes.CDLL(sorted(glob.glob(os.path.dirname(libs[0]) + "/libnvrtc-builtins.so.*"))[0], mode=ctypes.RTLD_GLOBAL)
rt = ctypes.CDLL(libs[0])
src = open(os.path.join(root, "src", "bound_kernel.cu"), "rb").read()
prog = ctypes.c_void_p()
assert rt.nvrtcCreateProgram(ctypes.byref(prog), src, b"bound_kernel.cu", 0, None, None) == 0
opts = [b"--gpu-architecture=compute_75", b"-std=c++14", b"--fmad=false"]
arr = (ctypes.c_char_p * len(opts))(*opts)
rc = rt.nvrtcCompileProgram(prog, len(opts), arr)
n = ctypes.c_size_t(); rt.nvrtcGetProgramLogSize(prog, ctypes.byref(n))
log = ctypes.create_string_buffer(n.value); rt.nvrtcGetProgramLog(prog, log)
if log.value.strip(): print(log.value.decode())
assert rc == 0, "NVRTC: compilazione fallita"
rt.nvrtcGetPTXSize(prog, ctypes.byref(n)); ptx = ctypes.create_string_buffer(n.value); rt.nvrtcGetPTX(prog, ptx)
ptx = ptx.value
open(os.path.join(root, "src", "bound_kernel.ptx"), "wb").write(ptx)
with open(os.path.join(root, "src", "kernel_ptx.h"), "w") as f:
    f.write("/* generato da tools/make_ptx.py da bound_kernel.cu (NVRTC, compute_75) */\nstatic const char kernel_ptx[] =\n")
    for line in ptx.decode().splitlines():
        f.write('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n')
    f.write(";\n")
head = [l for l in ptx.decode().splitlines() if l.startswith((".version", ".target"))]
print("PTX: %d byte, %s" % (len(ptx), " ".join(head)))
print("letture a 128 bit: %d, scritture a 128 bit: %d, mul.wide/mul.hi: %d" % (ptx.count(b"ld.global.v2.u64"), ptx.count(b"st.global.v2.u64"), ptx.count(b"mul.wide.u32") + ptx.count(b"mul.hi.u64")))
px = [p for sp in site.getsitepackages() for p in glob.glob(sp + "/nvidia/cuda_nvcc/bin/ptxas")]
if px:
    for arch in ("sm_75", "sm_86", "sm_89"):
        r = subprocess.run([px[0], "-arch=" + arch, "-v", "-o", "/dev/null", os.path.join(root, "src", "bound_kernel.ptx")], capture_output=True, text=True)
        info = [l.strip() for l in (r.stdout + r.stderr).splitlines() if "Used" in l or "error" in l.lower() or "spill" in l.lower()]
        print(arch, "ok" if r.returncode == 0 else "ERRORE", "|", " ".join(info)[:200])
