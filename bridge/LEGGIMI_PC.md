# Ponte PC per BOUND (bound_bridge.py)

```
macchina FPGA (bound-miner)  --stratum, porta 7340-->  bound_bridge.py  --RPC 127.0.0.1-->  boundd con la patch
```
1. Nodo: vedi `node_patch/LEGGIMI_NODO.md`. Deve essere sincronizzato e con il suo miner interno fermo.
2. Ponte, sulla stessa macchina del nodo (stesso utente: legge `~/.bound/mainnet/.cookie`), Python 3 senza pacchetti:
   ```
   python3 bound_bridge.py
   ```
   Ogni miner mina per l'indirizzo del suo `--user INDIRIZZO.nome` (il ponte chiede al nodo un template per
   ciascun indirizzo collegato). Opzioni: `--address B...` = indirizzo di riserva per i miner il cui utente non e'
   un indirizzo valido, `--listen 0.0.0.0:7340`, `--datadir`, `--rpc 127.0.0.1:7332` (testnet 18332, regtest 18543),
   `--share-s 15`, `--refresh 20`.
3. Miner: `--pool stratum+tcp://IP_DEL_PC:7340 --user B...IL_TUO_INDIRIZZO.nome` (BlackMiner: nella pagina Pool/Miner
   o in bound.conf; GPU: `bound-gpuminer --user ... --pool IP_DEL_PC:7340`).
4. Se il nodo e il ponte girano in WSL: la porta 7340 va inoltrata da Windows a WSL e aperta nel firewall,
   con questi comandi in PowerShell come amministratore (da rifare se l'IP di WSL cambia):
   ```
   $ip = (wsl hostname -I).Trim().Split(' ')[0]
   netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=7340 connectaddress=$ip connectport=7340
   New-NetFirewallRule -DisplayName "BOUND ponte 7340" -Direction Inbound -Protocol TCP -LocalPort 7340 -Action Allow
   ```

Nel log del ponte: `nodo: blocco N da minare`, `macchina collegata`, ogni minuto `stato: ... share ... blocchi ...
kH/s medi dalle share`, e `*** BLOCCO N ACCETTATO` quando una share sta sotto il target di rete.
Le share servono solo a misurare l'hashrate: e' mining in solitaria, paga solo il blocco intero (50 BOUND).

Dev fee: e' nei miner, non nel ponte. Per 180 s ogni 100 minuti un miner si presenta con l'indirizzo dello
sviluppatore (utente `B....devfee`): nel log del ponte si vede `mina per BW1ch...` e poi di nuovo il tuo indirizzo.
