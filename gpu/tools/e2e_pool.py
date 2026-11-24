#!/usr/bin/env python3
"""Prova di bound-gpuminer contro il pool finto (tools/mock_ariapool.py), senza GPU: kernel sulla CPU, 64 righe.
   MINER=.../bound-gpuminer [FEE_ADDR=B... se il miner e' compilato con finestre di fee corte] python3 tools/e2e_pool.py [secondi] [opzioni]
   E2E_REAL_GPU=1: senza --cpu-backend, cioe' con le schede vere (o POCL: BOUND_OPENCL_CPU=1, BOUND_OPENCL_DUP=2 per due schede)"""
import os, random, subprocess, sys, tempfile, threading, time
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here)
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 25
MINER = os.environ.get("MINER", os.path.join(root, "dist", "bound-gpuminer")); PORT = 17500 + random.randint(0, 200)
ADDR = "B" + "".join(random.choice("123456789ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(51)); FEE = os.environ.get("FEE_ADDR")
def run(cmd, sink, **kw):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    threading.Thread(target=lambda: [sink.append(l.rstrip()) for l in p.stdout], daemon=True).start(); return p
plog, mlog = [], []
pool = run([sys.executable, os.path.join(here, "mock_ariapool.py"), str(PORT), "64", "3000"], plog, stdin=subprocess.PIPE); time.sleep(0.8)
miner = run([MINER] + ([] if os.environ.get("E2E_REAL_GPU") else ["--cpu-backend"]) + ["--lines", "64", "--batch", "512", "--pool", "stratum+tcp://127.0.0.1:%d" % PORT, "--user", ADDR + ".rig7"] + sys.argv[2:], mlog, cwd=tempfile.mkdtemp())
time.sleep(DUR); pool.stdin.write("DROP\n"); pool.stdin.flush(); time.sleep(40 if "--slow" in os.environ.get("E2E", "") else 12)
miner.terminate(); time.sleep(0.3); pool.terminate()
ml = "\n".join(mlog); fails = []
def check(c, m):
    print(("  ok   " if c else "  FAIL ") + m)
    if not c: fails.append(m)
shares = sum(l.startswith("SHARE " + ADDR + " rig7") for l in plog); rej = sum(l.startswith("REJECT") for l in plog)
check("protocollo AriaPool" in ml or "--proto" in " ".join(sys.argv), "protocollo riconosciuto da solo")
check("pool: accesso fatto come" in ml, "accesso al pool")
check(shares >= 5 and ml.count("share accettata") >= 5, "share accettate dal pool (hash ricalcolato): %d, viste dal miner: %d" % (shares, ml.count("share accettata")))
check(rej == 0 and "rifiutata" not in ml and "ERRORE" not in ml, "nessuna share rifiutata, nessun nonce sbagliato (rifiutate: %d)" % rej)
check("blocco nuovo" in ml, "job nuovi seguiti")
check("connessione persa" in ml and sum(l.startswith("LOGIN " + ADDR) for l in plog) >= 2, "ricollegamento dopo la caduta del pool")
if FEE:
    nf = sum(l.startswith("SHARE " + FEE + " devfee") for l in plog)
    check(nf >= 2 and "dev fee: finita" in ml and shares >= 5, "dev fee: %d share all'indirizzo della fee, %d all'utente" % (nf, shares))
if os.environ.get("KEEP"): check(any(l.startswith("KEEPALIVE") for l in plog), "keepalive")
late = [l for l in mlog if "kH/s" in l]; print("   ", late[-1] if late else "(nessuna riga di stato)")
print("RESULT:", "PASS" if not fails else "FAIL (%d)" % len(fails))
if fails: print("\n".join(mlog[-30:])); print("\n".join(plog[-20:]))
