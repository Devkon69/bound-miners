/* csd_pow.c - Compute Substrate (CSD) proof of work, portable C (see csd_pow.h). */
#include <string.h>
#include <math.h>
#include "csd_pow.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static const uint32_t IV[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

void cp_compress(uint32_t st[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(block + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void cp_sha256(const uint8_t *d, size_t n, uint8_t out[32])
{
    uint32_t st[8]; memcpy(st, IV, sizeof st);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) cp_compress(st, d + i);
    uint8_t blk[128]; size_t r = n - i;
    memset(blk, 0, sizeof blk);
    memcpy(blk, d + i, r);
    blk[r] = 0x80;
    size_t tot = (r + 9 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) blk[tot - 1 - k] = (uint8_t)(bits >> (8 * k));
    cp_compress(st, blk);
    if (tot == 128) cp_compress(st, blk + 64);
    for (int k = 0; k < 8; k++) put_be32(out + 4 * k, st[k]);
}

void cp_sha256d(const uint8_t *d, size_t n, uint8_t out[32])
{
    uint8_t h[32]; cp_sha256(d, n, h); cp_sha256(h, 32, out);
}

void cp_header(uint8_t h[84], uint32_t version, const uint8_t prev[32], const uint8_t merkle[32],
               uint64_t time, uint32_t bits, uint32_t nonce)
{
    put_le32(h, version);
    memcpy(h + 4, prev, 32);
    memcpy(h + 36, merkle, 32);
    for (int k = 0; k < 8; k++) h[68 + k] = (uint8_t)(time >> (8 * k));
    put_le32(h + 76, bits);
    put_le32(h + 80, nonce);
}

void cp_merkle(const uint8_t *cb, size_t cbn, const uint8_t (*branch)[32], int nbranch, uint8_t out[32])
{
    uint8_t h[32], buf[64];
    cp_sha256d(cb, cbn, h);
    for (int i = 0; i < nbranch; i++) {
        memcpy(buf, h, 32); memcpy(buf + 32, branch[i], 32);
        cp_sha256d(buf, 64, h);
    }
    memcpy(out, h, 32);
}

void cp_fpga_job(const uint8_t h[84], uint32_t mid[8], uint32_t tail[4])
{
    memcpy(mid, IV, 32);
    cp_compress(mid, h);
    for (int k = 0; k < 4; k++) tail[k] = be32(h + 64 + 4 * k);
}

void cp_hash_nonce(const uint8_t h[84], uint32_t nonce, uint8_t out[32])
{
    uint8_t t[84]; memcpy(t, h, 80); put_le32(t + 80, nonce);
    cp_sha256d(t, 84, out);
}

int cp_be256_le(const uint8_t a[32], const uint8_t b[32])
{
    return memcmp(a, b, 32) <= 0;
}

void cp_target_from_diff(double d, uint8_t t[32])
{
    uint8_t p1[32]; memset(p1, 0, 32); p1[4] = 0xFF; p1[5] = 0xFF;
    if (!(d > 0)) d = 1;
    if (d < 1) {
        /* fractional difficulty: t = pdiff1 * floor(2^32 / d) >> 32, saturating */
        double kd = floor(4294967296.0 / d);
        if (kd >= 1.8e19) { memset(t, 0xFF, 32); return; }   /* (near) everything is a share */
        uint64_t k = (uint64_t)kd;
        uint8_t w[40]; memset(w, 0, sizeof w);         /* 320-bit product, big-endian */
        /* multiply by the two 32-bit halves of k, byte by byte from the right */
        uint32_t kh[2] = { (uint32_t)(k >> 32), (uint32_t)k };
        for (int part = 0; part < 2; part++) {
            uint64_t carry = 0;
            int shift = part == 0 ? 4 : 0;               /* high half is worth 2^32 more */
            for (int i = 31; i >= 0; i--) {
                int pos = i + 8 - shift;
                uint64_t v = (uint64_t)p1[i] * kh[part] + carry + w[pos];
                w[pos] = (uint8_t)v; carry = v >> 8;
            }
            for (int pos = 7 - shift; pos >= 0 && carry; pos--) {
                uint64_t v = carry + w[pos]; w[pos] = (uint8_t)v; carry = v >> 8;
            }
        }
        /* result = w >> 32 bits = bytes w[0..35], keep the low 32 bytes */
        int over = 0;
        for (int i = 0; i < 4; i++) over |= w[i];
        if (over) memset(t, 0xFF, 32); else memcpy(t, w + 4, 32);
        return;
    }
    /* d >= 1: pdiff1 / round(d), exactly like the cairn bridge.  The divisor is
     * kept below 2^48 so rem * 256 fits in 64 bits (no __int128 on 32-bit ARM). */
    double dr = floor(d + 0.5);
    if (dr > 281474976710655.0) dr = 281474976710655.0;
    uint64_t div = (uint64_t)dr;
    if (div < 1) div = 1;
    uint64_t rem = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t acc = (rem << 8) | p1[i];
        t[i] = (uint8_t)(acc / div);
        rem = acc % div;
    }
}

int cp_selftest(void)
{
    static const uint8_t abc_exp[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    uint8_t o[32];
    cp_sha256((const uint8_t *)"abc", 3, o);
    if (memcmp(o, abc_exp, 32)) return 1;
    /* midstate + tail path must equal the plain double hash */
    uint8_t h[84]; for (int i = 0; i < 84; i++) h[i] = (uint8_t)(i * 37 + 11);
    uint32_t mid[8], tail[4]; cp_fpga_job(h, mid, tail);
    uint8_t blk[64]; memset(blk, 0, 64);
    for (int k = 0; k < 4; k++) put_be32(blk + 4 * k, tail[k]);
    memcpy(blk + 16, h + 80, 4); blk[20] = 0x80; blk[62] = 0x02; blk[63] = 0xA0;   /* 672 bits */
    uint32_t st[8]; memcpy(st, mid, 32); cp_compress(st, blk);
    uint8_t in2[32]; for (int k = 0; k < 8; k++) put_be32(in2 + 4 * k, st[k]);
    uint8_t a[32], b[32]; cp_sha256(in2, 32, a); cp_sha256d(h, 84, b);
    if (memcmp(a, b, 32)) return 2;
    uint8_t t1[32]; cp_target_from_diff(1, t1);
    if (t1[3] != 0 || t1[4] != 0xFF || t1[5] != 0xFF || t1[6] != 0) return 3;
    return 0;
}
