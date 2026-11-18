#!/usr/bin/env python3
"""Prova da capo a fondo di bound-gpuminer senza GPU: il kernel gira sulla CPU (--cpu-backend, 64 righe = regtest),
il resto e' tutto vero: Stratum verso pc_bridge/bound_bridge.py, che parla con `boundd --regtest` con la patch.
   BOUNDD=.../boundd BOUNDCLI=.../bound-cli MINER=/tmp/bound-gpuminer python3 tools/e2e.py [secondi]"""
import json, os, random, shutil, subprocess, sys, tempfile, threading, time
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here)
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 25
BOUNDD, CLI, MINER = os.environ.get("BOUNDD", "boundd"), os.environ.get("BOUNDCLI", "bound-cli"), os.environ.get("MINER", os.path.join(root, "dist", "bound-gpuminer"))
dd = tempfile.mkdtemp(prefix="bound_gpu_e2e_"); wd = tempfile.mkdtemp(prefix="bound_gpu_wd_")
RPC, STR = 18700 + random.randint(0, 80), 17440 + random.randint(0, 80)
node = subprocess.Popen([BOUNDD, "--regtest", "--datadir", dd, "node", "--noseeds", "--nolisten", "--rpcport", str(RPC)], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
def cli(*a):
    o = subprocess.check_output([CLI, "--regtest", "--datadir", dd, "--rpcport", str(RPC)] + [str(x) for x in a], stderr=subprocess.DEVNULL).decode().strip()
    try: return json.loads(o)
    except ValueError: return o.strip('"')
for _ in range(50):
    try: cli("getblockcount"); break
    except Exception: time.sleep(0.2)
cli("createwallet", "e2e-passphrase"); ADDR = cli("getnewaddress"); h0 = cli("getblockcount")
if os.environ.get("FEE_ADDR") == "new":
    os.environ["FEE_ADDR"] = cli("getnewaddress"); print("FEE_ADDR", os.environ["FEE_ADDR"]); subprocess.check_call(os.environ["FEE_BUILD"].replace("@ADDR@", os.environ["FEE_ADDR"]), shell=True)
def run(cmd, sink, cwd=None):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=cwd)
    threading.Thread(target=lambda: [sink.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    return p
blog, mlog = [], []
bridge = run([sys.executable, os.path.join(root, "pc_bridge", "bound_bridge.py"), "--address", ADDR, "--datadir", dd, "--rpc", "127.0.0.1:%d" % RPC,
              "--listen", "127.0.0.1:%d" % STR, "--refresh", "5"], blog)
time.sleep(1)
miner = run([MINER, "--cpu-backend", "--lines", "64", "--batch", "512", "--pool", "127.0.0.1:%d" % STR, "--user", ADDR + ".gpu", "--share-s", "2"], mlog, cwd=wd)
time.sleep(DUR)
# il ponte cade e torna: il miner deve ricollegarsi da solo
bridge.terminate(); time.sleep(4)
bridge = run([sys.executable, os.path.join(root, "pc_bridge", "bound_bridge.py"), "--address", ADDR, "--datadir", dd, "--rpc", "127.0.0.1:%d" % RPC,
              "--listen", "127.0.0.1:%d" % STR, "--refresh", "5"], blog)
time.sleep(12)
miner.terminate(); time.sleep(0.3)
h1 = cli("getblockcount"); tip = cli("getblock", cli("getbestblockhash"), 2)
bridge.terminate(); node.terminate()
fails = []
def check(c, m):
    print(("  ok   " if c else "  FAIL ") + m)
    if not c: fails.append(m)
ml = "\n".join(mlog); acc = sum("ACCETTATO" in l for l in blog)
check("controllo del kernel contro la CPU: OK" in ml, "controllo iniziale del kernel")
check("autorizzato come" in ml, "autorizzato dal ponte")
check(ml.count("share accettata") >= 5, "share accettate: %d" % ml.count("share accettata"))
check(h1 - h0 >= 5 and acc == h1 - h0, "blocchi: altezza %d -> %d, %d accettati secondo il ponte" % (h0, h1, acc))
check(ADDR in json.dumps(tip) or os.environ.get("FEE_ADDR", "-") in json.dumps(tip), "il coinbase dell'ultimo blocco paga l'indirizzo dato")
check("ERRORE" not in ml and "low difficulty" not in ml and "malformed" not in ml and "duplicate" not in ml, "nessun nonce sbagliato, nessuna share rifiutata per difficolta', formato o doppione")
check("connessione persa" in ml and ml.count("ponte: collegato") >= 2, "ricollegamento dopo la caduta del ponte")
if os.environ.get("FEE_ADDR"):
    fa = os.environ["FEE_ADDR"]; paid = [l for l in blog if "ACCETTATO" in l]
    nf = sum(("pagato a " + fa) in l for l in paid); nu = sum(("pagato a " + ADDR) in l for l in paid)
    check(nf >= 2 and nu >= 2 and "dev fee: finita" in ml, "dev fee: %d blocchi pagati all'indirizzo della fee, %d all'utente" % (nf, nu))
late = [l for l in mlog if "kH/s" in l]
print("   ", late[-1] if late else "(nessuna riga di stato)")
shutil.rmtree(dd, ignore_errors=True); shutil.rmtree(wd, ignore_errors=True)
print("RESULT:", "PASS" if not fails else "FAIL (%d)" % len(fails))
if fails: print("\n".join(mlog[-25:])); print("\n".join(blog[-15:]))
sys.exit(1 if fails else 0)
