#!/bin/sh
# build.sh - bound-gpubench per Windows (cross-compilato con zig) e per Linux. Non serve il CUDA Toolkit.
#   pip install ziglang nvidia-cuda-nvrtc-cu12==12.4.127      (NVRTC 12.4 -> PTX 8.4, driver NVIDIA >= 550)
#   NVRTC_DIR=<cartella con libnvrtc.so.12 della 12.4> sh build.sh
set -e
cd "$(dirname "$0")"
python3 tools/make_ptx.py                       # src/bound_kernel.cu -> src/kernel_ptx.h
# prova del kernel sulla CPU contro bound_pow.c
gcc -std=gnu99 -O2 -c src/bound_pow.c -o /tmp/bp.o
g++ -O2 -DCPU_SHIM -x c++ -c src/bound_kernel.cu -o /tmp/bk.o
g++ -O2 -o /tmp/test_kernel tools/test_kernel.cpp /tmp/bk.o /tmp/bp.o -lm && /tmp/test_kernel
# BoundHash sulla CPU (thread del miner): tutte le versioni (scalare, AVX2, AVX-512) contro bound_pow.c, e velocita'
#   gcc -O2 -Isrc -o /tmp/cpubench tools/cpubench.c src/cpu_hash.c src/bound_pow.c -lpthread && /tmp/cpubench THREAD HASH_PER_THREAD
# Linux
gcc -std=gnu99 -O2 -Wall -c src/gpubench.c -o /tmp/gb.o
gcc -std=gnu99 -O2 -Wall -c src/gpuminer.c -o /tmp/gm.o
gcc -std=gnu99 -O2 -Wall -c src/cpu_hash.c -o /tmp/ch.o
gcc -std=gnu99 -O2 -w -c src/third_party/cJSON.c -o /tmp/cj.o
mkdir -p dist
g++ -o dist/bound-gpubench /tmp/gb.o /tmp/bk.o /tmp/bp.o -lm -ldl
g++ -o dist/bound-gpuminer /tmp/gm.o /tmp/ch.o /tmp/cj.o /tmp/bk.o /tmp/bp.o -lm -ldl -lpthread
# Windows
Z="python3 -m ziglang"
W="-target x86_64-windows-gnu"
$Z cc  $W -std=gnu99 -O2 -Wall -c src/bound_pow.c -o /tmp/w_bp.o
$Z c++ $W -O2 -DCPU_SHIM -x c++ -c src/bound_kernel.cu -o /tmp/w_bk.o
$Z cc  $W -std=gnu99 -O2 -Wall -c src/gpubench.c -o /tmp/w_gb.o
$Z cc  $W -std=gnu99 -O2 -Wall -c src/gpuminer.c -o /tmp/w_gm.o
$Z cc  $W -std=gnu99 -O2 -Wall -c src/cpu_hash.c -o /tmp/w_ch.o
$Z cc  $W -std=gnu99 -O2 -w -c src/third_party/cJSON.c -o /tmp/w_cj.o
$Z c++ $W -O2 -s -static -o dist/bound-gpubench.exe /tmp/w_gb.o /tmp/w_bk.o /tmp/w_bp.o
$Z c++ $W -O2 -s -static -o dist/bound-gpuminer.exe /tmp/w_gm.o /tmp/w_ch.o /tmp/w_cj.o /tmp/w_bk.o /tmp/w_bp.o -lws2_32
ls -la dist
# kernel OpenCL (AMD, Intel): src/kernel_cl.h e' bound_kernel.cu come stringa:  python3 tools/make_cl.py
# prova del kernel OpenCL (anche senza GPU, con POCL: apt install pocl-opencl-icd ocl-icd-libopencl1):
#   gcc -O2 -Isrc tools/test_ocl.c src/bound_pow.c -ldl -o /tmp/test_ocl && BOUND_OPENCL_CPU=1 /tmp/test_ocl 4096 128
# piu' schede finte con una: BOUND_OPENCL_CPU=1 BOUND_OPENCL_DUP=2 E2E_REAL_GPU=1 MINER=... python3 tools/e2e_pool.py 25 --cpu 1
# file .bat del pacchetto Windows: scripts/   script .sh del pacchetto Linux: scripts_linux/
# prova del protocollo AriaPool senza GPU:  MINER=dist/bound-gpuminer python3 tools/e2e_pool.py [S] [--cpu 2 | --no-gpu --cpu 2]
# prova completa senza GPU (serve boundd con la patch):
#   BOUNDD=.../boundd BOUNDCLI=.../bound-cli python3 tools/e2e.py
