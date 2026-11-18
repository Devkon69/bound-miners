# BOUND (BoundHash) su BlackMiner F2 / F1 Ultra / F1 Plus, BCU1525, FK33

Creato il 16/11/2026. Moneta: BOUND (github.com/Sonic-Bound/Bound, commit e3549ec), lanciata il 16/11/2026,
annuncio bitcointalk.org/index.php?topic=5600075.0. Proof of work proprio, **BoundHash**: scratchpad da
512 KiB per hash (4096 righe da 1024 bit), 8192 passi in catena con round BLAKE2b/BlaMka (Argon2) e una
moltiplicazione 64x64, SHA-256 all'inizio e alla fine. Blocchi da 10 minuti, 50 BOUND.

**Stato (18/11/2026):** bitstream F1 Ultra compilato (7 core, timing chiuso a 90 MHz, WNS +0,158 ns) e provato in
macchina con la BeagleBone: circa 50 kH/s e 30 W per hash board; in overclock regge fino a 100 MHz. Miner
`bound-miner` 1.1 con dev fee 3 %, ponte e patch del nodo provati insieme su regtest. Pacchetti nella cartella
`release/fpga-f1-ultra/`. Non ancora provati: pacchetto per l'interfaccia web, F2 e F1 Plus, mainnet a lungo.

## Build fuori contesto F1 Ultra (Vivado 2026.1, 16/11, `out_f1u_7c_100m_b0r1m2`)
7 core, XC7K420T-2: **798 RAMB36 su 835** (114 per core, come previsto), 91.182 LUT (35 %), 41.346 FF (8 %),
238 DSP48E1 (34 per core, 14 %). Per core: ~12.700 LUT di cui 7.300 nei mux dello scratchpad.
Timing: chiesti 100 MHz, WNS -0,484 ns -> **chiude a 95,4 MHz**. Percorso critico: il BlaMka (2 DSP48E1 in
cascata + somma a 64 bit, 10,45 ns, meta' logica e meta' routing). Potenza stimata 4,8 W per FPGA.
**Hashrate: 7 x 95 MHz / 90.660 = 7.335 H/s per FPGA, circa 44 kH/s per hash board da 6 FPGA.**

## Perche' il conto si fa sulla RAM
Un hash non si mette in pipeline (ogni passo dipende dal precedente e l'indirizzo lo decidono i dati):
si mettono piu' core, e ogni core vuole il suo scratchpad. 4096 x 1024 bit = 114 RAMB36 oppure 15 URAM288.

| Scheda | FPGA | RAM | Core max (RAM) | Note |
|---|---|---|---|---|
| F2 | 6 x XC7VX415T per hash board | 880 RAMB36 | 7 | |
| F1 Ultra | XC7K420T | 835 RAMB36 | 7 | |
| F1 Plus | XC7K325T | 445 RAMB36 | 3 | |
| BCU1525 | XCVU9P | 960 URAM + 2160 RAMB36 | 63 + 18 = 81 | probabile limite le LUT, vedi sotto |
| FK33 | XCVU33P | 160 URAM + 672 RAMB36 | 10 + 5 = 15 | |

Clock per hash (misurati in simulazione): **90.660** con BM2 0, RDLAT 1, MULLAT 2; 168.483 con BM2 1, RDLAT 2,
MULLAT 3. Hashrate per core = MHz x 10^6 / clock per hash: 100 MHz -> 1.100 H/s, 150 -> 1.650, 200 -> 2.200.
Riferimento: i5-10400F circa 5.400 H/s con tutti i thread (docs/boundhash.md del repository).

Con ~12.700 LUT per core (misurati sulla Ultra) 81 core sulla VU9P sarebbero ~1,03 M LUT su 1,18 M: al limite.
Quanti ce ne stanno davvero lo dice la sintesi Vivado per la BCU (comando sotto).

## File
- `model/bound_ref.py`: modello Python riscritto dalla specifica, con la divisione host / FPGA
  (`header_mid`: stato SHA-256 dopo i primi 64 byte dell'header, uguale per tutti i nonce).
  `--selftest` = i 9 vettori ufficiali di tests/test_boundhash.cpp (2, 64 e 4096 righe) -> OK.
  `--const` rigenera `rtl/bound_const.vh`.
- `rtl/bound_core.v`: un core. Parametri `AW` (12 = mainnet), `RAM_STYLE` block / ultra, `RDLAT`, `BM2`, `MULLAT`.
- `rtl/bound_ram.v`: scratchpad (BRAM o URAM). `rtl/bound_sha256.v`: SHA-256 iterativo (8 compressioni per hash).
- `rtl/bound_engine.v`: NCORE core sullo stesso lavoro, **stesse porte di prox_engine / rigor_engine**: JOB da
  209 byte, candidati con handshake, STATUS. Nonce = base + {core 3 bit, posizione 4 bit, contatore 25 bit}.
- `rtl/bound_fpga.v`, `bound_chain_if.v`, `bound_bm_top.v`: da `proxima-f2` (prox_fpga, prox_chain_if, prox_f2_top).
  Cambiano solo il motore, i parametri, la sigla INFO "BND1" v1.0 e il clock di default (25 x 38 / 10 = 95 MHz).
  `mmcm_drp.v`, `uart.v`, `xadc_temp.v` copiati senza modifiche.
- `host/bound_proto.py`: protocollo lato host (da prox_proto.py) con `job_state200` e `fpga_nonce`.
- `docs/PROTOCOLLO.md`: cosa cambia nel JOB / CAND / STATUS rispetto a Proxima.
- `vivado/build.tcl`, `reclock.tcl`, `bound_f1u.xdc`, `bound_f2.xdc`, `bound_f1p.xdc`: da `proxima-f2` (pin identici).
  `vivado/build_ooc.tcl`: build fuori contesto (niente pin, niente bitstream), anche per BCU1525 e FK33.

## Simulazioni (17/11, tutte PASS)
- `python3 sim/run_core.py` (iverilog): 24 configurazioni a 2 e 64 righe con 12 vettori, piu' due a 4096 righe
  con i vettori ufficiali.
- `python3 sim/run_engine.py` (iverilog; anche `RDLAT=2 BM2=1 MULLAT=3`): 3 core, due lavori, candidati veri e
  nessuno perso, arresto con `en`.
- `python3 sim/chain/run_chain.py` (Verilator; anche `RDLAT=2 BM2=1 MULLAT=3`): 3 FPGA x 2 core in catena, ENUM,
  INFO "BND1", lavoro per FPGA e lavoro a tutti (nonce diversi per posizione), ogni candidato ricalcolato dal
  modello e nessuno perso, STATUS, cambio clock via DRP, blocco termico su un FPGA, spegnimento.

## Bitstream F1 Ultra (Vivado Tcl Shell NUOVO)
```
cd PERCORSO/fpga/vivado; set argv {f1u 7 95}; set argc 3; source build.tcl
```
Uscita `out_f1u_7c_95m/bound_f1u.bit`. Il margine a 95 MHz e' di pochi centesimi di ns: se il timing non chiude,
```
cd PERCORSO/fpga/vivado; set argv {f1u 7 95 90}; set argc 4; source reclock.tcl
```
(uscita `out_f1u_7c_95m_at90`, senza ricompilare; 90 MHz = 6.950 H/s per FPGA).
Poi F2: `set argv {7 95}; set argc 2; source build.tcl`. F1 Plus: `set argv {f1p 3 95}; set argc 3; source build.tcl`.
Nota per F2 e F1 Plus: su Proxima un DSP48 senza clock aveva fermato il routing (15/11). Qui i DSP del BlaMka
sono combinatori; sulla Ultra la build fuori contesto si e' instradata. Se su F2 / F1 Plus il routing si ferma
con GLOBAL_LOGIC su DSP_CLK, usare `BM2 1` (prodotti registrati): `set argv {7 175 xc7vx415tffg1157-2 all ExtraNetDelay_high 1}; set argc 6`.

## Build fuori contesto (BCU1525, FK33, prove di timing)
```
cd PERCORSO/fpga/vivado; set argv {bcu 6 3 200 0 2 2 synth}; set argc 8; source build_ooc.tcl
cd PERCORSO/fpga/vivado; set argv {fk33}; set argc 1; source build_ooc.tcl
```
Uscita in `out_<board>_.../RESULT.txt`: WNS, MHz a cui chiude, hash al secondo stimati.

## Miner, ponte e nodo (17/11)
- `host_c/`: **bound-miner 1.0** = rigor-miner 1.0 (csd-miner 1.4.5) passato per `make_bound.py` (una trentina di sostituzioni
  verificate): BoundHash in C (`bound_pow.c`, portabile, 0,3 ms per hash sul PC, selftest con i vettori ufficiali),
  merkle di BOUND, velocita' in H/s (nominale = core x MHz x 10^6 / 90660), lavoro rinnovato ogni 30 s, ordine dei
  byte raw, tetto del clock = build + 15 MHz, `--max-cands 4`, `--lines` per regtest, dev fee 3 % (versione 1.1:
  180 s ogni 100 minuti si presenta al ponte con l'indirizzo dello sviluppatore). Catena, ventole, clock DRP,
  web, API, loader, profili: invariati. Binari: `bound-miner-armv7-1.1-fee3`, `bound-loadfpga-armv7`
  (`make -f bound.mk arm loader`, zig; `FEE=0` per compilare senza fee). Il bitstream dichiara v1.1 (= clock variabile, per il miner).
- `../bridge/bound_bridge.py` + `LEGGIMI_PC.md`: server Stratum per le macchine, verso le due RPC nuove del nodo.
- `../node/`: patch al nodo (93 righe), binari Linux statici gia' compilati, `LEGGIMI_NODO.md`.
- Prova: `BOUNDD=.../boundd BOUNDCLI=.../bound-cli python3 sim/host_e2e.py 40` = bound-miner vero + emulatore di
  6 FPGA (hash dallo stato del JOB, nonce del bitstream) + ponte + `boundd --regtest` con la patch: 26 blocchi
  accettati dal nodo in 40 s, coinbase all'indirizzo giusto, CLK, errore hardware iniettato rilevato -> PASS.
  `BENCH=1 ...` -> PASS (hashrate dai risultati = hashrate dai contatori).

## Cosa manca
1. Prova in macchina del pacchetto per l'interfaccia web (quello BBB e' provato).
2. F2 e F1 Plus: stesso `build.tcl` (`set argv {7 90}; set argc 2` e `set argv {f1p 3 90}; set argc 3`), mai compilati.
3. Top BCU1525 e FK33: non fatti (solo `build_ooc.tcl` per le misure).
4. Una GPU rende molto di piu' su questo algoritmo (RTX 4090: ~350 kH/s): vedi la cartella `gpu/`.

## Rischi
Moneta nata oggi, senza exchange ne' prezzo, codice dichiarato sperimentale e non verificato da terzi.
L'algoritmo non e' fatto contro le GPU (lo dice la documentazione): un miner GPU puo' arrivare.
Un cambio di BoundHash e' un hard fork e butta via il bitstream.
