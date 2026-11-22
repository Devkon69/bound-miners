#!/usr/bin/env python3
"""Pool finto col protocollo di AriaPool per BOUND (bound.ariabrain.com:3373), come osservato il 22/11/2026:
  -> {"id":1,"method":"login","params":{"address":"B...","worker":"w","capabilities":["submit_claimed_hash"],"difficulty_hint":N}}
  <- {"id":1,"result":{...,"protocol_version":2},"status":"ok"}
  <- {"method":"job","params":{"job_id","header_base" (76 byte hex),"target" (numero a 256 bit in hex),"difficulty","height",
                                "nonce_start","nonce_end" (64 bit: i 32 bassi sono il nonce dell'header),"network_target","template_id"}}
  -> {"id":N,"method":"submit","params":{"job_id","nonce","claimed_hash" (i 32 byte dell'hash, in hex)}}
  <- {"id":N,"result":{"accepted":true,...},"status":"ok"}      oppure {"error":"...","id":N}
  -> {"id":N,"method":"keepalive","params":{}}                  <- {"id":N,"result":{"status":"KEEPALIVED"},"status":"ok"}
  a ogni altro metodo (mining.subscribe, ...) risponde {"error":"unknown method","id":N}.
Qui ogni share e' ricalcolata davvero (modello Python, righe dello scratchpad a scelta) e contata per indirizzo.
   python3 mock_ariapool.py PORTA [RIGHE=64] [DIFF=3000]      stampa una riga per evento; "DROP" su stdin chiude le connessioni"""
import json, os, socket, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fpga", "model"))
import bound_ref
PORT = int(sys.argv[1]); LINES = int(sys.argv[2]) if len(sys.argv) > 2 else 64; DIFF = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
lock = threading.Lock(); conns = []; seq = [0x1016ff00]; height = [4095]
def say(*a):
    with lock: print(*a, flush=True)
def mkjob(c):
    seq[0] += 1; c["n"] += 1
    base = (1).to_bytes(4, "little") + bytes([height[0] % 251] * 32) + os.urandom(32) + int(time.time()).to_bytes(4, "little") + bytes.fromhex("a0357c1c")
    j = {"job_id": os.urandom(8).hex(), "base": base, "start": seq[0] << 32, "diff": c["diff"], "target": (1 << 256) // c["diff"]}
    c["jobs"][j["job_id"]] = j
    for k in list(c["jobs"])[:-4]: del c["jobs"][k]
    send(c, {"method": "job", "params": {"difficulty": j["diff"], "header_base": base.hex(), "height": height[0], "job_id": j["job_id"],
          "network_target": "000000007c35a000000000000000000000000000000000000000000000000000", "nonce_end": j["start"] + 0xFFFFFFFF,
          "nonce_start": j["start"], "target": "%064x" % j["target"], "template_id": "%d-%d" % (height[0], c["n"])}})
def send(c, o):
    try: c["s"].sendall((json.dumps(o, separators=(",", ":")) + "\n").encode())
    except OSError: pass
def serve(s):
    c = {"s": s, "jobs": {}, "n": 0, "addr": None, "diff": DIFF, "seen": set()}
    with lock: conns.append(c)
    buf = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try: m = json.loads(line)
                except ValueError: send(c, {"error": "invalid JSON", "id": 0}); continue
                i, meth, p = m.get("id"), m.get("method"), m.get("params")
                if meth == "login":
                    if not isinstance(p, dict): send(c, {"error": "invalid login params", "id": i}); return
                    a = p.get("address", "")
                    if not (isinstance(a, str) and len(a) == 52 and a[0] == "B"): send(c, {"error": "not a mainnet address", "id": i}); return
                    if "submit_claimed_hash" not in (p.get("capabilities") or []):
                        send(c, {"error": "miner must support submit_claimed_hash (protocol version 2)", "id": i}); return
                    c["addr"] = a; c["worker"] = p.get("worker", "")
                    if p.get("difficulty_hint"): c["diff"] = int(p["difficulty_hint"])
                    say("LOGIN", a, c["worker"], "diff", c["diff"])
                    send(c, {"id": i, "result": {"capabilities": ["submit_claimed_hash", "difficulty_hint"], "protocol_version": 2,
                                                 "required_capabilities": ["submit_claimed_hash"]}, "status": "ok"})
                    mkjob(c)
                elif meth == "keepalive" and c["addr"]: send(c, {"id": i, "result": {"status": "KEEPALIVED"}, "status": "ok"}); say("KEEPALIVE", c["addr"])
                elif meth == "submit" and c["addr"]:
                    if not (isinstance(p, dict) and isinstance(p.get("job_id"), str) and isinstance(p.get("nonce"), int)):
                        send(c, {"error": "invalid submit params", "id": i}); continue
                    try: ch = bytes.fromhex(p.get("claimed_hash", "")); assert len(ch) == 32
                    except Exception: send(c, {"error": "claimed_hash must be 32 bytes of hex", "id": i}); continue
                    j = c["jobs"].get(p["job_id"]); n = p["nonce"]
                    if not j: err = "stale job"
                    elif not j["start"] <= n <= j["start"] + 0xFFFFFFFF: err = "nonce out of range"
                    elif (p["job_id"], n) in c["seen"]: err = "duplicate share"
                    elif int.from_bytes(ch, "little") > j["target"]: err = "low difficulty share"
                    elif bound_ref.bound_hash(j["base"] + (n & 0xFFFFFFFF).to_bytes(4, "little"), LINES) != ch: err = "claimed hash mismatch"
                    else: err = None
                    c["seen"].add((p["job_id"], n))
                    if err: say("REJECT", c["addr"], err); send(c, {"error": err, "id": i})
                    else:
                        say("SHARE", c["addr"], c["worker"])
                        send(c, {"id": i, "result": {"accepted": True, "difficulty": j["diff"], "status": "pending verification", "verified": False}, "status": "ok"})
                else: send(c, {"error": "unknown method", "id": i})
    except OSError: pass
    finally:
        with lock:
            if c in conns: conns.remove(c)
        s.close()
def ticker():
    k = 0
    while True:
        time.sleep(3); k += 1
        if k % 3 == 0: height[0] += 1
        with lock: cs = [c for c in conns if c["addr"]]
        for c in cs: mkjob(c)
def stdin():
    for l in sys.stdin:
        if l.strip() == "DROP":
            with lock: cs = list(conns)
            for c in cs:
                try: c["s"].shutdown(socket.SHUT_RDWR)
                except OSError: pass
            say("DROPPED", len(cs))
threading.Thread(target=ticker, daemon=True).start(); threading.Thread(target=stdin, daemon=True).start()
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", PORT)); srv.listen(8); say("LISTEN", PORT)
while True:
    s, _ = srv.accept(); threading.Thread(target=serve, args=(s,), daemon=True).start()
