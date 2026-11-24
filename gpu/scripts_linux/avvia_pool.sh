#!/bin/sh
# solo GPU (tutte quelle trovate: NVIDIA, AMD, Intel). Metti il tuo indirizzo BOUND al posto di TUO_INDIRIZZO; pc1 = nome di questo PC.
cd "$(dirname "$0")"
UTENTE=TUO_INDIRIZZO.pc1
POOL=bound.ariabrain.com:3373
while true; do
  ./bound-gpuminer --user "$UTENTE" --pool "$POOL" "$@"
  echo "il miner si e' fermato: riparto tra 10 secondi (Ctrl-C per uscire)"; sleep 10
done
