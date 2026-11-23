#!/bin/sh
# prova le combinazioni di thread CPU e hash per thread (circa 40 s), poi misura 30 s. Senza GPU: aggiungi --cpu-only
cd "$(dirname "$0")"
./bound-gpuminer --tune-cpu --bench 30 "$@"
