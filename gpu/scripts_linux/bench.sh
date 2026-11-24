#!/bin/sh
# velocita' senza rete (20 s) di tutte le GPU; --cpu auto per aggiungere la CPU, --cpu-only solo CPU, --gpu 0 una scheda
cd "$(dirname "$0")"
./bound-gpuminer --bench 20 "$@"
