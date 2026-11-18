/* bound_pow.c - BOUND proof of work (BoundHash), see bound_pow.h.
 * Written from docs/boundhash.md of github.com/Sonic-Bound/Bound (commit e3549ec); checked against
 * the vectors of tests/test_boundhash.cpp. Portable C99: no 128-bit integers (ARMv7). */
#include "bound_pow.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ SHA-256 */
static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static const uint32_t IV[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(uint32_t st[8], const uint8_t b[64])
{
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4*i] << 24 | (uint32_t)b[4*i+1] << 16 | (uint32_t)b[4*i+2] << 8 | b[4*i+3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i-16] + (RR(w[i-15], 7) ^ RR(w[i-15], 18) ^ (w[i-15] >> 3)) + w[i-7] + (RR(w[i-2], 17) ^ RR(w[i-2], 19) ^ (w[i-2] >> 10));
    memcpy(s, st, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (RR(s[4], 6) ^ RR(s[4], 11) ^ RR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] + w[i];
        uint32_t t2 = (RR(s[0], 2) ^ RR(s[0], 13) ^ RR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) st[i] += s[i];
}

/* finish a message whose first `done` bytes (a multiple of 64) are already in st */
static void sha_finish(uint32_t st[8], const uint8_t *msg, size_t len, size_t done, uint8_t out[32])
{
    uint8_t blk[128];
    size_t total = done + len;
    while (len >= 64) { compress(st, msg); msg += 64; len -= 64; }
    memset(blk, 0, sizeof blk);
    memcpy(blk, msg, len);
    blk[len] = 0x80;
    size_t n = len + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)total * 8;
    for (int i = 0; i < 8; i++) blk[n - 1 - i] = (uint8_t)(bits >> (8 * i));
    compress(st, blk);
    if (n == 128) compress(st, blk + 64);
    for (int i = 0; i < 8; i++) { out[4*i] = st[i] >> 24; out[4*i+1] = st[i] >> 16; out[4*i+2] = st[i] >> 8; out[4*i+3] = st[i]; }
}

void bp_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t st[8]; memcpy(st, IV, sizeof st);
    sha_finish(st, msg, len, 0, out);
}

/* SHA-256 state after the block SHA256(tag) || SHA256(tag) */
static void tag_mid(const char *tag, uint32_t st[8])
{
    uint8_t t[64];
    bp_sha256((const uint8_t *)tag, strlen(tag), t);
    memcpy(t + 32, t, 32);
    memcpy(st, IV, 8 * sizeof(uint32_t));
    compress(st, t);
}

void bp_tagged_hash(const char *tag, const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t st[8]; tag_mid(tag, st);
    sha_finish(st, msg, len, 64, out);
}

static uint32_t mid_seed[8], mid_res[8];
static int mids_ready;
static void mids(void)
{
    if (mids_ready) return;
    tag_mid("BOUND/BoundHash", mid_seed);
    tag_mid("BOUND/BoundHash-result", mid_res);
    mids_ready = 1;
}

/* ------------------------------------------------------------------ BoundHash */
static unsigned bp_lines = 4096;
static uint64_t *pad;            /* bp_lines x 16 words */
static unsigned pad_lines;

int bp_set_lines(unsigned lines)
{
    if (lines < 2 || lines > (1u << 20) || (lines & (lines - 1))) return -1;
    bp_lines = lines;
    return 0;
}

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
static inline uint64_t ld64(const uint8_t *p)
{
    uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v;
}
#define BM(x, y) ((x) + (y) + 2 * (uint64_t)(uint32_t)(x) * (uint32_t)(y))
#define G(a, b, c, d) do { \
    a = BM(a, b); d = rotr64(d ^ a, 32); c = BM(c, d); b = rotr64(b ^ c, 24); \
    a = BM(a, b); d = rotr64(d ^ a, 16); c = BM(c, d); b = rotr64(b ^ c, 63); } while (0)
static void rnd(uint64_t v[16])
{
    G(v[0], v[4], v[8], v[12]); G(v[1], v[5], v[9], v[13]); G(v[2], v[6], v[10], v[14]); G(v[3], v[7], v[11], v[15]);
    G(v[0], v[5], v[10], v[15]); G(v[1], v[6], v[11], v[12]); G(v[2], v[7], v[8], v[13]); G(v[3], v[4], v[9], v[14]);
}
static void fill_line(const uint64_t x[16], uint64_t i, uint64_t out[16])
{
    uint64_t t[16];
    memcpy(t, x, sizeof t);
    t[15] ^= i;
    rnd(t);
    for (int k = 0; k < 16; k++) out[k] = t[k] ^ x[k];
}
static void mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = mid << 32 | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* seed (32 bytes) -> hash */
static void from_seed(const uint8_t seed[32], uint8_t out[32])
{
    unsigned L = bp_lines;
    if (!pad || pad_lines < L) {
        free(pad);
        pad = malloc((size_t)L * 16 * sizeof(uint64_t)); pad_lines = L;
        if (!pad) abort();
    }
    uint64_t start[16], x[16], v[16];
    uint8_t in[33], d[32];
    memcpy(in, seed, 32);
    for (int k = 0; k < 4; k++) {
        in[32] = (uint8_t)k;
        bp_sha256(in, 33, d);
        for (int q = 0; q < 4; q++) start[4 * k + q] = ld64(d + 8 * q);
    }
    fill_line(start, 0, pad);
    fill_line(pad, 1, pad + 16);
    for (unsigned i = 2; i < L; i++) {
        const uint64_t *prev = pad + 16 * (size_t)(i - 1);
        uint32_t ref = (uint32_t)(((uint64_t)(uint32_t)prev[0] * (i - 1)) >> 32);
        const uint64_t *oth = pad + 16 * (size_t)ref;
        for (int k = 0; k < 16; k++) x[k] = prev[k] ^ oth[k];
        fill_line(x, i, pad + 16 * (size_t)i);
    }
    for (int k = 0; k < 16; k++) v[k] = start[k] ^ pad[16 * (size_t)(L - 1) + k];
    for (uint64_t r = 0; r < L; r++) {
        uint64_t *l = pad + 16 * (size_t)(v[0] & (L - 1));
        for (int k = 0; k < 16; k++) v[k] ^= l[k];
        uint64_t hi, lo;
        mul128(v[0], v[1] | 1, &hi, &lo);
        v[2] += hi; v[3] ^= lo; v[15] ^= r;
        rnd(v);
        for (int k = 0; k < 16; k++) l[k] += v[k];
    }
    uint8_t buf[32 + 128];
    memcpy(buf, seed, 32);
    for (int k = 0; k < 16; k++) for (int i = 0; i < 8; i++) buf[32 + 8 * k + i] = (uint8_t)(v[k] >> (8 * i));
    uint32_t st[8];
    mids();
    memcpy(st, mid_res, sizeof st);
    sha_finish(st, buf, sizeof buf, 64, out);
}

void bp_boundhash(const uint8_t header[80], uint8_t out[32])
{
    uint8_t seed[32];
    uint32_t st[8];
    mids();
    memcpy(st, mid_seed, sizeof st);
    sha_finish(st, header, 80, 64, seed);
    from_seed(seed, out);
}

void bp_hash_nonce(const uint8_t header[80], uint32_t nonce, uint8_t out[32])
{
    uint8_t h[80];
    memcpy(h, header, 76);
    h[76] = (uint8_t)nonce; h[77] = (uint8_t)(nonce >> 8); h[78] = (uint8_t)(nonce >> 16); h[79] = (uint8_t)(nonce >> 24);
    bp_boundhash(h, out);
}

void bp_job_state(const uint8_t header[80], uint8_t state[BP_STATE_BYTES])
{
    uint32_t st[8];
    mids();
    memcpy(st, mid_seed, sizeof st);
    compress(st, header);
    memset(state, 0, BP_STATE_BYTES);
    for (int i = 0; i < 8; i++) { state[4*i] = st[i] >> 24; state[4*i+1] = st[i] >> 16; state[4*i+2] = st[i] >> 8; state[4*i+3] = st[i]; }
    memcpy(state + 32, header + 64, 12);
}

void bp_fpga_hash(const uint8_t state[BP_STATE_BYTES], uint32_t nonce, uint8_t out[32])
{
    uint32_t st[8];
    uint8_t tail[16], seed[32];
    for (int i = 0; i < 8; i++) st[i] = (uint32_t)state[4*i] << 24 | (uint32_t)state[4*i+1] << 16 | (uint32_t)state[4*i+2] << 8 | state[4*i+3];
    memcpy(tail, state + 32, 12);
    tail[12] = (uint8_t)nonce; tail[13] = (uint8_t)(nonce >> 8); tail[14] = (uint8_t)(nonce >> 16); tail[15] = (uint8_t)(nonce >> 24);
    sha_finish(st, tail, 16, 128, seed);
    from_seed(seed, out);
}

void bp_merkle(const uint8_t *cb, size_t cbn, const uint8_t (*branch)[32], int nbranch, uint8_t out[32])
{
    uint8_t pair[64];
    bp_tagged_hash("BOUND/wtxid", cb, cbn, out);
    for (int i = 0; i < nbranch; i++) {
        memcpy(pair, out, 32); memcpy(pair + 32, branch[i], 32);
        bp_tagged_hash("BOUND/merkle", pair, 64, out);
    }
}

uint64_t bp_top64(const uint8_t h[32])
{
    uint64_t v = 0; for (int i = 31; i >= 24; i--) v = v << 8 | h[i]; return v;
}

int bp_le256_le(const uint8_t a[32], const uint8_t b[32])
{
    for (int i = 31; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i];
    return 1;
}

void bp_target_from_diff(double d, uint8_t t[32])
{
    /* target = 0xFFFF * 2^208 / d: mantissa of 64 bits placed at the right exponent */
    memset(t, 0, 32);
    if (!(d > 0)) { memset(t, 0xFF, 32); return; }
    int e;
    double m = frexp(65535.0 / d, &e);            /* 65535/d = m * 2^e, 0.5 <= m < 1 */
    uint64_t mant = (uint64_t)ldexp(m, 64);       /* top 64 bits */
    int sh = 208 + e - 64;                        /* target = mant << sh */
    if (sh + 64 > 256) { memset(t, 0xFF, 32); return; }
    for (int i = 0; i < 64; i++) {
        int bit = sh + i;
        if (bit >= 0 && ((mant >> i) & 1)) t[bit / 8] |= (uint8_t)(1 << (bit % 8));
    }
}

double bp_diff_of(const uint8_t h[32])
{
    /* pdiff1 / hash, from the top 128 bits of the hash */
    double v = 0;
    for (int i = 31; i >= 16; i--) v = v * 256 + h[i];
    if (v <= 0) return 1e300;
    return 65535.0 * ldexp(1.0, 208 - 128) / v;
}

static void vector_header(uint32_t n, uint8_t h[80])
{
    char s[32];
    memset(h, 0, 80);
    h[0] = 1;
    int l = sprintf(s, "prev %u", (unsigned)n);   bp_sha256((const uint8_t *)s, (size_t)l, h + 4);
    l = sprintf(s, "merkle %u", (unsigned)n);     bp_sha256((const uint8_t *)s, (size_t)l, h + 36);
    uint32_t t = 1800000000u + n, bits = 0x1E03FFFFu;
    for (int i = 0; i < 4; i++) { h[68 + i] = (uint8_t)(t >> (8 * i)); h[72 + i] = (uint8_t)(bits >> (8 * i)); h[76 + i] = (uint8_t)(n >> (8 * i)); }
}

int bp_selftest(void)
{
    static const struct { unsigned lines, n; const char *hex; } v[] = {
        {64, 0, "a1da4b56c502232301c7e9b6d05a2a5eb2d14fe2cab122bec8322f662bf05893"},
        {64, 2, "8237b3e10288bb3b85b040ce43360ae68057eb88d1895ed21515cedccd8987ef"},
        {4096, 0, "a1bfe39eba36286133775ee3db0040b7d638827743a25e29a9eb95ab7e42676b"},
        {4096, 1, "b39b4468daa307a37e9758cb2eca618ff93bfde326420c9d4aad728ae8b435de"},
        {4096, 2, "da06793503671e059bf562f81ee8f1e5286bdcf06b0922c8b6365270df29d5df"},
    };
    unsigned keep = bp_lines;
    int bad = 0;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t h[80], out[32], out2[32], st[BP_STATE_BYTES];
        char hex[65];
        vector_header(v[i].n, h);
        bp_set_lines(v[i].lines);
        bp_boundhash(h, out);
        for (int k = 0; k < 32; k++) sprintf(hex + 2 * k, "%02x", out[31 - k]);   /* as explorers show it */
        if (strcmp(hex, v[i].hex)) bad |= 1;
        bp_job_state(h, st);                          /* the host / FPGA split gives the same hash */
        bp_fpga_hash(st, v[i].n, out2);
        if (memcmp(out, out2, 32)) bad |= 2;
    }
    bp_set_lines(keep);
    uint8_t t[32];
    bp_target_from_diff(1.0, t);                      /* 0xFFFF << 208 */
    if (t[26] != 0xFF || t[27] != 0xFF || t[28] || t[25] > 1) bad |= 4;
    bp_target_from_diff(1.0 / 1024, t);               /* 0xFFFF << 218 */
    if (bp_top64(t) != 0x000003FFFC000000ULL) bad |= 8;
    return bad;
}
