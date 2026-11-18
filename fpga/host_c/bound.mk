# Makefile - bound-miner e bound-loadfpga per la control board BlackMiner / BBB (ARMv7, musl statico con zig cc).
# Il ponte (pc_bridge/bound_bridge.py) parla stratum+tcp in chiaro: build NO_TLS, come rigor-miner e ting-miner 1.1.
#
#   pip install ziglang
#   make -f bound.mk arm              (-> bound-miner-armv7-1.1-fee3; FEE=0 per la build senza fee)
#   make -f bound.mk native           (PC, per sim/host_e2e.py)
#   make -f bound.mk loader           (bound-loadfpga-armv7, da csd_loadfpga.c)
# bound_main.c si rigenera da rigor_main.c con make_bound.py.
# Dev fee (default 3 %): finestre sullo stesso ponte dell'utente con l'indirizzo dello sviluppatore (DEVFEE_ADDRESS in bound_main.c).

SRC = bound_main.c bound_pow.c csd_pow.c third_party/cJSON.c
CFLAGS += -O2 -Wall -Wno-unused-result -Wno-date-time -std=gnu99 -DNO_TLS
ARMCC ?= ./zcc
FEE ?= 3
VER = 1.1

bound_main.c: rigor_main.c make_bound.py
	python3 make_bound.py rigor_main.c bound_main.c

arm: bound-miner-armv7-$(VER)-fee$(FEE)
bound-miner-armv7-$(VER)-fee$(FEE): $(SRC) bound_pow.h csd_pow.h
	$(ARMCC) $(CFLAGS) -DDEVFEE_PERCENT=$(FEE) -static -s -o $@ $(SRC) -lm

native: bound-miner
bound-miner: $(SRC) bound_pow.h csd_pow.h
	$(CC) $(CFLAGS) -DDEVFEE_PERCENT=$(FEE) -o $@ $(SRC) -lm

loader: bound-loadfpga-armv7
bound-loadfpga-armv7: csd_loadfpga.c
	$(ARMCC) -O2 -Wall -Wno-date-time -std=gnu99 -DPROG='"bound-loadfpga"' -DLOGFILE='"/tmp/bound_loadfpga.log"' -static -s -o $@ csd_loadfpga.c

clean:
	rm -f bound-miner bound-miner-armv7-* bound-loadfpga-armv7
