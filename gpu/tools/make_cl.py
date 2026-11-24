#!/usr/bin/env python3
"""Scrive src/kernel_cl.h: il sorgente di src/bound_kernel.cu come stringa C, per il backend OpenCL (il driver della
scheda lo compila all'avvio; il ramo __OPENCL_VERSION__ del sorgente e' quello usato).   python3 tools/make_cl.py"""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "src", "bound_kernel.cu")).read()
with open(os.path.join(root, "src", "kernel_cl.h"), "w") as f:
    f.write("/* generato da tools/make_cl.py da bound_kernel.cu: sorgente del kernel OpenCL */\nstatic const char kernel_cl[] =\n")
    for line in src.splitlines():
        f.write('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n')
    f.write(";\n")
print("kernel_cl.h: %d righe" % len(src.splitlines()))
