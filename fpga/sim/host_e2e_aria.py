# Parte ARIA=1 di host_e2e.py (eseguito da li': HERE, ROOT, HC, obj, lib, P, DUR, LINES gia' definiti).
# ARIA_REAL=host:porta ARIA_USER=INDIRIZZO.nome: contro il pool vero (4096 righe, l'emulatore fa pochi kH/s): solo accesso e share.
REAL = os.environ.get('ARIA_REAL')
if REAL: LINES = 4096; lib.bp_set_lines(LINES)
FEETEST = os.environ.get('FEETEST') == '1'
B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZ'
ADDR = 'B' + ''.join(random.choice(B58) for _ in range(51)); FEEADDR = 'B' + ''.join(random.choice(B58) for _ in range(51))
fee_def = ['-DDEVFEE_PERCENT=50', '-DDEVFEE_CYCLE_S=16.0', '-DDEVFEE_FIRST_S=6.0', '-DDEVFEE_ADDRESS="%s"' % FEEADDR] if FEETEST else ['-DDEVFEE_PERCENT=0']
subprocess.check_call(['gcc', '-std=gnu99', '-O2', '-Wall', '-Wno-unused-result', '-Wno-date-time', '-DNO_TLS'] + fee_def +
                      ['-o', os.path.join(obj, 'bound-miner'), os.path.join(HC, 'bound_main.c'), os.path.join(HC, 'bound_pow.c'),
                       os.path.join(HC, 'csd_pow.c'), os.path.join(HC, 'third_party', 'cJSON.c'), '-lm'])
PORT = 17600 + random.randint(0, 200)
pool = subprocess.Popen([sys.executable, os.path.join(ROOT, '..', 'gpu', 'tools', 'mock_ariapool.py'), str(PORT), str(LINES), '40'],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True) if not REAL else subprocess.Popen(['sleep', '100000'], stdout=subprocess.PIPE, text=True)
plog = []
threading.Thread(target=lambda: [plog.append(l.rstrip()) for l in pool.stdout], daemon=True).start()
time.sleep(0.8)
N_FPGA, CORES, CW = 3, 2, 25
class Fpga:
    def __init__(s, pos): s.pos = pos; s.mhz = 95; s.en = False; s.job = None; s.cnt = 0; s.jobs_seen = 0
tried = {}; dup = [0]; tlock = threading.Lock(); hlock = threading.Lock()
class Emu(threading.Thread):
    def __init__(s, fd):
        super().__init__(daemon=True); s.fd = fd; s.f = [Fpga(i) for i in range(N_FPGA)]
        s.buf = bytearray(); s.stop = False; s.lock = threading.Lock(); s.sent = 0; s.hashes = 0
    def send(s, typ, pos, payload):
        body = bytes([typ, pos]) + bytes(payload); c = P.crc16(body)
        with s.lock: os.write(s.fd, b'\x5A\xA5' + body + bytes([c >> 8, c & 0xFF]))
    def handle(s, cmd, addr, pl):
        for f in [f for f in s.f if addr == 0xFF or addr == f.pos]:
            if cmd == P.C_INFO: s.send(P.R_INFO, f.pos, b'BND1' + bytes([1, 1, CORES, 12, f.mhz & 0xFF, ((95 // 5) << 1) | (f.mhz >> 8)]))
            elif cmd == P.C_STATUS:
                flags = (1 if f.en else 0) | (4 if f.job else 0) | 8 | (64 if f.en else 0)
                s.send(P.R_STATUS, f.pos, struct.pack('<HBBB', P.degc_to_code(55), flags, 0, f.job['id'] if f.job else 0) + struct.pack('<I', f.cnt // CORES) + b'\x00')
            elif cmd == P.C_CTRL:
                f.en = bool(pl[0] & 1)
                if pl[0] & 2: f.job = None
            elif cmd == P.C_JOB and len(pl) == 209:
                f.job = {'id': pl[0], 'target': int.from_bytes(pl[1:9], 'big'), 'state': bytes(pl[9:209]), 'base': struct.unpack_from('<I', pl, 9 + 44)[0]}
                f.cnt = 0; f.jobs_seen += 1
            elif cmd == P.C_CLK: f.mhz = int(round(25 * pl[0] / pl[1]))
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
            if pkt[2] != P.C_ENUM and P.crc16(pkt[2:5 + ln]) != (pkt[5 + ln] << 8 | pkt[6 + ln]): continue
            s.handle(pkt[2], pkt[3], pkt[5:5 + ln])
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
                key = (j['state'][:44], nonce)                    # stesso header e stesso nonce su due FPGA = lavoro doppio
                with tlock:
                    if key in tried: dup[0] += 1
                    tried[key] = 1
                with hlock: lib.bp_fpga_hash(j['state'], ctypes.c_uint32(nonce), out)     # bound_pow.c ha uno scratchpad solo
                top = int.from_bytes(out.raw[24:32], 'little')
                if top <= j['target']:
                    top24 = top >> 40
                    s.send(P.R_CAND, f.pos, bytes([j['id']]) + struct.pack('<IH', nonce, core << 6) + bytes([top24 & 0xFF, (top24 >> 8) & 0xFF, top24 >> 16]))
                    s.sent += 1; break
emus = []; ports = []
for _ in range(2):
    m, sl = pty.openpty(); tty.setraw(m); e = Emu(m); e.start(); emus.append(e); ports.append(os.ttyname(sl))
cmd = [os.path.join(obj, 'bound-miner'), '--port', ports[0], '--port', ports[1], '--pool', 'stratum+tcp://' + (REAL or '127.0.0.1:%d' % PORT),
       '--user', os.environ.get('ARIA_USER', ADDR + '.e2e'), '--no-fan', '--status-s', '0.5', '--freq', '90', '--job-s', '1', '--lines', str(LINES), '--suggest-s', '0.25' if REAL else '0', '--max-cands', '20']
if os.environ.get('MINER_CMD'): cmd[0:1] = os.environ['MINER_CMD'].split()      # es. "qemu-arm-static host_c/bound-miner-armv7-1.2-fee3"
print('bound-miner per %.0f s (pool finto AriaPool, 2 catene) ...' % DUR, flush=True)
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out = []
threading.Thread(target=lambda: [out.append(l.rstrip()) for l in proc.stdout], daemon=True).start()
time.sleep(DUR * 0.6)
if not REAL: pool.stdin.write('DROP\n'); pool.stdin.flush()
time.sleep(DUR * 0.4)
proc.terminate(); proc.wait(5)
for e in emus: e.stop = True
time.sleep(0.3); pool.terminate()
log = '\n'.join(out); open(os.path.join(obj, 'miner_aria.log'), 'w').write(log + '\n'); open(os.path.join(obj, 'pool_aria.log'), 'w').write('\n'.join(plog) + '\n')
fails = []
def check(c, msg):
    print(('  ok   ' if c else '  FAIL ') + msg)
    if not c: fails.append(msg)
if REAL:
    check('AriaPool protocol' in log and 'logged in as' in log, 'pool vero: protocollo riconosciuto, accesso fatto')
    check(log.count('share accepted') >= 3, 'pool vero: share accettate: %d' % log.count('share accepted'))
    check('share rejected' not in log and 'hardware error' not in log, 'pool vero: nessuna share rifiutata')
    check(dup[0] == 0, 'nessun nonce provato due volte (%d hash)' % sum(e.hashes for e in emus))
    print('\n'.join([l for l in out if 'share accepted' not in l][:14])); sh = [l for l in out if 'total' in l and 'shares' in l]
    if sh: print('   ', sh[-1][-150:])
    print('RESULT:', 'PASS' if not fails else 'FAIL (%d)' % len(fails)); pool.terminate(); sys.exit(1 if fails else 0)
su = sum(l.startswith('SHARE ' + ADDR + ' e2e') for l in plog); sf = sum(l.startswith('SHARE ' + FEEADDR + ' devfee') for l in plog)
rej = [l for l in plog if l.startswith('REJECT')]
check('6 FPGA' in log, 'il miner ha trovato 6 FPGA su 2 catene')
check('AriaPool protocol' in log and 'logged in as' in log, 'protocollo riconosciuto da solo, accesso fatto')
check(su >= 10 and log.count('share accepted') >= 10, 'share accettate dal pool (hash ricalcolato): %d, viste dal miner: %d' % (su + sf, log.count('share accepted')))
check(not [l for l in rej if 'stale' not in l] and 'hardware error' not in log, 'nessuna share sbagliata o doppia (rifiutate: %s)' % sorted(set(l.split(' ', 2)[2] for l in rej)))
check(len(rej) <= max(3, (su + sf) // 20), 'poche share su job vecchi: %d su %d' % (len(rej), su + sf))
check(dup[0] == 0, 'nessun nonce provato due volte sullo stesso header (%d su %d hash)' % (dup[0], sum(e.hashes for e in emus)))
check(all(f.jobs_seen >= 6 for e in emus for f in e.f), 'lavori rinnovati su ogni FPGA %s' % [f.jobs_seen for e in emus for f in e.f])
check(sum(l.startswith('LOGIN ' + ADDR) for l in plog) >= 2 and 'connection lost' in log, 'ricollegamento dopo la caduta del pool')
if FEETEST: check(sf >= 3 and su >= 3 and 'dev fee: done' in log, 'dev fee: %d share all\'indirizzo della fee, %d all\'utente' % (sf, su))
sh = [l for l in out if 'total' in l and 'shares' in l]
if sh: print('   ', sh[-1][-150:])
print('RESULT:', 'PASS' if not fails else 'FAIL (%d)' % len(fails))
if fails: print('\n'.join(out[-40:])); print('\n'.join(plog[-25:]))
sys.exit(1 if fails else 0)
