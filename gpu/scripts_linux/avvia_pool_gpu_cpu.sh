#!/bin/sh
# tutte le GPU (NVIDIA, AMD, Intel) + CPU insieme. --cpu auto = tutti i thread logici meno uno per ogni GPU.
cd "$(dirname "$0")"
UTENTE=TUO_INDIRIZZO.pc1
POOL=bound.ariabrain.com:3373
CPU="--cpu auto --cpu-lanes 3"
while true; do
  ./bound-gpuminer --user "$UTENTE" --pool "$POOL" $CPU "$@"
  echo "il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)"; sleep 10
done
