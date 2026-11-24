# BOUND miners: BlackMiner FPGA + NVIDIA GPU

Miners for **BOUND** (proof of work: BoundHash, https://github.com/Sonic-Bound/Bound), everything needed to mine
solo on your own node:

| Folder | What it is |
|---|---|
| `gpu/` | **bound-gpuminer**: CUDA miner for NVIDIA cards, Windows and Linux. About 348 kH/s on an RTX 4090. |
| `fpga/` | Bitstream (Verilog), host miner **bound-miner** and packages for the **BlackMiner F1 Ultra** hash boards. About 42-50 kH/s and 50 W per hash board. |
| `bridge/` | **bound_bridge.py**: Stratum server for the miners, talks to your node (Python 3, no packages). |
| `node/` | Patch for the BOUND node (`boundd`) that adds two RPC commands for external miners. |
| `release/` | Ready-to-use binaries and packages built from this source. |

Two ways to mine:

- **Pool (GPU miner from 0.4, BlackMiner miner from 1.2):** AriaPool, `bound.ariabrain.com:3373`. Nothing else to run:
  `bound-gpuminer.exe --user YOUR_BOUND_ADDRESS.pc1 --pool bound.ariabrain.com:3373`; on a BlackMiner the packages come
  with that pool already set, you only enter your address.
- **Solo:** you run a node, the bridge next to it, and point the miners (GPU or BlackMiner) at the bridge.

```
GPU miner / BlackMiner  --Stratum, port 7340-->  bound_bridge.py  --RPC, 127.0.0.1-->  boundd (patched)
```

Most of the detailed documentation in the folders is in Italian (`LEGGIMI*`).

## Quick start

**1. Node** (Linux x86_64 or WSL, Ubuntu 24.04). Use `release/node/boundd-fpga-linux-x86_64`, or build it yourself
(`node/LEGGIMI_NODO.md`):
```
./boundd-fpga-linux-x86_64 node                 # no --mine
./bound-cli-linux-x86_64 createwallet "a passphrase"      # write down the 24 words
./bound-cli-linux-x86_64 getnewaddress          # your B... address
```
**2. Bridge**, on the same machine as the node:
```
python3 bridge/bound_bridge.py                  # listens on 0.0.0.0:7340
```
Every miner mines for the address in its own `--user ADDRESS.worker`.

**3a. GPU** (`release/gpu/`, NVIDIA driver 550 or newer, no CUDA Toolkit needed):
```
bound-gpuminer.exe --bench 20
bound-gpuminer.exe --user YOUR_BOUND_ADDRESS.pc1 --pool IP_OF_THE_BRIDGE:7340        (solo, your bridge)
bound-gpuminer.exe --user YOUR_BOUND_ADDRESS.pc1 --pool bound.ariabrain.com:3373     (AriaPool)
```
The pool does not speak Stratum v1 but its own JSON protocol (`login` / `job` / `submit` with the claimed hash); the miner
detects it by itself. The protocol as observed is written down in `gpu/tools/mock_ariapool.py`, a mock pool used by the
tests. `--tune` measures at start-up how many hashes in flight give the best rate on your card (the default, 8192, is
the best value measured on an RTX 4090).

**AMD, Intel and several GPUs (from 0.6).** NVIDIA cards run with CUDA, AMD and Intel GPUs (also the graphics integrated
in the processor) with OpenCL, loaded at run time from the card's driver. By default the miner uses every GPU it finds,
each in its own thread, together with the CPU (`--cpu auto`); all of them take nonces from one shared counter. `--list-gpus`
lists them, `--gpu 0,2` picks some, `--no-cuda` runs NVIDIA cards through OpenCL. The OpenCL kernel is the same source as
the CUDA one (`src/bound_kernel.cu`, `__OPENCL_VERSION__` branch), compiled by the driver at start-up and checked against
the CPU. Tested on the pool with three parts at once on one PC: RTX 3060 (CUDA) + Radeon RX 6600 (OpenCL) + 12-thread
CPU (AVX2), about 152 kH/s (62 + 37 + 52), shares accepted. Intel GPUs are not tested yet. On cards other than the RTX 4090,
run `--tune` once to find the best number of hashes in flight for each card.

**GPU + CPU (from 0.5).** `--cpu auto` adds CPU threads next to the GPU (all logical CPUs but one), `--cpu-only` mines with
the CPU alone, also on a PC without an NVIDIA card (without a usable GPU the miner falls back to the CPU by itself). Launch
scripts for the three modes: `avvia_pool*.bat` on Windows, `avvia_pool*.sh` on Linux. Measured with an RTX 4090 and a
Ryzen 9 7950X3D on the pool: GPU alone ~349 kH/s, CPU alone ~234 kH/s, both together ~563 kH/s (GPU 351 + CPU 212). A BoundHash scratchpad (512 KiB) fits in the CPU caches, so the CPU is limited by its cores, not by RAM:
the miner uses AVX-512 or AVX2 and lets every thread work on 2-4 hashes at once (`--cpu-lanes`, default 3) so that their
latencies overlap. `--tune-cpu` tries thread counts and lanes and keeps the best. GPU and CPU take their nonces from the
same counter, CPU threads run at low priority, and every CPU share is re-checked before it is sent. Measured on one core
of a Ryzen 9 7950X3D (2 threads): about 15 kH/s in the benchmark, 22 kH/s on the real pool (CPU only, 32 shares accepted,
0 rejected).
**3b. BlackMiner F1 Ultra** (`release/fpga-f1-ultra/`):
- `upgrade_altcoin_F1Ultra_bound_90_fee3.tar.gz`: package for the vendor web interface (Configuration > Upgrade),
  then choose the coin `bound`, pool `stratum+tcp://IP_OF_THE_BRIDGE:7340`, user `YOUR_BOUND_ADDRESS.worker`.
- `bound-ultra-bbb-fee3.zip`: package for hash boards driven by a BeagleBone Black rev C.
- `bound_f1u_7c_90m.bit`, `bound-miner-armv7-1.2-fee3`, `bound-loadfpga-armv7`: the single pieces.

**3c. BlackMiner F2, F1 Plus, F1** (web-interface packages, same use as the F1 Ultra one; tested on the machines):
- `release/fpga-f2/upgrade_altcoin_F2_bound_90_fee3.tar.gz` and the bitstream `bound_f2_7c_90m.bit`.
- `release/fpga-f1/upgrade_altcoin_F1Plus_bound_90_fee3.tar.gz`, `upgrade_altcoin_F1_bound_90_fee3.tar.gz` and the
  bitstream `bound_f1p_3c_90m.bit` (F1 and F1 Plus have the same FPGA).
- Miner and loader are the same ARM binaries as the F1 Ultra.

Shares only measure the hashrate: this is solo mining, only a whole block pays (the bridge prints it).

## Dev fee

Both miners have a **3 % dev fee**, declared at start-up and in `--help`: for 180 seconds every 100 minutes (the
first time after 20 minutes) the miner logs in to *your* bridge, or to the pool, with the developer's address
`BW1chX39ABzxeVgHqqmTDGL5woHd6GAgj2rqVm4iw4Ty5fG7K1Y7`, so what it finds in that window pays the developer.
The benchmarks have no fee. The source is here: build with `-DDEVFEE_PERCENT=0` (GPU) or `FEE=0` (FPGA) to remove it.

## Performance

| Hardware | Hashrate | Notes |
|---|---|---|
| RTX 4090 | 348 kH/s | measured, 8192 hashes in flight (4 GiB of VRAM), default settings |
| BlackMiner F1 Ultra, one hash board (6 x XC7K420T) | 42-50 kH/s, ~50 W | timing closed at 90 MHz (41.7 kH/s nominal); runs up to 100 MHz in overclock (46.3 kH/s nominal); about 50 kH/s and 50 W reported on a real board |
| CPU, Ryzen 9 7950X3D (31 threads, 3 hashes per thread) | 234 kH/s alone, 212 kH/s next to the GPU | AVX-512; `--tune-cpu` picks the threads and hashes per thread |
| RTX 3060 + Radeon RX 6600 + 12-thread CPU together | about 152 kH/s on AriaPool | GPU0 62 (CUDA) + GPU1 37 (OpenCL) + CPU 52; default batch, not tuned |
| RTX 4090 + Ryzen 9 7950X3D together | about 563 kH/s on AriaPool | GPU 351 + CPU 212; 274 shares accepted, 0 rejected |
| BlackMiner F2, one hash board (6 x XC7VX415T) | 41.7 kH/s nominal | 7 cores per FPGA, timing closed at 90 MHz (WNS +0.043 ns); runs on a real machine |
| BlackMiner F1 / F1 Plus (XC7K325T) | 2.98 kH/s per FPGA nominal | 3 cores per FPGA (block RAM limit), timing closed at 90 MHz (WNS +0.363 ns); runs on a real machine |

BoundHash moves about 2.1 MB of memory per hash (a 512 KiB scratchpad, 16,384 random 128-byte accesses), so the
hashrate follows memory bandwidth: GPUs win by a wide margin. On the FPGA every core needs its own scratchpad in
block RAM (114 RAMB36): 7 cores fit in an XC7K420T.

## What has been tested, and what has not

- BoundHash implementations (Python model, C, Verilog, CUDA kernel) against the official test vectors of the node.
- FPGA: core, engine and a chain of FPGAs in simulation; the F1 Ultra bitstream on real hardware with the BeagleBone package;
  the F2 and F1 / F1 Plus bitstreams, miner 1.1 and their web-interface packages on real machines (21 Nov 2026).
- GPU: the kernel on a real RTX 4090 (hashes identical to the CPU).
- AriaPool protocol: the GPU miner (kernel on the CPU) and the BlackMiner miner 1.2 (emulated FPGA chains) against the
  real pool (login, jobs, accepted shares, 22 Nov 2026), and against a mock pool that recomputes every hash (shares, fee
  windows, reconnection, no nonce tried twice across boards).
- Miners + bridge + patched node end to end on **regtest** (blocks accepted by the node, fee windows paying the fee
  address, reconnection). Done with the real FPGA miner against an emulated chain, and with the GPU miner running
  its kernel on the CPU.
- **Not tested yet:** the F1 Ultra web-interface upgrade package on a machine (the F2 and F1 ones are tested),
  networking of the GPU miner on Windows, the full GPU miner on a real card against the pool, BlackMiner miner 1.2 on a
  real machine (1.1 is the one tested), the pool's deferred verification of the shares, long runs on mainnet. Use at your own risk.

## Building

- GPU: `gpu/build.sh` (zig as cross-compiler, NVRTC from pip to compile the kernel to PTX; no CUDA Toolkit, no GPU).
- FPGA bitstream: Vivado, `fpga/vivado/build.tcl` (`set argv {f1u 7 90}; set argc 3; source build.tcl`).
- FPGA host miner: `fpga/host_c/bound.mk` (`bound_main.c` is generated from `rigor_main.c` by `make_bound.py`).
- Packages: `fpga/pacchetto_web/build_pkg.py` and `fpga/pacchetto_bbb/build_bbb.py`; they start from an existing
  package of the same family (RIGOR / Proxima), which is not in this repository.
- Node: `node/bound-fpga-rpc.patch` on BOUND 1.1.0 (commit e3549ec).

## License

MIT, see `LICENSE`.

## Third-party code

- BOUND node: MIT license, (c) its authors; `node/bound-fpga-rpc.patch` is a patch to it and `release/node/` are
  builds of it with the patch.
- cJSON (`*/third_party/`): MIT license, see `cJSON_LICENSE`.
- BlackMiner pin constraints (`fpga/vivado/*.xdc`) come from the vendor's published pinout.
