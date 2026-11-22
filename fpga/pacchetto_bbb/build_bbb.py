#!/usr/bin/env python3
"""Pacchetto BOUND (BoundHash) per la BeagleBone Black rev C con la carrier BlackMiner (F1 Ultra).
Modello: prox-ultra-bbb-nofee.zip del 14/11 (Proxima; a sua volta da RIGOR, funzionante in macchina): install.sh,
run/start, service, bm-fan.sh 0.4, bmctl 0.2.2, bm-web 1.5 -> 1.6 (aggiunta la moneta bound).

  python3 build_bbb.py BASE_PROX_BBB.zip BIT CORES MHZ_TETTO MHZ_INIZIO OUT.zip
  es. ... prox-ultra-bbb-nofee.zip ../vivado/out_f1u_7c_90m/bound_f1u.bit 7 90 90 bound-ultra-bbb-nofee.zip
  env BOUND_FEE (default 3; 0 = senza fee), BOUND_MINER (default ../host_c/bound-miner-armv7-1.2-fee<BOUND_FEE>),
      BOUND_NOTE (riga in piu' nel LEGGIMI)

Attenzione: bm-web prende i MHz del bitstream dal primo "NNN MHz" (tre cifre) del LEGGIMI, altrimenti dalla
sua tabella: qui i MHz hanno due cifre, quindi nel LEGGIMI non deve comparire nessun numero di tre cifre
seguito da " MHz" (controllato sotto).
"""
import hashlib, io, os, re, sys, zipfile

base, bit, cores, mtop, mstart, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
HERE = os.path.dirname(os.path.abspath(__file__))
HC = os.path.join(HERE, "..", "host_c")
FEE = int(os.environ.get("BOUND_FEE", "3"))
FEEN = "nofee" if FEE == 0 else "fee%d" % FEE
NAME = "bound-ultra-bbb-" + FEEN
DEVFEE_ADDRESS = "BW1chX39ABzxeVgHqqmTDGL5woHd6GAgj2rqVm4iw4Ty5fG7K1Y7"
POOL = "stratum+tcp://bound.ariabrain.com:3373"      # AriaPool; in solitaria: stratum+tcp://IP_DEL_PC:7340 (il ponte)
CPH = 90660
HPS = cores * mtop * 1e6 / CPH

z = zipfile.ZipFile(base)
root = z.namelist()[0].split("/")[0]
T = lambda n: z.read(root + "/" + n).decode()
B = lambda n: z.read(root + "/" + n)


def must(s, *subs):
    for x in subs:
        assert x in s, "manca %r" % x
    return s


def rep(s, a, b):
    assert s.count(a) >= 1, "manca %r" % a[:60]
    return s.replace(a, b)


def ren(s):
    for a, b in [("prox-miner", "bound-miner"), ("prox_start.sh", "bound_start.sh"), ("prox_run.sh", "bound_run.sh"),
                 ("prox.conf", "bound.conf"), ("prox.service", "bound.service"), ("fpgaminer_top_prox.bit", "fpgaminer_top_bound.bit"),
                 ("/config/prox", "/config/bound"), ("PROXIMA miner", "BOUND miner"), ("PROXIMA", "BOUND"), ("prox", "bound")]:
        s = s.replace(a, b)
    return s


f = {}
EX = "--pool %s --user TUO_INDIRIZZO.ultra1 --freq-max %d --fan-min 20 --fan-max 100" % (POOL, mtop)
MSG = "metti il tuo indirizzo BOUND (B...) al posto di TUO_INDIRIZZO (e, se mini in solitaria, l'IP del PC col ponte al posto di IP_DEL_PC)"
# ---- bound_run.sh
f["bound_run.sh"] = must(ren(T("prox_run.sh")), "--freq-file $DIR/freq.json", "/config/bound")
# ---- bound.service: in conflitto con tutti gli altri miner, anche prox
s = ren(T("prox.service"))
s = rep(s, "Conflicts=rigor.service ting.service", "Conflicts=prox.service rigor.service ting.service")
f["bound.service"] = must(s, "prox.service rigor.service", "fpgaminer_top_bound.bit", "bound_run.sh")
# ---- bound_start.sh
s = ren(T("prox_start.sh"))
s = rep(s, 'OTHERS="rigor ting noid mds verus aln csd qtc quip nock"', 'OTHERS="prox rigor ting noid mds verus aln csd qtc quip nock"')
s, k = re.subn(r"#   --pool http://bound\.ariabrain\.com:3359 --user TOKEN\.ultra1 --freq-max \d+ --fan-min 20 --fan-max 100", "#   " + EX, s); assert k == 1
s = rep(s, 'grep -q TUO_TOKEN $DIR/bound.conf && { echo "in $DIR/bound.conf metti il tuo token di AriaPool al posto di TUO_TOKEN"; exit 1; }',
        'grep -q -e TUO_INDIRIZZO -e IP_DEL_PC $DIR/bound.conf && { echo "in $DIR/bound.conf %s"; exit 1; }' % MSG)
f["bound_start.sh"] = must(s, '"prox rigor ting noid', "TUO_INDIRIZZO", "bound-miner", POOL)
# ---- bound.conf.esempio
f["bound.conf.esempio"] = EX + "\n"
# ---- install.sh
s = ren(T("install.sh"))
s, k = re.subn(r"bitstream \d+ core BLAKE2b \(txid-blake2b\), build a \d+ MHz", "bitstream %d core BoundHash, build a %d MHz" % (cores, mtop), s); assert k == 1
s = rep(s, "Gli altri miner (RIGOR, TING, NOID, MDS, Verus, ALN, ...) restano", "Gli altri miner (Proxima, RIGOR, TING, NOID, MDS, Verus, ALN, ...) restano")
s = rep(s, 'grep -q TUO_TOKEN /config/bound/bound.conf && echo "ATTENZIONE: in bound.conf metti il tuo token di AriaPool al posto di TUO_TOKEN"',
        'grep -q -e TUO_INDIRIZZO -e IP_DEL_PC /config/bound/bound.conf && echo "ATTENZIONE: in bound.conf %s"' % MSG)
s, k = re.subn(r"aggiungi --freq-max \d+ a bound.conf", "aggiungi --freq-max %d a bound.conf" % mtop, s); assert k == 1
s = rep(s, 'echo "creato /config/bound/bound.conf: metti il tuo token di AriaPool (sudo nano /config/bound/bound.conf)"',
        'echo "creato /config/bound/bound.conf: %s (sudo nano /config/bound/bound.conf)"' % MSG)
s, k = re.subn(r"# frequenza iniziale: \d+ MHz \(il bitstream e' costruito per \d+\);", "# frequenza iniziale: %d MHz (il bitstream e' costruito per %d);" % (mstart, mtop), s); assert k == 1
s, k = re.subn(r'''echo '\{ "freq" : "\d+" \}' > /config/bound/freq.json; echo "creato /config/bound/freq.json \(\d+ MHz\)"''',
               '''echo '{ "freq" : "%d" }' > /config/bound/freq.json; echo "creato /config/bound/freq.json (%d MHz)"''' % (mstart, mstart), s); assert k == 1
s = s.replace("bm-web 1.5", "bm-web 1.6")
f["install.sh"] = must(s, "TUO_INDIRIZZO", '"freq" : "%d"' % mstart, "fpgaminer_top_bound.bit", "bm-web 1.6")
assert "AriaPool" not in f["install.sh"] and "TOKEN" not in f["install.sh"]
# ---- bm-web 1.6: moneta bound (le altre restano)
s = T("bm-web.py")
s, k = re.subn(r'(    \("prox",  "Proxima \(PROX\)".*\n)',
               lambda m: m.group(1) + '    ("bound", "BOUND",                  "BoundHash",         %d, "In pool: stratum+tcp://bound.ariabrain.com:3373 (AriaPool). In solitaria: pool = ponte sul PC, stratum+tcp://IP_DEL_PC:7340. Utente INDIRIZZO_BOUND.nome. Bitstream %d core BoundHash costruito a %d MHz (circa %.1f kH/s per board): --freq-max %d in bound.conf."),\n' % (mstart, cores, mtop, HPS * 6 / 1000, mtop),
               s, count=1); assert k == 1
s, k = re.subn(r'VERSION = "1\.5"', 'VERSION = "1.6"', s); assert k == 1
f["bm-web.py"] = must(s, '("bound"', 'VERSION = "1.6"', '("prox"')
compile(f["bm-web.py"], "bm-web.py", "exec")
for n in ("bm-web.html", "bm-web.service", "bm-fan.sh"):
    f[n] = T(n)
f["fan.conf"] = T("fan.conf").replace("rigor.conf", "bound.conf")
f["bin/bmctl"] = B("bin/bmctl")
miner_path = os.environ.get("BOUND_MINER", os.path.join(HC, "bound-miner-armv7-1.2-fee%d" % FEE))
f["bound-miner"] = open(miner_path, "rb").read()
if FEE: assert DEVFEE_ADDRESS.encode() in f["bound-miner"], "il miner non contiene l'indirizzo della fee"
feetxt = ("Nessuna dev fee in questa versione." if FEE == 0 else
          f"Dev fee {FEE} % (dichiarata): {FEE * 60} s ogni 100 minuti il miner lavora per l'indirizzo dello sviluppatore\n"
          f"{DEVFEE_ADDRESS}: in quella finestra si presenta al pool (o al tuo\n"
          f"ponte) con quell'indirizzo. In solitaria serve il ponte di questo repository, con un indirizzo per miner.")
f["fpgaminer_top_bound.bit"] = open(bit, "rb").read()
assert "3001800103752093" in f["fpgaminer_top_bound.bit"][:2048].hex(), "il bitstream non e' per la XC7K420T (F1 Ultra)"
md = lambda b: hashlib.md5(b if isinstance(b, bytes) else b.encode()).hexdigest()
note = os.environ.get("BOUND_NOTE", "")
f["LEGGIMI.txt"] = f"""BOUND (BoundHash) per F1 Ultra su BeagleBone Black rev C - {FEEN}
===================================================================================
Bitstream: {cores} core BoundHash (uno scratchpad da 512 KiB per core, tutto in block RAM), costruito a {mtop} MHz.
{cores} x {mtop} MHz / {CPH} clock per hash = {HPS:.0f} H/s per FPGA, circa {HPS * 6 / 1000:.1f} kH/s per board da 6 FPGA.
(Un i5-10400F fa circa 5,4 kH/s con tutti i thread.) Le velocita' sono in H/s e kH/s, non in GH/s.
{note}
bound-miner 1.2 (pool AriaPool oppure il proprio ponte; frequenza al volo, profili Giorno / Notte),
bm-web 1.6, bm-fan.sh 0.4, bmctl 0.2.2.
{feetxt}

Due modi di minare (il miner riconosce da solo quale dei due c'e' dall'altra parte):
A. In pool su AriaPool (https://pool.ariabrain.com), senza altro da installare:
      --pool {POOL} --user TUO_INDIRIZZO.ultra1
      TUO_INDIRIZZO = il tuo indirizzo BOUND (B...); ultra1 = nome. Il pool usa un protocollo suo, non Stratum:
      va solo con bound-miner 1.2 o piu' nuovo. I conti si vedono sulla pagina del pool.
B. In solitaria sul proprio nodo: sul PC servono il nodo con la patch (node/LEGGIMI_NODO.md) e il ponte
   (bridge/LEGGIMI_PC.md, porta 7340):
      --pool stratum+tcp://IP_DEL_PC:7340 --user TUO_INDIRIZZO.ultra1
   Le share servono solo a misurare l'hashrate: paga solo il blocco intero (il ponte scrive BLOCCO ACCETTATO).

Installazione (sulla BBB):
  unzip {NAME}.zip && cd {NAME} && sudo sh install.sh
  sudo sh /config/bound/bound_start.sh bench 60      prova degli FPGA senza rete (errori HW = 0?)
  sudo nano /config/bound/bound.conf     metti il tuo indirizzo (e l'IP del PC, se usi il ponte)
  sudo sh /config/bound/bound_start.sh start         mining (ferma gli altri miner)
  sudo sh /config/bound/bound_start.sh enable        avvio automatico
  sudo sh /config/bound/bound_start.sh log

/config/bound/bound.conf (una riga):
  {EX}
  --freq-max {mtop}: il bitstream e' costruito a {mtop} MHz; ogni richiesta piu' alta viene portata a {mtop}.

Frequenze: /config/bound/freq.json (all'installazione "freq" {mstart}); bm-web > Frequenza: casella per chip,
profili Giorno / Notte, cambio al volo senza fermare il miner. A 85 C l'FPGA si ferma da solo e riparte a 75 C.
Il consumo e' contenuto (circa 50 W per hash board): scendere di frequenza non serve, toglie solo hashrate.
bm-web 1.6: http://IP_BBB/ (scheda Miner: moneta BOUND). Log: journalctl -u bound -f.

md5: bitstream {md(f["fpgaminer_top_bound.bit"])}, bound-miner {md(f["bound-miner"])}.
Bitstream e miner 1.0 provati in macchina (18/11: circa 50 kH/s e 50 W per board); la 1.1 aggiunge la dev fee,
la 1.2 il pool AriaPool (provato contro il pool vero con FPGA emulati, non ancora in macchina). Nel bench guarda "6 FPGA" per board, "verified results" e "hardware errors";
in mining "pool: job", "share accepted" ed eventuali "hardware error".
"""
assert not re.search(r"\d{3} MHz", f["LEGGIMI.txt"]), "nel LEGGIMI c'e' un 'NNN MHz' a tre cifre: bm-web lo prenderebbe per il clock del bitstream"
order = ["LEGGIMI.txt", "bin/bmctl", "bm-fan.sh", "bm-web.html", "bm-web.py", "bm-web.service", "fan.conf",
         "fpgaminer_top_bound.bit", "install.sh", "bound-miner", "bound.conf.esempio", "bound.service", "bound_run.sh", "bound_start.sh"]
for n in order:
    if not isinstance(f[n], bytes):
        low = f[n].lower()
        assert n.startswith("bm-web") or ("prox" not in low.replace("prox rigor", "").replace("prox.service", "").replace("proxima", "")), n
f["md5sums.txt"] = "".join("%s  %s\n" % (md(f[n]), n) for n in order)
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zo:
    for n in order + ["md5sums.txt"]:
        zi = zipfile.ZipInfo(NAME + "/" + n, (2026, 11, 17, 12, 0, 0))
        zi.external_attr = (0o755 if n.endswith((".sh", ".py")) or n in ("bound-miner", "bin/bmctl") else 0o644) << 16
        zi.compress_type = zipfile.ZIP_DEFLATED
        zo.writestr(zi, f[n] if isinstance(f[n], bytes) else f[n].encode())
open(out, "wb").write(buf.getvalue())
print(out, md(buf.getvalue()))
