#!/bin/sh
# velocita' senza rete (20 s); --cpu auto per GPU + CPU, --cpu-only per la sola CPU
cd "$(dirname "$0")"
./bound-gpuminer --bench 20 "$@"
