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
# Linux
gcc -std=gnu99 -O2 -Wall -c src/gpubench.c -o /tmp/gb.o
gcc -std=gnu99 -O2 -Wall -c src/gpuminer.c -o /tmp/gm.o
gcc -std=gnu99 -O2 -w -c src/third_party/cJSON.c -o /tmp/cj.o
mkdir -p dist
g++ -o dist/bound-gpubench /tmp/gb.o /tmp/bk.o /tmp/bp.o -lm -ldl
g++ -o dist/bound-gpuminer /tmp/gm.o /tmp/cj.o /tmp/bk.o /tmp/bp.o -lm -ldl
# Windows
Z="python3 -m ziglang"
W="-target x86_64-windows-gnu"
$Z cc  $W -std=gnu99 -O2 -Wall -c src/bound_pow.c -o /tmp/w_bp.o
$Z c++ $W -O2 -DCPU_SHIM -x c++ -c src/bound_kernel.cu -o /tmp/w_bk.o
$Z cc  $W -std=gnu99 -O2 -Wall -c src/gpubench.c -o /tmp/w_gb.o
$Z cc  $W -std=gnu99 -O2 -Wall -c src/gpuminer.c -o /tmp/w_gm.o
$Z cc  $W -std=gnu99 -O2 -w -c src/third_party/cJSON.c -o /tmp/w_cj.o
$Z c++ $W -O2 -s -static -o dist/bound-gpubench.exe /tmp/w_gb.o /tmp/w_bk.o /tmp/w_bp.o
$Z c++ $W -O2 -s -static -o dist/bound-gpuminer.exe /tmp/w_gm.o /tmp/w_cj.o /tmp/w_bk.o /tmp/w_bp.o -lws2_32
ls -la dist
# file .bat del pacchetto Windows: scripts/
# prova del protocollo AriaPool senza GPU:  MINER=dist/bound-gpuminer python3 tools/e2e_pool.py
# prova completa senza GPU (serve boundd con la patch):
#   BOUNDD=.../boundd BOUNDCLI=.../bound-cli python3 tools/e2e.py
