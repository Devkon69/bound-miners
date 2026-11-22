#!/usr/bin/env python3
"""Pacchetto upgrade (interfaccia web BlackMiner) di BOUND (BoundHash) per F1 Ultra, F2, F1 Plus o F1, da un
pacchetto RIGOR della stessa macchina (la macchina e il tipo di FPGA si leggono dal runme.sh della base): cambia moneta, programma, caricatore, bitstream, configurazione, LEGGIMI e md5sums.
Tutto il resto (bm_hooks, bm_freqprof, status.sed, hook di cgminer.sh, runme.sh) resta quello dei pacchetti
"_init" delle altre monete.

  python3 build_pkg.py BASE_RIGOR.tar.gz BIT.bit CORES MHZ MHZ_MAX OUT.tar.gz [NOTA]
  es. ... upgrade_altcoin_F1Ultra_rigor_175_fee4.tar.gz ../vivado/out_f1u_7c_90m/bound_f1u.bit 7 90 100 \\
          upgrade_altcoin_F1Ultra_bound_90_fee3.tar.gz
      ... upgrade_altcoin_F2_rigor_175_fee4.tar.gz ../vivado/out_7c_90m/bound_f2.bit 7 90 100 upgrade_altcoin_F2_bound_90_fee3.tar.gz
      ... upgrade_altcoin_F1Plus_rigor_175_fee4.tar.gz ../vivado/out_f1p_3c_90m/bound_f1p.bit 3 90 100 upgrade_altcoin_F1Plus_bound_90_fee3.tar.gz
  env BOUND_TESTED="data" se il pacchetto e' stato provato in macchina, BOUND_POWER = testo sul consumo (non Ultra)
  env BOUND_FEE (default 3; 0 = pacchetto senza fee), BOUND_MINER (default ../host_c/bound-miner-armv7-1.2-fee<BOUND_FEE>)
"""
import hashlib, io, os, re, sys, tarfile

base, bit, cores, mhz, mmax, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
nota = sys.argv[7] if len(sys.argv) > 7 else ""
HERE = os.path.dirname(os.path.abspath(__file__)); HC = os.path.join(HERE, "..", "host_c")
FEE = int(os.environ.get("BOUND_FEE", "3"))
DEVFEE_ADDRESS = "BW1chX39ABzxeVgHqqmTDGL5woHd6GAgj2rqVm4iw4Ty5fG7K1Y7"
CPH = 90660
HPS = cores * mhz * 1e6 / CPH
md5 = lambda b: hashlib.md5(b).hexdigest()

tf = tarfile.open(base)
src = {m.name.split("/")[-1]: tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()}
modes = {m.name.split("/")[-1]: m.mode for m in tf.getmembers() if m.isfile()}
prefix = os.path.dirname([m.name for m in tf.getmembers() if m.isfile()][0])
# macchina e FPGA della base
_r = src["runme.sh"].decode()
WANT = re.search(r"^want=([0-9a-f]{8})$", _r, re.M).group(1)
MACH, CHIP = re.search(r"this package is for the (.+?) \((XC7\w+)\)", _r).groups()
WORKER = re.search(r'"user": "TUO_INDIRIZZO_RG1Q\.(\w+)"', src["cgminer_rigor.conf"].decode()).group(1)
ULTRA = MACH == "F1 Ultra"
POWER = os.environ.get("BOUND_POWER", "circa 50 W per hash board" if ULTRA else "")
assert POWER, "BOUND_POWER: testo sul consumo per questa macchina"


def ren(t):
    for a, b in [("RIGOR (ORU, OpuHash)", "BOUND (BoundHash)"), ("stratum+tcp://rgr.ariabrain.com:3356", "stratum+tcp://bound.ariabrain.com:3373"),
                 ("rgr.ariabrain.com:3356", "bound.ariabrain.com:3373 (AriaPool) or IP_OF_YOUR_PC:7340 (your bridge)"), ("TUO_INDIRIZZO_RG1Q", "TUO_INDIRIZZO_BOUND"),
                 ("YOUR_RG1Q_ADDRESS", "YOUR_BOUND_ADDRESS"), ("rg1q....worker", "YOUR_BOUND_ADDRESS.worker"),
                 ("rigor-miner 1.0", "bound-miner 1.2"), ("RIGOR", "BOUND"), ("Rigor", "Bound"), ("rigor", "bound")]:
        t = t.replace(a, b)
    return t


f = {}
for name, data in src.items():
    if name in ("rigor-miner", "rigor-loadfpga", "fpgaminer_top_rigor.bit", "md5sums.txt", "LEGGIMI.txt"):
        continue
    f[ren(name)] = ren(data.decode()).encode()
# ---- configurazione della moneta
c = f["cgminer_bound.conf"].decode()
c, k = re.subn(r'"freq" : "\d+"', '"freq" : "%d"' % mhz, c); assert k == 1
assert "stratum+tcp://bound.ariabrain.com:3373" in c and "TUO_INDIRIZZO_BOUND." + WORKER in c
f["cgminer_bound.conf"] = c.encode()
# ---- extra.conf
e = f["extra.conf"].decode()
for a, b in [("#   --freq-max MHZ        highest clock accepted (default 300, or 75 above the build clock)",
              "#   --freq-max MHZ        highest clock accepted (default: 15 above the build clock)"),
             ("#   --byteorder btc|raw   stratum byte order (default auto: chosen from the pool answers)\n", ""),
             ("#   --job-s S             new header for each FPGA every S s (default 80 % of a job = 2^29 nonces per core)",
              "#   --job-s S             new header for each FPGA every S s (default 30)")]:
    assert a in e, a
    e = e.replace(a, b)
e, k = re.subn(r"--freq-max \d+\n", "--freq-max %d\n" % mmax, e); assert k == 1
f["extra.conf"] = e.encode()
# ---- runme.sh: tetto del clock, elenco dei bitstream non del costruttore, messaggio finale
r = f["runme.sh"].decode()
r, k = re.subn(r"--freq-max \d+", "--freq-max %d" % mmax, r); assert k == 2, k
a = "*_bound.bit|*_csd.bit|*_qtc.bit|*_noid.bit|*_mds.bit|*_ting.bit|*_walk.bit)"
assert a in r
r = r.replace(a, "*_bound.bit|*_rigor.bit|*_prox.bit|*_csd.bit|*_qtc.bit|*_noid.bit|*_mds.bit|*_ting.bit|*_walk.bit)")
assert ("want=" + WANT) in r and "bound-miner 1.2" in r
r = r.replace('pool IP_DEL_PC:7340 (your bridge), user YOUR_BOUND_ADDRESS.worker"', 'pool stratum+tcp://IP_OF_YOUR_PC:7340 (the bridge next to your node), user YOUR_BOUND_ADDRESS.worker"')
f["runme.sh"] = r.encode()
# ---- programmi e bitstream
def prog(name):   # binario ARM: host_c/ (appena compilato) oppure release/fpga-f1-ultra/ del repository
    for d in (HC, os.path.join(HERE, "..", "..", "release", "fpga-f1-ultra")):
        if os.path.exists(os.path.join(d, name)): return os.path.join(d, name)
    raise SystemExit("manca " + name)
miner = open(os.environ.get("BOUND_MINER") or prog("bound-miner-armv7-1.2-fee%d" % FEE), "rb").read()
if FEE: assert DEVFEE_ADDRESS.encode() in miner, "il miner non contiene l'indirizzo della fee"
bitb = open(bit, "rb").read()
m = re.search(r"30018001(........)", bitb[:2048].hex())
assert m and m.group(1) == WANT, "il bitstream non e' per la %s (%s)" % (CHIP, MACH)
f["bound-miner"] = miner
f["bound-loadfpga"] = open(prog("bound-loadfpga-armv7"), "rb").read()
f["fpgaminer_top_bound.bit"] = bitb
fee_txt = ("NESSUNA DEV FEE in questa versione." if not FEE else
           f"DEV FEE {FEE} % (dichiarata): {FEE * 60} s ogni 100 minuti il miner lavora per l'indirizzo dello sviluppatore\n"
           f"{DEVFEE_ADDRESS}: in quella finestra si presenta al pool (o al tuo\n"
           f"ponte) con quell'indirizzo, quindi quello che trova in quei {FEE * 60} s va a lui. Lo scrive nel log all'avvio e a ogni finestra.\n"
           f"In solitaria il ponte deve essere quello di questo repository (versione con un indirizzo per miner).")
out_name = os.path.basename(out)
PERB = f"circa {HPS * 6 / 1000:.1f} kH/s per hash board da 6 FPGA, " if ULTRA else ""
GHS = (f"{HPS * 6 / 1000:.0f} kH/s si leggono\n     come {HPS * 6 / 1e9:.6f} GH/s" if ULTRA else
       f"{HPS / 1000:.1f} kH/s per FPGA si leggono\n     come {HPS / 1e9:.7f} GH/s")
PROVA = ("Il bitstream e il miner sono provati su una F1 Ultra con la BeagleBone (pacchetto BBB); QUESTO pacchetto per\n"
         "l'interfaccia web e il miner 1.2 (AriaPool) NON SONO ANCORA PROVATI IN MACCHINA" if ULTRA else
         f"Stesso RTL e stesso miner della F1 Ultra (provata in macchina con la BeagleBone); il bitstream per la {MACH}\n"
         "e QUESTO pacchetto NON SONO ANCORA PROVATI IN MACCHINA")
if os.environ.get("BOUND_TESTED"):   # es. BOUND_TESTED="21/11/2026"
    PROVA = (f"Bitstream e pacchetto provati in macchina su una {MACH} ({os.environ['BOUND_TESTED']}) con il miner 1.1. Il miner 1.2 di questo\n"
             "pacchetto aggiunge il pool AriaPool: provato contro il pool vero con FPGA emulati, NON ANCORA IN MACCHINA. Per controllare")
else:
    PROVA += ": dopo"
f["LEGGIMI.txt"] = f"""BOUND (BoundHash) per BlackMiner {MACH} - pacchetto per l'interfaccia web
=====================================================================

Contenuto: bitstream bound, {cores} core BoundHash (uno scratchpad da 512 KiB per core, in block RAM) a {mhz} MHz:
           {HPS:.0f} H/s per FPGA nominali, {PERB}{POWER};
           bound-miner 1.2 (pool AriaPool oppure Stratum verso il proprio ponte, ventole PWM, pagina Frequency con
           profili), bound-loadfpga.
           {nota}
Questo pacchetto e' solo per la {MACH} ({CHIP}). Se la macchina ha FPGA di un altro tipo
l'installazione si ferma con un messaggio e non cambia niente.

{fee_txt}

COME SI MINA BOUND (due modi; il miner riconosce da solo quale dei due c'e' dall'altra parte)
  A. IN POOL, su AriaPool (https://pool.ariabrain.com): non serve altro.
        pool   = stratum+tcp://bound.ariabrain.com:3373        (e' gia' scritto nel pacchetto)
        utente = IL_TUO_INDIRIZZO_BOUND.nome_macchina           (indirizzo B...)
     Il pool usa un protocollo suo, non Stratum: va solo con bound-miner 1.2 o piu' nuovo. Tutti gli FPGA lavorano
     sullo stesso header, ognuno su una parte diversa dei nonce. I conti si vedono sulla pagina del pool.
  B. IN SOLITARIA sul proprio nodo. Su un PC (Linux o WSL) servono
        1. il nodo BOUND con la patch bound-fpga (cartella node/ del repository),
        2. il ponte bound_bridge.py (cartella bridge/), che ascolta sulla porta 7340.
        pool   = stratum+tcp://IP_DEL_PC:7340                   utente = come sopra
     Le share servono solo a misurare l'hashrate: paga solo il blocco intero (il ponte scrive BLOCCO ACCETTATO).

Installazione (come i bitstream originali, senza ssh)
  1. Interfaccia web > Configuration > Upgrade: scegli {out_name} e conferma.
     La macchina si riavvia da sola (circa 90 s).
  2. Configuration > Pool/Miner: nel menu della moneta scegli "bound"; pool e utente come sopra. Premi Save.
  3. Miner Status: temperature e stato degli FPGA per scheda (o = sta minando, x = fermo, - = nessuna
     risposta), Accepted / Rejected = share del pool o del ponte.
     ATTENZIONE: la pagina mostra l'hashrate in GH/s, e BoundHash va a kH/s: {GHS}. I valori leggibili (H/s per FPGA, kH/s totali) sono in /tmp/bound.log.

Cosa si regola dal web
  - pool (AriaPool o il proprio ponte) e utente
  - ventola fissa (Pool/Miner, "fan customize"); senza, e' automatica sulla temperatura
  - per tornare a un'altra moneta basta sceglierla nel menu e salvare
  - Frequency: clock degli FPGA in MHz. Il bitstream ha il timing chiuso a {mhz} MHz; extra.conf contiene
    "--freq-max {mmax}": fino a {mmax} e' overclock (guarda la colonna HW, errori hardware), oltre il miner non va.
    AVVISO: con --web-freq il miner usa il valore della pagina Frequency. Se la macchina arriva da
    un'altra moneta metti {mhz} nella pagina.

Cartella /config/bound (resta dopo i riavvii)
  bin/cgminer     il programma bound-miner (si chiama cgminer per gli script originali)
  bin/bound-loadfpga  caricatore del bitstream (DONE basso + INIT_B alto = caricato)
  extra.conf      opzioni in piu' che il web non ha (vedi i commenti nel file)
  fan.conf        frequenza PWM per ventole a 4 fili (Noctua: PWM_PERIOD_NS=40000)
  bound_hook.sh   avvio automatico: "sh /config/bound/bound_hook.sh status" mostra lo stato
  bound_start.sh  strumenti via ssh: bench (sh /config/bound/bound_start.sh bench), test ventole
  install.log     resoconto dell'installazione

Log del miner: /tmp/bound.log (in RAM); log del caricatore /tmp/bound_loadfpga.log; hook /tmp/bound_hook.log.
Gli errori compaiono anche in System > Kernel Log.

Avvio, bm_hooks, pagina Frequency con profili giorno / notte, ventole e caricatore: come nei pacchetti delle
altre monete (CSD, NOID, MDS, QTC, TING, RIGOR). Un extra.conf o fan.conf gia' presente non viene sostituito.

md5: bitstream {md5(bitb)}, bound-miner {md5(miner)}.
{PROVA} il primo avvio guarda /tmp/bound.log e
/tmp/bound_hook.log.
""".encode()
order = ["bound-miner", "bound-loadfpga", "fpgaminer_top_bound.bit", "cgminer_bound.conf", "bound_hook.sh", "cgminer_hook.inc",
         "status.sed", "status_tiles.inc", "bound_start.sh", "bm_hooks.sh", "bm_freqprof.cgi", "freqprof.html"]
for n in order: assert n in f, n
f["md5sums.txt"] = "".join("%s  %s\n" % (md5(f[n]), n) for n in order).encode()
for n, d in f.items():
    if n not in ("bound-miner", "bound-loadfpga", "fpgaminer_top_bound.bit", "bm_hooks.sh", "bm_freqprof.cgi", "freqprof.html", "status.sed", "status_tiles.inc", "fan.conf"):
        low = d.decode().lower()
        assert "rigor" not in low.replace("*_rigor.bit", "").replace("ting, rigor", "") and "rgr.ariabrain" not in low and "rg1q" not in low, n
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w:gz") as to:
    for n in sorted(f):
        ti = tarfile.TarInfo((prefix + "/" if prefix else "") + n)
        ti.size = len(f[n]); ti.mtime = 1795000000
        ti.mode = 0o755 if n.endswith((".sh", ".cgi")) or n in ("bound-miner", "bound-loadfpga") else 0o644
        to.addfile(ti, io.BytesIO(f[n]))
open(out, "wb").write(buf.getvalue())
print(out, md5(buf.getvalue()), "file:", len(f), "prefisso:", repr(prefix))
