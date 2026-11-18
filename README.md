# BOUND miners: BlackMiner FPGA + NVIDIA GPU

Miners for **BOUND** (proof of work: BoundHash, https://github.com/Sonic-Bound/Bound), everything needed to mine
solo on your own node:

| Folder | What it is |
|---|---|
| `gpu/` | **bound-gpuminer**: CUDA miner for NVIDIA cards, Windows and Linux. About 348 kH/s on an RTX 4090. |
| `fpga/` | Bitstream (Verilog), host miner **bound-miner** and packages for the **BlackMiner F1 Ultra** hash boards. About 42-50 kH/s and 30 W per hash board. |
| `bridge/` | **bound_bridge.py**: Stratum server for the miners, talks to your node (Python 3, no packages). |
| `node/` | Patch for the BOUND node (`boundd`) that adds two RPC commands for external miners. |
| `release/` | Ready-to-use binaries and packages built from this source. |

BOUND has no pool: you run a node, the bridge next to it, and point the miners at the bridge.

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
bound-gpuminer.exe --user YOUR_BOUND_ADDRESS.pc1 --pool IP_OF_THE_BRIDGE:7340
```
**3b. BlackMiner F1 Ultra** (`release/fpga-f1-ultra/`):
- `upgrade_altcoin_F1Ultra_bound_90_fee3.tar.gz`: package for the vendor web interface (Configuration > Upgrade),
  then choose the coin `bound`, pool `stratum+tcp://IP_OF_THE_BRIDGE:7340`, user `YOUR_BOUND_ADDRESS.worker`.
- `bound-ultra-bbb-fee3.zip`: package for hash boards driven by a BeagleBone Black rev C.
- `bound_f1u_7c_90m.bit`, `bound-miner-armv7-1.1-fee3`, `bound-loadfpga-armv7`: the single pieces.

Shares only measure the hashrate: this is solo mining, only a whole block pays (the bridge prints it).

## Dev fee

Both miners have a **3 % dev fee**, declared at start-up and in `--help`: for 180 seconds every 100 minutes (the
first time after 20 minutes) the miner logs in to *your* bridge with the developer's address
`BW1chX39ABzxeVgHqqmTDGL5woHd6GAgj2rqVm4iw4Ty5fG7K1Y7`, so a block found in that window pays the developer.
The benchmarks have no fee. The source is here: build with `-DDEVFEE_PERCENT=0` (GPU) or `FEE=0` (FPGA) to remove it.

## Performance

| Hardware | Hashrate | Notes |
|---|---|---|
| RTX 4090 | 348 kH/s | measured, 8192 hashes in flight (4 GiB of VRAM), default settings |
| BlackMiner F1 Ultra, one hash board (6 x XC7K420T) | 42-50 kH/s, ~30 W | timing closed at 90 MHz (41.7 kH/s nominal); runs up to 100 MHz in overclock (46.3 kH/s nominal); about 50 kH/s and 30 W reported on a real board |

BoundHash moves about 2.1 MB of memory per hash (a 512 KiB scratchpad, 16,384 random 128-byte accesses), so the
hashrate follows memory bandwidth: GPUs win by a wide margin. On the FPGA every core needs its own scratchpad in
block RAM (114 RAMB36): 7 cores fit in an XC7K420T.

## What has been tested, and what has not

- BoundHash implementations (Python model, C, Verilog, CUDA kernel) against the official test vectors of the node.
- FPGA: core, engine and a chain of FPGAs in simulation; the F1 Ultra bitstream on real hardware with the BeagleBone package.
- GPU: the kernel on a real RTX 4090 (hashes identical to the CPU).
- Miners + bridge + patched node end to end on **regtest** (blocks accepted by the node, fee windows paying the fee
  address, reconnection). Done with the real FPGA miner against an emulated chain, and with the GPU miner running
  its kernel on the CPU.
- **Not tested yet:** the web-interface upgrade package on a machine, the F2 and F1 Plus builds (same scripts,
  never compiled), networking of the GPU miner on Windows, long runs on mainnet. Use at your own risk.

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
