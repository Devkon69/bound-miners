#!/bin/sh
# elenca le GPU trovate (NVIDIA con CUDA, AMD e Intel con OpenCL) con il numero da usare in --gpu
cd "$(dirname "$0")"
./bound-gpuminer --list-gpus
