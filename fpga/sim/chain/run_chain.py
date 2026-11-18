#!/usr/bin/env python3
"""
run_chain.py - BOUND, simulazione di sistema (Verilator): N FPGA cablati come sulla hash board,
comandati con il protocollo seriale. Scratchpad da 64 righe (AW=6) per avere molti hash in pochi ms.

Controlli: ENUM + INFO ("BND1"), lavoro per FPGA (header diversi), lavoro a tutti (stesso header, nonce
diversi per posizione), ogni candidato ricalcolato con il modello, nessun candidato perso (tutti quelli
del modello fino all'ultimo contatore visto per core), STATUS, cambio clock (DRP), blocco termico su un
FPGA, disabilitazione globale.

  python3 sim/chain/run_chain.py            RDLAT=2 BM2=1 MULLAT=3 python3 sim/chain/run_chain.py
"""
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "model"))
sys.path.insert(0, os.path.join(ROOT, "host"))
import bound_ref as M
import bound_proto as P

N, NCORE, AW, CPB = 3, 2, 6, 8
RDLAT, BM2, MULLAT = (int(os.environ.get(k, d)) for k, d in (("RDLAT", 1), ("BM2", 0), ("MULLAT", 2)))
LINES = 1 << AW
TGT = (1 << 64) // 48 - 1                  # un candidato ogni 48 hash: la seriale resta libera
CW = 32 - 3 - 4
BYTE_NS = CPB * 10 * 10
BASE2 = 0x00A00000                         # base del nonce del lavoro a tutti


def hash_num(header76, nonce):
    return int(M.display(M.bound_hash(header76 + nonce.to_bytes(4, "little"), LINES)), 16)


def main():
    rtl = os.path.join(ROOT, "rtl")
    heads = {(0x11, p): M.vector_header(1000 + p)[:76] for p in range(N)}
    hb = M.vector_header(2000)[:76]        # lavoro a tutti
    def job(h, base=0):
        return P.job_state200(M.header_mid(h + bytes(4)), h[64:76], base)
    T_JOB1, T_ST1, T_JOB2, T_ST2 = 150_000, 2_600_000, 2_700_000, 5_000_000
    T_CLK = T_ST2 + 80_000
    T_TRIP, T_DIS, T_END = 5_600_000, 7_400_000, 7_800_000
    S = []
    def send(t, data):
        S.append(f"        wait_until({t});")
        S.extend(f"        tx(8'h{b:02x});" for b in data)
    send(1_000, P.cmd_enum(0))
    send(20_000, P.cmd_info())
    send(120_000, P.cmd_ctrl(True, trip_c=90, resume_c=80))
    t = T_JOB1
    for p in range(N):
        send(t, P.cmd_job(0x11, TGT, job(heads[(0x11, p)]), addr=p)); t += 220 * BYTE_NS
    send(T_ST1, P.cmd_status())
    send(T_JOB2, P.cmd_job(0x22, TGT, job(hb, BASE2), addr=P.BROADCAST))
    send(T_ST2, P.cmd_status())
    send(T_CLK, P.cmd_clk(75))                         # clock variabile: 25 * 24 / 8 = 75 MHz
    send(T_CLK + 250_000, P.cmd_info())
    S.append(f"        wait_until({T_TRIP}); set_temp(2, {P.degc_to_code(105)});")
    send(T_TRIP + 20_000, P.cmd_job(0x33, TGT, job(heads[(0x11, 0)]), addr=P.BROADCAST))
    send(T_TRIP + 1_500_000, P.cmd_status())
    send(T_DIS, P.cmd_ctrl(False))
    send(T_DIS + 150_000, P.cmd_status())
    S.append(f"        wait_until({T_END});")
    open(os.path.join(HERE, "stim.vh"), "w").write("\n".join(S) + "\n")

    obj = os.path.join(HERE, "obj_chain")
    src = [os.path.join(HERE, "tb_chain.v"), os.path.join(HERE, "chain_top.v")] + [
        os.path.join(rtl, f) for f in ("bound_fpga.v", "bound_chain_if.v", "bound_engine.v", "bound_core.v",
                                       "bound_ram.v", "bound_sha256.v", "mmcm_drp.v", "uart.v", "xadc_temp.v")]
    cmd = ["verilator", "--binary", "--timing", "-DSIM", "-Wno-fatal", "-Wno-WIDTHEXPAND", "-Wno-WIDTHTRUNC",
           "-Wno-UNUSEDSIGNAL", "-Wno-INITIALDLY", "-Wno-MULTIDRIVEN", "--top-module", "tb_chain",
           f"-GN={N}", f"-GNCORE={NCORE}", f"-GAW={AW}", f"-GRDLAT={RDLAT}", f"-GBM2={BM2}", f"-GMULLAT={MULLAT}",
           f"-GCPB={CPB}", "-I" + HERE, "-I" + rtl, "--Mdir", obj, "-o", "tbchain"] + src
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-4000:]); sys.exit(1)
    t0 = time.time()
    print(f"simulo {T_END / 1e6:.1f} ms ({N} FPGA x {NCORE} core, {LINES} righe, RDLAT={RDLAT} BM2={BM2} MULLAT={MULLAT}) ...", flush=True)
    r = subprocess.run([os.path.join(obj, "tbchain")], cwd=HERE, capture_output=True, text=True)
    print(f"  fatto in {time.time() - t0:.0f} s")
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:]); sys.exit(1)

    df = P.Deframer(); resp = []
    for line in open(os.path.join(HERE, "rx.log")):
        _, ns, bh = line.split()
        for x in df.feed(bytes([int(bh, 16)])):
            x.t = int(ns); resp.append(x)
    fails = []
    def check(cond, msg):
        print(("  ok   " if cond else "  FAIL ") + msg)
        if not cond: fails.append(msg)

    infos = [x for x in resp if x.type == P.R_INFO]
    check(sorted(x.pos for x in infos if x.t < T_ST1) == list(range(N)), f"INFO dalle posizioni {sorted(x.pos for x in infos if x.t < T_ST1)}")
    check(all(x.magic == b"BND1" and x.cores == NCORE and x.rounds == AW and x.version == (1, 1) for x in infos),
          "INFO: BND1 v1.1, core, log2 righe")
    late_info = [x for x in infos if T_CLK + 250_000 < x.t < T_DIS]
    check(len(late_info) == N and all(x.mhz == 75 for x in late_info),
          f"CLK 75 MHz: INFO dice {[x.mhz for x in late_info]} (build {infos[0].built_mhz})")
    check("DRP_ERR" not in r.stdout and "DRP_W" in r.stdout, "DRP: scritture solo con l'MMCM in reset")

    cands = [x for x in resp if x.type == P.R_CAND]
    def analyse(jid, header_of, base, t_from, t_to, positions, after_reset=False):
        """ogni candidato vero e al suo posto; nessuno perso fino all'ultimo contatore visto per core.
        after_reset: il dominio hash e' ripartito da zero dentro la finestra; i candidati gia' in viaggio
        (contatori alti) si controllano e basta, la completezza si guarda dall'ultima ripartenza in poi."""
        for p in positions:
            mine = [c for c in cands if c.job_id == jid and c.pos == p and t_from <= c.t < t_to]
            h = header_of(p); bad = 0; seen = {}
            for c in mine:
                off = (c.nonce - base) & 0xFFFFFFFF
                core, cpos, ctr = off >> (CW + 4), (off >> CW) & 15, off & ((1 << CW) - 1)
                v = hash_num(h, c.nonce)
                if (v >> 192) > TGT or c.top24 != (v >> 232) or cpos != p or core != c.core or core >= NCORE:
                    bad += 1; print("    candidato sbagliato", c)
                seen.setdefault(core, []).append(ctr)
            lost = 0; nexp = 0
            for core, ctrs in seen.items():
                if after_reset:
                    cut = max([i for i in range(1, len(ctrs)) if ctrs[i] < ctrs[i - 1]], default=None)
                    if cut is None: continue                     # non si vede la ripartenza: solo candidati veri
                    ctrs = ctrs[cut:]
                exp = [k for k in range(max(ctrs) + 1) if (hash_num(h, P.fpga_nonce(base, core, p, k)) >> 192) <= TGT]
                nexp += len(exp)
                if ctrs != exp: lost += 1
            check(bad == 0 and lost == 0 and (len(mine) > 0 or after_reset),
                  f"lavoro {jid:#x} FPGA {p}{' dopo il cambio di clock' if after_reset else ''}: {len(mine)} candidati, tutti veri, "
                  f"{nexp} attesi dal modello, nessuno perso")
    analyse(0x11, lambda p: heads[(0x11, p)], 0, 0, T_JOB2, range(N))
    # il cambio di clock resetta il dominio hash e ricarica il lavoro 0x22 da capo (contatori da zero)
    analyse(0x22, lambda p: hb, BASE2, 0, T_CLK, range(N))
    analyse(0x22, lambda p: hb, BASE2, T_CLK, T_TRIP + 20_000, range(N), after_reset=True)
    analyse(0x33, lambda p: heads[(0x11, 0)], 0, T_TRIP, T_END, (0, 1))
    check(not [c for c in cands if c.job_id == 0x33 and c.pos == 2], "lavoro 0x33: niente dall'FPGA 2 in blocco termico")
    check(not [c for c in cands if c.job_id not in (0x11, 0x22, 0x33)], "nessun candidato con id sconosciuto")
    check(not [c for c in cands if c.job_id == 0x11 and c.t > T_JOB2 + 400_000], "nessun candidato del lavoro vecchio dopo quello nuovo")

    st = [x for x in resp if x.type == P.R_STATUS]
    s1 = [x for x in st if T_ST1 < x.t < T_JOB2]
    check(len(s1) == N and all(x.hash_en and x.job_valid and x.job_id == 0x11 and x.drops == 0 for x in s1),
          "STATUS #1: lavoro 0x11 valido su ogni FPGA, hashing acceso, nessuna perdita")
    check(all(100 < x.iter < 2000 for x in s1), f"STATUS #1: hash avviati dal core 0 = {[x.iter for x in s1]}")
    s2 = [x for x in st if T_ST2 < x.t < T_CLK]
    check(len(s2) == N and all(x.job_valid and x.job_id == 0x22 for x in s2), "STATUS #2: lavoro 0x22")
    s3 = [x for x in st if T_TRIP + 1_500_000 < x.t < T_DIS]
    trip = {x.pos: x.thermal_trip for x in s3}
    check(len(s3) == N and trip.get(2) is True and not any(v for k, v in trip.items() if k != 2),
          f"STATUS #3: blocco termico solo sull'FPGA 2 {trip}")
    s4 = [x for x in st if x.t > T_DIS]
    check(len(s4) == N and not any(x.hash_en for x in s4), "STATUS #4: tutti spenti")
    check(not [c for c in cands if c.t > T_DIS + 200_000], "nessun candidato dopo lo spegnimento")
    print("RESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
    sys.exit(0 if not fails else 1)


if __name__ == "__main__":
    main()
