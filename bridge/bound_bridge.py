#!/usr/bin/env python3
"""bound_bridge.py - ponte tra le macchine FPGA (bound-miner, Stratum v1) e il nodo BOUND con la patch
bound-fpga (RPC getminingwork / submitminingwork). Solo libreria standard di Python 3.

    python3 bound_bridge.py --address B... [--datadir ~/.bound/mainnet] [--listen 0.0.0.0:7340]

  --address     indirizzo BOUND di riserva: incassa i blocchi dei miner il cui --user non e' un indirizzo
                BOUND valido (es. "ultra1"). Ogni miner che si presenta con --user INDIRIZZO.nome mina
                per il proprio indirizzo: e' cosi' che piu' persone usano lo stesso ponte, ed e' cosi' che
                funziona la dev fee dei miner (per qualche minuto si presentano con l'indirizzo dello
                sviluppatore).
  --datadir     cartella dati del nodo: da li' si legge il cookie dell'RPC (.cookie)
  --rpc         host:porta dell'RPC del nodo (default 127.0.0.1:7332 mainnet; testnet 18332, regtest 18543)
  --listen      dove ascolta lo stratum per le macchine (default 0.0.0.0:7340)
  --share-s     secondi tra una share e l'altra se il miner non suggerisce una difficolta' (default 15)
  --refresh     ogni quanti secondi chiedere un template nuovo al nodo anche senza blocco nuovo (default 20)

Il nodo deve girare sulla stessa macchina (l'RPC ascolta solo su 127.0.0.1) con il suo miner interno fermo
(niente --mine, `bound-cli setmining false`).

Stratum (ordine dei byte "raw", quello che bound-miner usa di default):
  subscribe -> [[..], extranonce1 (4 byte), 4]       memo del coinbase = extranonce1 || extranonce2
  notify    [job, prevhash (32 byte dell'header), coinbase senza memo, "", ramo merkle, version, nbits, ntime, clean]
            version / nbits / ntime = i 4 byte dell'header in esadecimale
  submit    [utente, job, extranonce2, ntime, nonce (4 byte dell'header)]
Ogni share va al nodo (submitminingwork): lui calcola il BoundHash, il ponte lo confronta con il target
della share; se sta sotto il target di rete il nodo accetta il blocco.
"""
import argparse, base64, json, os, socket, struct, sys, threading, time, urllib.request, urllib.error

PDIFF1 = 0xFFFF << 208

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

class Node:
    def __init__(self, rpc, cookie_file):
        self.url = "http://%s/" % rpc; self.cookie_file = cookie_file; self.cookie = None; self.n = 0
        self.lock = threading.Lock()
    def call(self, method, *params):
        with self.lock:
            for attempt in (0, 1):
                if self.cookie is None:
                    self.cookie = open(self.cookie_file, "rb").read().strip()
                self.n += 1
                body = json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": list(params)}).encode()
                req = urllib.request.Request(self.url, body, {"Content-Type": "application/json",
                      "Authorization": "Basic " + base64.b64encode(self.cookie).decode()})
                try:
                    with urllib.request.urlopen(req, timeout=20) as r:
                        out = json.loads(r.read())
                except urllib.error.HTTPError as e:
                    if e.code == 401 and attempt == 0:      # nodo riavviato: cookie nuovo
                        self.cookie = None; continue
                    try: out = json.loads(e.read())
                    except Exception: raise RuntimeError("HTTP %d dal nodo" % e.code)
                if out.get("error"): raise RuntimeError("%s: %s" % (method, out["error"].get("message", out["error"])))
                return out["result"]

class Bridge:
    def __init__(self, a):
        self.a = a; self.node = Node(a.rpc, a.cookie)
        self.address = a.address
        self.jobs = {}                 # job id (str) -> work del nodo
        self.cur = {}                  # indirizzo -> ultimo work per quell'indirizzo
        self.valid = {}                # indirizzo -> True / False (validateaddress del nodo)
        self.tip = None; self.last = {}
        self.conns = []; self.lock = threading.Lock(); self.xn1_next = int(time.time()) & 0xFFFF
        self.blocks = 0; self.shares = 0; self.rejected = 0; self.stale = 0; self.t0 = time.time(); self.work = 0.0

    # ---------------------------------------------------------------- lavoro dal nodo
    def address_ok(self, addr):
        """l'indirizzo e' valido per questa rete? (chiesto al nodo una volta sola)"""
        if addr not in self.valid:
            try: self.valid[addr] = bool(self.node.call("validateaddress", addr).get("isvalid"))
            except Exception: return False
        return self.valid[addr]

    def fetch(self, addr):
        w = self.node.call("getminingwork", addr)
        w["jid"] = "%x" % w["id"]; w["address"] = addr
        old = self.cur.get(addr)
        clean = old is None or w["prevhash"] != old["prevhash"]
        with self.lock:
            self.jobs[w["jid"]] = w
            for k in sorted(self.jobs, key=lambda x: int(x, 16))[:-256]: del self.jobs[k]
            self.cur[addr] = w
        self.last[addr] = time.time()
        return w, clean

    def notify_params(self, w, clean):
        return [w["jid"], w["prevhash"], w["coinbase"], "", w["branch"],
                struct.pack("<i", w["version"]).hex(), struct.pack("<I", w["bits"]).hex(), struct.pack("<I", w["time"]).hex(), clean]

    def poll_loop(self):
        err = 0
        while True:
            try:
                addrs = sorted(set(c.address for c in list(self.conns) if c.address))
                if addrs:
                    tip = self.node.call("getbestblockhash")
                    newtip = tip != self.tip
                    if newtip:
                        self.tip = tip
                        with self.lock: self.jobs = {}
                    for addr in addrs:
                        if newtip or addr not in self.cur or time.time() - self.last.get(addr, 0) > self.a.refresh:
                            w, clean = self.fetch(addr)
                            if newtip and addr == addrs[0]:
                                log("nodo: blocco %d da minare, target %s..., %d transazioni" % (w["height"], w["target"][:16], w["transactions"]))
                            for c in list(self.conns):
                                if c.address == addr: c.send_job(w, clean or newtip)
                    for addr in [a for a in self.cur if a not in addrs]: del self.cur[addr]
                if err: log("nodo: di nuovo raggiungibile"); err = 0
            except Exception as e:
                err += 1
                if err == 1 or err % 30 == 0: log("nodo: %s (riprovo)" % e)
            time.sleep(self.a.poll)

    def stats_loop(self):
        while True:
            time.sleep(60)
            el = time.time() - self.t0
            log("stato: %d macchine, %d share (%d rifiutate, %d vecchie), %d blocchi, %.2f kH/s medi dalle share"
                % (len(self.conns), self.shares, self.rejected, self.stale, self.blocks, self.work / el / 1e3))

    # ---------------------------------------------------------------- share
    def submit(self, conn, p):
        user, jid, x2, ntime, nonce = p[:5]
        w = self.jobs.get(jid)
        if not w: self.stale += 1; return False, [21, "job not found", None]
        try:
            memo = conn.xn1 + bytes.fromhex(x2); n = struct.unpack("<I", bytes.fromhex(nonce))[0]
            t = struct.unpack("<I", bytes.fromhex(ntime))[0]
            assert len(memo) == 8
        except Exception:
            self.rejected += 1; return False, [20, "malformed share", None]
        key = (jid, x2, nonce)
        if key in conn.seen: self.rejected += 1; return False, [22, "duplicate share", None]
        conn.seen.add(key)
        r = self.node.call("submitminingwork", w["id"], memo.hex(), t, n)
        if r.get("stale"): self.stale += 1; return False, [21, "stale", None]
        h = int(r["powhash"], 16)
        if h > conn.target: self.rejected += 1; return False, [23, "low difficulty share", None]
        self.shares += 1; conn.shares += 1; self.work += conn.diff * 4294967296.0; conn.work += conn.diff * 4294967296.0
        if r.get("accepted"):
            self.blocks += 1
            log("*** BLOCCO %d ACCETTATO da %s, pagato a %s: %s" % (w["height"], conn.name, w["address"], r.get("block")))
        elif r.get("block"):
            log("blocco %d trovato da %s ma rifiutato dal nodo (%s)" % (w["height"], conn.name, r.get("reason")))
        return True, None

class Conn(threading.Thread):
    def __init__(self, br, sock, peer):
        super().__init__(daemon=True); self.br, self.sock, self.peer = br, sock, peer
        br.xn1_next = (br.xn1_next + 1) & 0xFFFFFFFF
        self.xn1 = struct.pack(">I", br.xn1_next); self.name = "%s:%d" % peer; self.auth = False
        self.diff = None; self.target = 0; self.seen = set(); self.shares = 0; self.work = 0.0; self.wl = threading.Lock()
        self.suggested = None; self.address = ""
    def tx(self, o):
        with self.wl: self.sock.sendall((json.dumps(o) + "\n").encode())
    def set_diff(self, d):
        d = max(d, 1e-9); self.diff = d; self.target = min(int(PDIFF1 / d), (1 << 256) - 1)
        self.tx({"id": None, "method": "mining.set_difficulty", "params": [d]})
    def send_job(self, w, clean):
        if not self.auth: return
        try:
            if clean: self.seen.clear()
            self.tx({"id": None, "method": "mining.notify", "params": self.br.notify_params(w, clean)})
        except OSError: pass
    def run(self):
        br = self.br; buf = b""
        log("macchina collegata da %s" % self.name)
        try:
            while True:
                d = self.sock.recv(65536)
                if not d: break
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip(): continue
                    m = json.loads(line); meth = m.get("method"); p = m.get("params") or []
                    if meth == "mining.subscribe":
                        self.tx({"id": m["id"], "result": [[["mining.notify", self.xn1.hex()]], self.xn1.hex(), 4], "error": None})
                    elif meth == "mining.authorize":
                        user = str(p[0]) if p else ""
                        self.name = "%s (%s)" % (user, self.peer[0])
                        cand = user.split(".")[0]
                        # l'indirizzo del miner se e' valido, altrimenti quello di --address
                        addr = cand if br.address_ok(cand) else br.address
                        if not addr:
                            log("%s: '%s' non e' un indirizzo BOUND valido e il ponte non ha --address: rifiutato" % (self.name, cand))
                            self.tx({"id": m["id"], "result": False, "error": [24, "user is not a BOUND address", None]})
                            continue
                        self.address = addr; self.auth = True
                        log("%s: mina per %s%s" % (self.name, addr, "" if addr == cand else " (indirizzo di riserva del ponte)"))
                        self.tx({"id": m["id"], "result": True, "error": None})
                        if self.diff is None:
                            self.set_diff(self.suggested if self.suggested else br.a.default_diff)
                        with br.lock:
                            if self not in br.conns: br.conns.append(self)
                        try:
                            w = br.cur.get(addr) or br.fetch(addr)[0]
                            self.send_job(w, True)
                        except Exception as e:
                            log("nodo: %s" % e)
                    elif meth == "mining.suggest_difficulty":
                        try: self.suggested = float(p[0])
                        except Exception: self.suggested = None
                        if self.suggested and self.suggested > 0:
                            if self.auth or self.diff is not None: self.set_diff(self.suggested)
                    elif meth == "mining.submit":
                        try: ok, err = br.submit(self, p)
                        except Exception as e: ok, err = False, [20, "node: %s" % e, None]
                        self.tx({"id": m["id"], "result": ok, "error": err})
                    elif m.get("id") is not None:
                        self.tx({"id": m["id"], "result": True, "error": None})
        except (OSError, ValueError) as e:
            pass
        with br.lock:
            if self in br.conns: br.conns.remove(self)
        log("macchina scollegata: %s (%d share)" % (self.name, self.shares))
        try: self.sock.close()
        except OSError: pass

def main():
    ap = argparse.ArgumentParser(description="ponte Stratum per bound-miner verso un nodo BOUND con la patch bound-fpga")
    ap.add_argument("--address", default="")
    ap.add_argument("--datadir", default=os.path.join(os.path.expanduser("~"), ".bound", "mainnet"))
    ap.add_argument("--cookie", default="")
    ap.add_argument("--rpc", default="127.0.0.1:7332")
    ap.add_argument("--listen", default="0.0.0.0:7340")
    ap.add_argument("--share-s", type=float, default=15.0)
    ap.add_argument("--rate", type=float, default=44000.0, help="H/s attesi per macchina, per la difficolta' iniziale")
    ap.add_argument("--refresh", type=float, default=20.0)
    ap.add_argument("--poll", type=float, default=1.0)
    a = ap.parse_args()
    a.cookie = a.cookie or os.path.join(a.datadir, ".cookie")
    a.default_diff = a.rate * a.share_s / 4294967296.0
    br = Bridge(a)
    try:
        info = br.node.call("getblockchaininfo")
        log("nodo %s: altezza %s" % (a.rpc, info.get("blocks", info.get("height", "?"))))
        if "getminingwork" not in json.dumps(br.node.call("help")):
            log("ATTENZIONE: questo nodo non ha getminingwork: serve boundd con la patch bound-fpga"); sys.exit(1)
    except Exception as e:
        log("nodo non raggiungibile (%s): continuo a provare" % e)
    host, port = a.listen.rsplit(":", 1)
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, int(port))); srv.listen(16)
    log("ponte BOUND: stratum su %s; ogni miner mina per l'indirizzo del suo --user%s" % (a.listen, ", di riserva %s" % a.address if a.address else ""))
    threading.Thread(target=br.poll_loop, daemon=True).start()
    threading.Thread(target=br.stats_loop, daemon=True).start()
    while True:
        c, peer = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Conn(br, c, peer).start()

if __name__ == "__main__":
    main()
