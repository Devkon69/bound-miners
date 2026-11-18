"""
bound_proto.py - host side of the BOUND serial chain protocol (framing as NOID v1.1 / QTC / BC3).

Command  (host -> FPGAs):  A5 5A CMD ADDR LEN payload CRC16_hi CRC16_lo
Response (FPGA -> host):   5A A5 TYPE POS payload[10] CRC16_hi CRC16_lo   (16 bytes)
CRC16-CCITT, poly 0x1021, init 0xFFFF.  Multi-byte fields are little-endian.
See docs/PROTOCOL.md.
"""
import struct

C_ENUM, C_JOB, C_CTRL, C_STATUS, C_INFO, C_CLK = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
R_CAND, R_STATUS, R_INFO = 0x81, 0x82, 0x83
BROADCAST = 0xFF
RESP_LEN = 16

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def command(cmd, addr=BROADCAST, payload=b""):
    body = bytes([cmd, addr, len(payload)]) + bytes(payload)
    c = crc16(body)
    return b"\xA5\x5A" + body + bytes([c >> 8, c & 0xFF])

def cmd_enum(first_pos=0):
    return command(C_ENUM, BROADCAST, bytes([first_pos, (~first_pos) & 0xFF]))

def cmd_ctrl(enable, clear_job=False, trip_c=85, resume_c=75, watchdog_s=0, addr=BROADCAST):
    flags = (1 if enable else 0) | (2 if clear_job else 0)
    return command(C_CTRL, addr, bytes([flags, trip_c, resume_c, watchdog_s]))

def cmd_status(addr=BROADCAST):
    return command(C_STATUS, addr)

def cmd_info(addr=BROADCAST):
    return command(C_INFO, addr)

def clk_params(mhz, fin=25.0):
    """(M, O, achieved MHz) for the hash MMCM: VCO = fin*M in 600..1440 MHz, f = VCO/O,
    the closest value not above the request (ties: the highest VCO, lowest jitter)."""
    best = None
    for o in range(2, 65):
        for m in range(24, 58):
            f = fin * m / o
            if f <= mhz + 1e-9 and (best is None or f > best[2] + 1e-9 or
                                    (abs(f - best[2]) < 1e-9 and m > best[0])):   # same f: higher VCO
                best = (m, o, f)
    return best

def cmd_clk(mhz, addr=BROADCAST):
    m, o, f = clk_params(mhz)
    r = int(round(f))
    return command(C_CLK, addr, bytes([m, o, r & 0xFF, r >> 8]))

def job_state200(mid_words, tail12, nonce_base=0):
    """Stato del JOB per BOUND: mid (8 parole SHA-256, big-endian) + byte 64..75 dell'header + base del nonce."""
    assert len(mid_words) == 8 and len(tail12) == 12
    st = struct.pack(">8I", *mid_words) + bytes(tail12) + struct.pack("<I", nonce_base)
    return st + bytes(200 - len(st))

def cmd_job(job_id, target64, state200, addr=BROADCAST):
    """target64: target >> 192 (i 64 bit alti del target a 256 bit); l'FPGA manda ogni nonce il cui
    hash >> 192 e' <= target64. state200: job_state200()."""
    assert len(state200) == 200
    payload = bytes([job_id & 0xFF]) + target64.to_bytes(8, "big") + bytes(state200)
    assert len(payload) == 209
    return command(C_JOB, addr, payload)

def fpga_nonce(base, core, pos, counter, idw=3):
    """Nonce che il core `core` dell'FPGA in posizione `pos` prova per `counter`-esimo."""
    cw = 32 - idw - 4
    return (base + ((core << (cw + 4)) | (pos << cw) | counter)) & 0xFFFFFFFF

class Response:
    def __init__(self, raw):
        self.raw = raw
        self.type, self.pos = raw[2], raw[3]
        self.payload = raw[4:14]
        self.crc_ok = crc16(raw[2:14]) == (raw[14] << 8 | raw[15])
        p = self.payload
        if self.type == R_CAND:
            self.job_id = p[0]
            self.nonce = struct.unpack("<I", p[1:5])[0]
            idx = p[5] | (p[6] << 8)
            self.core = (idx >> 6) & 0xF
            self.top24 = p[7] | (p[8] << 8) | (p[9] << 16)   # i 24 bit alti dell'hash
        elif self.type == R_STATUS:
            self.temp_code = p[0] | (p[1] << 8)
            self.temp_c = self.temp_code * 503.975 / 4096 - 273.15
            f = p[2]
            self.hash_en, self.thermal_trip, self.job_valid = bool(f & 1), bool(f & 2), bool(f & 4)
            self.enumerated, self.bi, self.wd_expired, self.host_en = bool(f & 8), bool(f & 16), bool(f & 32), bool(f & 64)
            self.clk_changing = bool(f & 128)
            self.addr_pins = p[3]
            self.job_id = p[4]
            self.iter = struct.unpack("<I", p[5:9])[0]
            self.drops = p[9]
        elif self.type == R_INFO:
            self.magic = bytes(p[0:4])
            self.version = (p[4], p[5])
            self.cores, self.rounds = p[6], p[7]
            self.mhz = p[8] | ((p[9] & 1) << 8)          # current MHz (9 bits)
            self.built_mhz = (p[9] >> 1) * 5             # build MHz / 5

    def __repr__(self):
        d = {k: v for k, v in self.__dict__.items() if k not in ("raw", "payload")}
        return "Response(%s)" % d

class Deframer:
    """byte stream -> Response objects (resynchronises on 5A A5)"""
    def __init__(self):
        self.buf = bytearray()
    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"\x5A\xA5")
            if i < 0:
                del self.buf[:-1]
                break
            if len(self.buf) - i < RESP_LEN:
                del self.buf[:i]
                break
            raw = bytes(self.buf[i:i + RESP_LEN])
            r = Response(raw)
            if r.crc_ok:
                out.append(r)
                del self.buf[:i + RESP_LEN]
            else:
                del self.buf[:i + 1]
        return out

def degc_to_code(t):
    return int((t + 273.15) * 4096 / 503.975)
