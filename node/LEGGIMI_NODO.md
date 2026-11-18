# Nodo BOUND con la patch bound-fpga

Il nodo ufficiale (`boundd` 1.0.0, commit e3549ec del 16/11/2026) mina solo con i suoi thread: non ha
getblocktemplate, submitblock ne' stratum. La patch `bound-fpga-rpc.patch` (93 righe, solo `src/rpc/methods.cpp`)
aggiunge due RPC, usate da `pc_bridge/bound_bridge.py`:

- `getminingwork <indirizzo>`: un template che paga l'indirizzo: campi dell'header, coinbase senza il memo da
  8 byte (l'extra nonce, lo mette il miner), ramo merkle. Il nodo tiene gli ultimi 256 template del tip.
- `submitminingwork <id> <memo> <time> <nonce>`: rimette il memo, ricalcola il merkle root, calcola il BoundHash
  e lo restituisce; se sta sotto il target di rete manda il blocco come fa `generatetoaddress`.

Non tocca consenso, rete, wallet: un nodo con la patch e' un nodo normale con due comandi in piu'.

## Binari pronti (Linux x86_64, statici)
`boundd-fpga-linux-x86_64` e `bound-cli-linux-x86_64`: compilati il 17/11/2026 su Ubuntu 24.04 dal commit e3549ec
con la patch. Vanno su Ubuntu 24.04 anche dentro WSL (su altre distribuzioni la ricerca dei seed via DNS di un
binario statico puo' non funzionare: li' conviene compilare).
```
chmod +x boundd-fpga-linux-x86_64 bound-cli-linux-x86_64
./boundd-fpga-linux-x86_64 node                      (mainnet, dati in ~/.bound/mainnet; NON mettere --mine)
./bound-cli-linux-x86_64 getblockchaininfo           (aspettare che sia sincronizzato)
./bound-cli-linux-x86_64 createwallet "una passphrase"      (scrivere le 24 parole!)
./bound-cli-linux-x86_64 getnewaddress               (l'indirizzo B... che incassa i blocchi)
./bound-cli-linux-x86_64 help getminingwork          (c'e' -> e' il nodo con la patch)
```

## Compilare da soli (Ubuntu 24.04 / WSL)
```
sudo apt install build-essential cmake pkg-config libsodium-dev libleveldb-dev libsnappy-dev nlohmann-json3-dev libgtest-dev
git clone https://github.com/Sonic-Bound/Bound && cd Bound && git checkout e3549ec
git apply /percorso/bound-fpga-rpc.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBOUND_GUI=OFF && cmake --build build -j --target boundd bound-cli
```
Se il progetto pubblica una versione nuova: riprovare `git apply` sul commit nuovo (la patch aggiunge un blocco
dopo `generatetoaddress`, di solito si applica uguale).
