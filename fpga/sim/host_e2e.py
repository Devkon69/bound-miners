#!/usr/bin/env python3
"""Prova da capo a fondo di bound-miner (il programma vero, compilato per il PC) con il nodo vero:

   bound-miner  <-- pty -->  emulatore della catena (6 FPGA, protocollo BOUND, INFO "BND1")
       |
       +-- TCP -->  pc_bridge/bound_bridge.py  -- RPC -->  boundd --regtest con la patch bound-fpga

L'emulatore risponde a ENUM/INFO/STATUS/CTRL/JOB/CLK come il bitstream e cerca i candidati con il nonce
del bitstream (base + {core, posizione, contatore}), calcolando l'hash dallo stato del JOB come l'FPGA
(bound_pow.c compilato come libreria, 64 righe = regtest). Un candidato viene corrotto: il miner lo deve
contare come errore hardware. Su regtest quasi ogni share e' anche un blocco: il nodo li deve accettare.

Controlli: 6 FPGA trovati, CLK per --freq, share accettate dal ponte, blocchi accettati dal nodo (altezza
che sale, coinbase all'indirizzo dato), nessuna share rifiutata per difficolta', errore hardware rilevato,
lavori nuovi a ogni blocco.

  BOUNDD=/percorso/boundd BOUNDCLI=/percorso/bound-cli python3 sim/host_e2e.py [SECONDI]

ARIA=1: al posto di ponte e nodo, il pool finto col protocollo di AriaPool (gpu/tools/mock_ariapool.py, ricalcola ogni
hash e rifiuta i doppioni) e DUE catene da 3 FPGA: tutti lavorano sullo stesso header, su fette diverse dei nonce.
  ARIA=1 [FEETEST=1] python3 sim/host_e2e.py [SECONDI]
"""
import ctypes, os, sys, pty, tty, time, threading, subprocess, struct, select, random, json, shutil, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import bound_proto as P

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 45
HC = os.path.join(ROOT, 'host_c'); obj = os.path.join(HERE, 'obj_e2e'); os.makedirs(obj, exist_ok=True)
BOUNDD = os.environ.get('BOUNDD', 'boundd'); CLI = os.environ.get('BOUNDCLI', 'bound-cli')
LINES = 64
subprocess.check_call(['gcc', '-std=gnu99', '-O2', '-shared', '-fPIC', '-o', os.path.join(obj, 'libboundpow.so'),
                       os.path.join(HC, 'bound_pow.c'), '-lm'])
lib = ctypes.CDLL(os.path.join(obj, 'libboundpow.so')); lib.bp_set_lines(LINES)

ARIA = os.environ.get('ARIA') == '1'
if ARIA:
    exec(open(os.path.join(HERE, 'host_e2e_aria.py')).read()); sys.exit(0)
# ------------------------------------------------------------------ nodo regtest + ponte
dd = tempfile.mkdtemp(prefix='bound_e2e_')
RPCPORT, STRATUM = 18600 + random.randint(0, 80), 17340 + random.randint(0, 80)
node = subprocess.Popen([BOUNDD, '--regtest', '--datadir', dd, 'node', '--noseeds', '--nolisten', '--rpcport', str(RPCPORT)],
                        stdout=open(os.path.join(obj, 'node.log'), 'w'), stderr=subprocess.STDOUT)
def cli(*a):
    o = subprocess.check_output([CLI, '--regtest', '--datadir', dd, '--rpcport', str(RPCPORT)] + [str(x) for x in a],
                                stderr=subprocess.DEVNULL).decode().strip()
    try: return json.loads(o)
    except ValueError: return o.strip('"')
for _ in range(50):
    try: cli('getblockcount'); break
    except Exception: time.sleep(0.2)
cli('createwallet', 'e2e-passphrase')
ADDR = cli('getnewaddress')
# FEETEST=1: miner compilato con finestre di fee corte (meta' del tempo, ogni 16 s) verso un secondo indirizzo
FEETEST = os.environ.get('FEETEST') == '1'
FEEADDR = cli('getnewaddress') if FEETEST else ''
fee_def = ['-DDEVFEE_PERCENT=50', '-DDEVFEE_CYCLE_S=16.0', '-DDEVFEE_FIRST_S=6.0', '-DDEVFEE_ADDRESS="%s"' % FEEADDR] if FEETEST else ['-DDEVFEE_PERCENT=0']
subprocess.check_call(['gcc', '-std=gnu99', '-O2', '-Wall', '-Wno-unused-result', '-Wno-date-time', '-DNO_TLS'] + fee_def +
                      ['-o', os.path.join(obj, 'bound-miner'), os.path.join(HC, 'bound_main.c'), os.path.join(HC, 'bound_pow.c'),
                       os.path.join(HC, 'csd_pow.c'), os.path.join(HC, 'third_party', 'cJSON.c'), '-lm'])
h0 = cli('getblockcount')
bridge = subprocess.Popen([sys.executable, os.path.join(ROOT, '..', 'bridge', 'bound_bridge.py'), '--address', ADDR, '--datadir', dd,
                           '--rpc', '127.0.0.1:%d' % RPCPORT, '--listen', '127.0.0.1:%d' % STRATUM, '--refresh', '5'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
blog = []
threading.Thread(target=lambda: [blog.append(l.rstrip()) for l in bridge.stdout], daemon=True).start()
time.sleep(1.0)

# ------------------------------------------------------------------ emulatore della catena
N_FPGA, CORES, CW = 6, 2, 25
class Fpga:
    def __init__(s, pos):
        s.pos = pos; s.mhz = 95; s.en = False; s.job = None; s.cnt = 0; s.jobs_seen = 0; s.clk_writes = []

class Emu(threading.Thread):
    def __init__(s, fd):
        super().__init__(daemon=True); s.fd = fd; s.f = [Fpga(i) for i in range(N_FPGA)]
        s.buf = bytearray(); s.stop = False; s.lock = threading.Lock(); s.corrupted = 0; s.sent = 0; s.hashes = 0
    def send(s, typ, pos, payload):
        body = bytes([typ, pos]) + bytes(payload); c = P.crc16(body)
        with s.lock: os.write(s.fd, b'\x5A\xA5' + body + bytes([c >> 8, c & 0xFF]))
    def handle(s, cmd, addr, pl):
        for f in [f for f in s.f if addr == 0xFF or addr == f.pos]:
            if cmd == P.C_INFO:
                s.send(P.R_INFO, f.pos, b'BND1' + bytes([1, 1, CORES, 12, f.mhz & 0xFF, ((95 // 5) << 1) | (f.mhz >> 8)]))
            elif cmd == P.C_STATUS:
                flags = (1 if f.en else 0) | (4 if f.job else 0) | 8 | (64 if f.en else 0)
                s.send(P.R_STATUS, f.pos, struct.pack('<HBBB', P.degc_to_code(55), flags, 0, f.job['id'] if f.job else 0)
                       + struct.pack('<I', f.cnt // CORES) + b'\x00')
            elif cmd == P.C_CTRL:
                f.en = bool(pl[0] & 1)
                if pl[0] & 2: f.job = None
            elif cmd == P.C_JOB and len(pl) == 209:
                f.job = {'id': pl[0], 'target': int.from_bytes(pl[1:9], 'big'), 'state': bytes(pl[9:209]),
                         'base': struct.unpack_from('<I', pl, 9 + 44)[0]}
                f.cnt = 0; f.jobs_seen += 1
            elif cmd == P.C_CLK:
                m, o = pl[0], pl[1]; f.mhz = int(round(25 * m / o)); f.clk_writes.append((m, o))
    def rx(s):
        r, _, _ = select.select([s.fd], [], [], 0.0005)
        if r:
            try: s.buf += os.read(s.fd, 4096)
            except OSError: return
        while True:
            i = s.buf.find(b'\xA5\x5A')
            if i < 0 or len(s.buf) - i < 5: break
            ln = s.buf[i + 4]
            if len(s.buf) - i < 7 + ln: break
            pkt = bytes(s.buf[i:i + 7 + ln]); del s.buf[:i + 7 + ln]
            cmd, addr = pkt[2], pkt[3]
            if cmd != P.C_ENUM and P.crc16(pkt[2:5 + ln]) != (pkt[5 + ln] << 8 | pkt[6 + ln]): continue
            s.handle(cmd, addr, pkt[5:5 + ln])
    def run(s):
        k = 0; out = ctypes.create_string_buffer(32)
        while not s.stop:
            s.rx()
            f = s.f[k % N_FPGA]; k += 1
            if not (f.en and f.job): continue
            j = f.job
            for _ in range(40):
                core = f.cnt % CORES; n = f.cnt // CORES
                nonce = (j['base'] + ((core << (CW + 4)) | (f.pos << CW) | n)) & 0xFFFFFFFF
                f.cnt += 1; s.hashes += 1
                lib.bp_fpga_hash(j['state'], ctypes.c_uint32(nonce), out)
                top = int.from_bytes(out.raw[24:32], 'little')
                if top <= j['target']:
                    top24 = top >> 40
                    if s.corrupted == 0 and s.sent >= 3: top24 ^= 0x5A5A5A; s.corrupted = 1
                    s.send(P.R_CAND, f.pos, bytes([j['id']]) + struct.pack('<IH', nonce, core << 6)
                           + bytes([top24 & 0xFF, (top24 >> 8) & 0xFF, top24 >> 16]))
                    s.sent += 1
                    break

master, slave = pty.openpty(); tty.setraw(master)
emu = Emu(master); emu.start()
cmd = [os.path.join(obj, 'bound-miner'), '--port', os.ttyname(slave), '--pool', 'stratum+tcp://127.0.0.1:%d' % STRATUM,
       '--user', ADDR + '.e2e', '--no-fan', '--status-s', '0.5', '--freq', '90', '--job-s', '4', '--lines', str(LINES),
       '--suggest-s', '2', '--max-cands', '20']
if os.environ.get('BENCH') == '1':
    cmd = cmd[:cmd.index('--pool')] + ['--no-fan', '--bench', str(int(DUR) - 5), '--freq', '90', '--lines', str(LINES)]
print('bound-miner per %.0f s (ponte + nodo regtest) ...' % DUR, flush=True)
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out = []
threading.Thread(target=lambda: [out.append(l.rstrip()) for l in proc.stdout], daemon=True).start()
time.sleep(DUR)
proc.terminate(); proc.wait(5); emu.stop = True
time.sleep(0.3)
log = '\n'.join(out); open(os.path.join(obj, 'miner.log'), 'w').write(log + '\n')
fails = []
def check(c, msg):
    print(('  ok   ' if c else '  FAIL ') + msg)
    if not c: fails.append(msg)
try:
    if os.environ.get('BENCH') == '1':
        bl = [l for l in out if 'verified results' in l]; print('\n'.join(bl))
        check(bool(bl) and ' 1 hardware errors' in bl[0], 'bench: risultati verificati, solo l\'errore hardware iniettato')
    else:
        h1 = cli('getblockcount'); bal = cli('getaddressbalance', ADDR) if False else None
        tip = cli('getblock', cli('getbestblockhash'), 2)
        open(os.path.join(obj, 'bridge.log'), 'w').write('\n'.join(blog) + '\n')
        acc = sum('BLOCCO' in l and 'ACCETTATO' in l for l in blog)
        hw = log.count('hardware error')
        clk = [f.clk_writes for f in emu.f]
        sh = [l for l in out if 'total' in l and 'shares' in l]
        check('6 FPGA' in log and 'BoundHash cores' in log, 'il miner ha trovato 6 FPGA BND1')
        check(all(len(w) >= 1 and all(25 * x[0] / x[1] == 90 for x in w) for w in clk), 'CLK per --freq 90 su ogni FPGA: %s' % [len(w) for w in clk])
        check('authorized' in log, 'autorizzato dal ponte')
        check(h1 - h0 >= 5 and acc == h1 - h0, 'blocchi: altezza %d -> %d, %d accettati secondo il ponte' % (h0, h1, acc))
        check(ADDR in json.dumps(tip) or (FEETEST and FEEADDR in json.dumps(tip)), 'il coinbase dell\'ultimo blocco paga l\'indirizzo dato')
        if FEETEST:
            paid = [l for l in blog if 'ACCETTATO' in l]
            nf = sum(('pagato a ' + FEEADDR) in l for l in paid); nu = sum(('pagato a ' + ADDR) in l for l in paid)
            check(nf >= 2 and nu >= 2 and 'dev fee: done' in log, 'dev fee: %d blocchi pagati all\'indirizzo della fee, %d all\'utente' % (nf, nu))
        check('low difficulty' not in log and 'malformed' not in log, 'nessuna share rifiutata per difficolta\' o formato')
        check(emu.corrupted == 1 and hw == 1, 'candidato corrotto contato come errore hardware (%d)' % hw)
        check(all(f.jobs_seen >= 4 for f in emu.f), 'ogni FPGA ha avuto lavori nuovi %s' % [f.jobs_seen for f in emu.f])
        if sh: print('   ', sh[-1][-150:])
        print('    emulatore: %d hash, %d candidati; ponte: %s' % (emu.hashes, emu.sent, [l for l in blog if 'stato' in l][-1:] ))
finally:
    bridge.terminate(); node.terminate()
    try: node.wait(5)
    except Exception: node.kill()
    shutil.rmtree(dd, ignore_errors=True)
print('RESULT:', 'PASS' if not fails else 'FAIL (%d)' % len(fails))
if fails: print('\n'.join(out[-40:])); print('\n'.join(blog[-20:]))
sys.exit(1 if fails else 0)
