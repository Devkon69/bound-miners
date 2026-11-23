#!/bin/sh
# SOLO CPU (anche senza scheda NVIDIA): tutti i thread, 3 hash per thread.
cd "$(dirname "$0")"
UTENTE=TUO_INDIRIZZO.pc1
POOL=bound.ariabrain.com:3373
CPU="--cpu auto --cpu-lanes 3"
while true; do
  ./bound-gpuminer --cpu-only --user "$UTENTE" --pool "$POOL" $CPU "$@"
  echo "il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)"; sleep 10
done
