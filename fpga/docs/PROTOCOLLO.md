# Protocollo seriale BOUND (bitstream "BND1" v1.1)

Trama, CRC, catena, ENUM, CTRL, STATUS, INFO e CLK sono quelli di Proxima / RIGOR (`proxima-f2/docs/PROTOCOL.md`):
`bound_chain_if.v` e' `prox_chain_if.v` con la sola sigla cambiata. 115200 baud, 8N1, una linea per hash board.
Cambia solo il significato del JOB, del CAND e di due campi di STATUS / INFO.

## JOB (0x02, 209 byte come gli altri)
| byte | contenuto |
|---|---|
| 0 | `job_id` |
| 1..8 | `target64`, big-endian: i 64 bit alti del target a 256 bit (target >> 192) |
| 9..40 | `mid`: stato SHA-256 (8 parole big-endian) dopo il blocco tag||tag e i primi 64 byte dell'header |
| 41..52 | byte 64..75 dell'header (ultimi 4 byte del merkle root, time, bits) |
| 53..56 | base del nonce, little-endian (di solito 0) |
| 57..208 | zero |

`mid` lo calcola l'host una volta per header: `header_mid()` in `model/bound_ref.py`, cioe' una compressione
SHA-256 dei primi 64 byte dell'header partendo dalla costante `MID_SEED` (`rtl/bound_const.vh`).
Python: `bound_proto.job_state200(mid, header[64:76], base)` e `cmd_job(id, target64, stato)`.

## Nonce
Il core `c` (0..7) dell'FPGA in posizione `pos` prova `base + {c (3 bit), pos (4 bit), contatore (25 bit)}`.
Un JOB mandato a tutti (ADDR 0xFF) da' quindi nonce diversi a ogni FPGA della catena. Per hash board diverse
l'host cambia l'header (o la base). Il contatore dura ore (~1 kH/s per core): il lavoro non si esaurisce,
resta valido finche' non arriva un JOB nuovo o CTRL con "cancella job".
JOB nuovo: ogni core finisce l'hash in corso (1 ms), lo scarta e riparte dal contatore 0.

## CAND (0x81)
`job_id`(1), `nonce`(4, LE: il nonce intero dell'header), `core << 6`(2), `top24`(3, LE: i 24 bit alti dell'hash).
L'FPGA lo manda quando `hash >> 192 <= target64`. L'host ricalcola l'hash intero e lo confronta col target.

## STATUS (0x82) e INFO (0x83)
- `iter` = hash avviati dal core 0 in questo lavoro (il bit 29 "finito" non si accende mai).
  Velocita' di un FPGA = `core x delta iter / delta t` hash al secondo; nominale `core x MHz x 10^6 / 90660`.
- INFO: `"BND1"`, 1, 1 (v1.1 = clock variabile, come gli altri bitstream), numero di core, 12 (log2 delle righe dello scratchpad), MHz attuali, MHz di build / 5.
- CLK: clock hash = 25 MHz x M / O come sempre; per BOUND servono 75..100 MHz (es. 95 = M 38, O 10).
