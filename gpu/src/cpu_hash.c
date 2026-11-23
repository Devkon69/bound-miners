/* BoundHash sulla CPU, per i thread del miner (bound-gpuminer --cpu).
 * Thread-safe: ogni thread ha il suo cpuh_t (scratchpad allineato + midstate del job).
 * Due versioni del round BLAKE2b/BlaMka: AVX2 (4 colonne per registro, come le implementazioni SIMD di Argon2)
 * e scalare. La scelta e' fatta all'avvio (cpuid + xgetbv); le due danno gli stessi hash (controllato in cpuh_selftest).
 * Per ogni nonce: una sola compressione SHA-256 per il seed (il midstate copre tag||tag e i primi 64 byte dell'header),
 * 4 SHA-256 per la riga iniziale, 4096 righe di fill, 4096 passi di mix, la SHA-256 finale. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bound_pow.h"
#include "cpu_hash.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <cpuid.h>
#define CPUH_X86 1
#endif

/* ---------------------------------------------------------------- SHA-256 (solo quello che serve qui) */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static const uint32_t IV[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void compress(uint32_t st[8], const uint8_t b[64])
{
    uint32_t w[64], a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4*i] << 24 | (uint32_t)b[4*i+1] << 16 | (uint32_t)b[4*i+2] << 8 | b[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    st[0] += a; st[1] += bb; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}
/* msg di len byte, dopo "done" byte gia' compressi in st */
static void sha_finish(uint32_t st[8], const uint8_t *msg, size_t len, uint64_t done, uint8_t out[32])
{
    uint8_t b[64];
    size_t i = 0;
    for (; len - i >= 64; i += 64) compress(st, msg + i);
    size_t r = len - i;
    memset(b, 0, 64); memcpy(b, msg + i, r); b[r] = 0x80;
    if (r >= 56) { compress(st, b); memset(b, 0, 64); }
    uint64_t bits = (done + len) * 8;
    for (int k = 0; k < 8; k++) b[63 - k] = (uint8_t)(bits >> (8 * k));
    compress(st, b);
    for (int k = 0; k < 8; k++) { out[4*k] = st[k] >> 24; out[4*k+1] = st[k] >> 16; out[4*k+2] = st[k] >> 8; out[4*k+3] = st[k]; }
}
static void sha256(const uint8_t *m, size_t n, uint8_t out[32]) { uint32_t st[8]; memcpy(st, IV, 32); sha_finish(st, m, n, 0, out); }

/* ---------------------------------------------------------------- round scalare */
static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
#define BM(x, y) ((x) + (y) + 2 * (uint64_t)(uint32_t)(x) * (uint32_t)(y))
#define G(a, b, c, d) do { \
    a = BM(a, b); d = rotr64(d ^ a, 32); c = BM(c, d); b = rotr64(b ^ c, 24); \
    a = BM(a, b); d = rotr64(d ^ a, 16); c = BM(c, d); b = rotr64(b ^ c, 63); } while (0)
static inline void rnd_s(uint64_t v[16])
{
    G(v[0], v[4], v[8], v[12]); G(v[1], v[5], v[9], v[13]); G(v[2], v[6], v[10], v[14]); G(v[3], v[7], v[11], v[15]);
    G(v[0], v[5], v[10], v[15]); G(v[1], v[6], v[11], v[12]); G(v[2], v[7], v[8], v[13]); G(v[3], v[4], v[9], v[14]);
}
static inline void mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
#if defined(__SIZEOF_INT128__)
    unsigned __int128 p = (unsigned __int128)a * b; *lo = (uint64_t)p; *hi = (uint64_t)(p >> 64);
#else
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = mid << 32 | (uint32_t)p00; *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}
static void core_scalar(uint64_t *pad, unsigned L, const uint64_t start[16], uint64_t vout[16])
{
    uint64_t t[16], x[16], v[16];
    /* riga 0 e riga 1 */
    memcpy(t, start, 128); rnd_s(t); for (int k = 0; k < 16; k++) pad[k] = t[k] ^ start[k];
    memcpy(t, pad, 128); t[15] ^= 1; rnd_s(t); for (int k = 0; k < 16; k++) pad[16 + k] = t[k] ^ pad[k];
    for (unsigned i = 2; i < L; i++) {
        const uint64_t *prev = pad + 16 * (size_t)(i - 1);
        uint32_t ref = (uint32_t)(((uint64_t)(uint32_t)prev[0] * (i - 1)) >> 32);
        const uint64_t *oth = pad + 16 * (size_t)ref;
        for (int k = 0; k < 16; k++) { x[k] = prev[k] ^ oth[k]; t[k] = x[k]; }
        t[15] ^= i;
        rnd_s(t);
        uint64_t *o = pad + 16 * (size_t)i;
        for (int k = 0; k < 16; k++) o[k] = t[k] ^ x[k];
    }
    for (int k = 0; k < 16; k++) v[k] = start[k] ^ pad[16 * (size_t)(L - 1) + k];
    for (uint64_t r = 0; r < L; r++) {
        uint64_t *l = pad + 16 * (size_t)(v[0] & (L - 1));
        for (int k = 0; k < 16; k++) v[k] ^= l[k];
        uint64_t hi, lo;
        mul128(v[0], v[1] | 1, &hi, &lo);
        v[2] += hi; v[3] ^= lo; v[15] ^= r;
        rnd_s(v);
        for (int k = 0; k < 16; k++) l[k] += v[k];
    }
    memcpy(vout, v, 128);
}

/* ---------------------------------------------------------------- round AVX2: righe a (v0..3), b (v4..7), c (v8..11), d (v12..15) */
#ifdef CPUH_X86
#define BMV(x, y) _mm256_add_epi64(_mm256_add_epi64(x, y), _mm256_add_epi64(_mm256_mul_epu32(x, y), _mm256_mul_epu32(x, y)))
#define LD(p) _mm256_load_si256((const __m256i *)(p))
#define ST(p, x) _mm256_store_si256((__m256i *)(p), x)
/* ---- versione generica a N hash intrecciati (N = 1..4); con AVX-512VL le rotazioni sono vprorq e ci sono 32 registri */
#define ROT_AVX2(x, n) ((n) == 32 ? _mm256_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)) : (n) == 24 ? _mm256_shuffle_epi8(x, r24) : \
                        (n) == 16 ? _mm256_shuffle_epi8(x, r16) : _mm256_xor_si256(_mm256_srli_epi64(x, 63), _mm256_add_epi64(x, x)))
#define ROT_512(x, n) _mm256_ror_epi64(x, n)
#define CORE_N(NAME, TGT, ROT)                                                                                              \
TGT static inline __attribute__((always_inline)) void NAME(uint64_t *const *P, unsigned L, const int N, const uint64_t *const *S, uint64_t *const *VO) \
{                                                                                                                         \
    const __m256i r24 = _mm256_setr_epi8(3,4,5,6,7,0,1,2, 11,12,13,14,15,8,9,10, 3,4,5,6,7,0,1,2, 11,12,13,14,15,8,9,10);   \
    const __m256i r16 = _mm256_setr_epi8(2,3,4,5,6,7,0,1, 10,11,12,13,14,15,8,9, 2,3,4,5,6,7,0,1, 10,11,12,13,14,15,8,9);   \
    (void)r24; (void)r16;                                                                                                 \
    __m256i x[4][4], v[4][4];                                                                                             \
    _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N)                                                         \
        for (int k = 0; k < 4; k++) x[n][k] = _mm256_loadu_si256((const __m256i *)(S[n] + 4 * k));                         \
    for (unsigned i = 0; i < L; i++) {                                                                                    \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) {                                                   \
            uint64_t *p = P[n];                                                                                           \
            if (i >= 2) {                                                                                                 \
                const uint64_t *q = p + 16 * (size_t)(i - 1);                                                             \
                const uint64_t *o = p + 16 * (size_t)(uint32_t)(((uint64_t)(uint32_t)q[0] * (i - 1)) >> 32);             \
                for (int k = 0; k < 4; k++) x[n][k] = _mm256_xor_si256(LD(q + 4 * k), LD(o + 4 * k));                     \
            } else if (i == 1) for (int k = 0; k < 4; k++) x[n][k] = LD(p + 4 * k);                                       \
            v[n][0] = x[n][0]; v[n][1] = x[n][1]; v[n][2] = x[n][2];                                                      \
            v[n][3] = _mm256_xor_si256(x[n][3], _mm256_set_epi64x((long long)i, 0, 0, 0));                                \
        }                                                                                                                 \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) RNDN(v[n][0], v[n][1], v[n][2], v[n][3], ROT);      \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) {                                                   \
            uint64_t *o = P[n] + 16 * (size_t)i;                                                                          \
            for (int k = 0; k < 4; k++) ST(o + 4 * k, _mm256_xor_si256(v[n][k], x[n][k]));                                \
        }                                                                                                                 \
    }                                                                                                                     \
    _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) {                                                       \
        const uint64_t *e = P[n] + 16 * (size_t)(L - 1);                                                                  \
        for (int k = 0; k < 4; k++) v[n][k] = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(S[n] + 4 * k)), LD(e + 4 * k)); \
    }                                                                                                                     \
    for (uint64_t r = 0; r < L; r++) {                                                                                    \
        uint64_t *l[4];                                                                                                   \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) {                                                   \
            l[n] = P[n] + 16 * (size_t)((uint64_t)_mm256_extract_epi64(v[n][0], 0) & (L - 1));                            \
            for (int k = 0; k < 4; k++) v[n][k] = _mm256_xor_si256(v[n][k], LD(l[n] + 4 * k));                            \
            uint64_t hi, lo;                                                                                              \
            mul128((uint64_t)_mm256_extract_epi64(v[n][0], 0), (uint64_t)_mm256_extract_epi64(v[n][0], 1) | 1, &hi, &lo); \
            v[n][0] = _mm256_xor_si256(_mm256_add_epi64(v[n][0], _mm256_set_epi64x(0, (long long)hi, 0, 0)),              \
                                       _mm256_set_epi64x((long long)lo, 0, 0, 0));                                        \
            v[n][3] = _mm256_xor_si256(v[n][3], _mm256_set_epi64x((long long)r, 0, 0, 0));                                \
        }                                                                                                                 \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N) RNDN(v[n][0], v[n][1], v[n][2], v[n][3], ROT);      \
        _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N)                                                     \
            for (int k = 0; k < 4; k++) ST(l[n] + 4 * k, _mm256_add_epi64(LD(l[n] + 4 * k), v[n][k]));                    \
    }                                                                                                                     \
    _Pragma("GCC unroll 4") for (int n = 0; n < 4; n++) if (n < N)                                                         \
        for (int k = 0; k < 4; k++) _mm256_storeu_si256((__m256i *)(VO[n] + 4 * k), v[n][k]);                             \
}
#define GVN(a, b, c, d, ROT) do { \
    a = BMV(a, b); d = ROT(_mm256_xor_si256(d, a), 32); c = BMV(c, d); b = ROT(_mm256_xor_si256(b, c), 24); \
    a = BMV(a, b); d = ROT(_mm256_xor_si256(d, a), 16); c = BMV(c, d); b = ROT(_mm256_xor_si256(b, c), 63); } while (0)
#define RNDN(a, b, c, d, ROT) do { \
    GVN(a, b, c, d, ROT); \
    b = _mm256_permute4x64_epi64(b, _MM_SHUFFLE(0, 3, 2, 1)); c = _mm256_permute4x64_epi64(c, _MM_SHUFFLE(1, 0, 3, 2)); \
    d = _mm256_permute4x64_epi64(d, _MM_SHUFFLE(2, 1, 0, 3)); \
    GVN(a, b, c, d, ROT); \
    b = _mm256_permute4x64_epi64(b, _MM_SHUFFLE(2, 1, 0, 3)); c = _mm256_permute4x64_epi64(c, _MM_SHUFFLE(1, 0, 3, 2)); \
    d = _mm256_permute4x64_epi64(d, _MM_SHUFFLE(0, 3, 2, 1)); } while (0)
#define TGT2 __attribute__((target("avx2")))
#define TGT5 __attribute__((target("avx2,avx512f,avx512vl")))
CORE_N(core_avx2_g, TGT2, ROT_AVX2)
CORE_N(core_512_g, TGT5, ROT_512)
typedef void (*core_fn)(uint64_t *const *, unsigned, const uint64_t *const *, uint64_t *const *);
#define SPEC(G, T, N) T static void G##_##N(uint64_t *const *P, unsigned L, const uint64_t *const *S, uint64_t *const *VO) { G##_g(P, L, N, S, VO); }
SPEC(core_avx2, TGT2, 1) SPEC(core_avx2, TGT2, 2) SPEC(core_avx2, TGT2, 3) SPEC(core_avx2, TGT2, 4)
SPEC(core_512, TGT5, 1) SPEC(core_512, TGT5, 2) SPEC(core_512, TGT5, 3) SPEC(core_512, TGT5, 4)
static const core_fn cores_avx2[5] = { 0, core_avx2_1, core_avx2_2, core_avx2_3, core_avx2_4 };
static const core_fn cores_512[5] = { 0, core_512_1, core_512_2, core_512_3, core_512_4 };
/* 0 = niente, 1 = AVX2, 2 = AVX2 + AVX-512F/VL (con il supporto del sistema operativo per i registri) */
static int simd_level(void)
{
    unsigned a, b, c, d, lo, hi;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return 0;               /* OSXSAVE, AVX */
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6 || __get_cpuid_max(0, 0) < 7) return 0;            /* il sistema salva XMM e YMM */
    __cpuid_count(7, 0, a, b, c, d);
    if (!((b >> 5) & 1)) return 0;
    if (((b >> 16) & 1) && ((b >> 31) & 1) && (lo & 0xE0) == 0xE0) return 2;   /* AVX-512F, VL; opmask e ZMM salvati */
    return 1;
}
#endif

/* ---------------------------------------------------------------- interfaccia */
static int simd = -1;                    /* 0 scalare, 1 AVX2, 2 AVX-512VL */
static uint32_t mid_seed[8], mid_res[8];
static void tag_mid(const char *tag, uint32_t st[8])
{
    uint8_t t[32], b[64];
    sha256((const uint8_t *)tag, strlen(tag), t);
    memcpy(b, t, 32); memcpy(b + 32, t, 32);
    memcpy(st, IV, 32); compress(st, b);
}
const char *cpuh_engine(void)
{
    if (simd < 0) {
        tag_mid("BOUND/BoundHash", mid_seed); tag_mid("BOUND/BoundHash-result", mid_res);
        const char *e = getenv("BOUND_CPU_SIMD");          /* per le prove: 0, 1, 2 */
#ifdef CPUH_X86
        simd = simd_level();
        if (e && atoi(e) < simd) simd = atoi(e);
#else
        simd = 0; (void)e;
#endif
    }
    return simd == 2 ? "AVX-512" : simd == 1 ? "AVX2" : "scalare";
}
static void *amalloc(size_t n)
{
#if defined(_WIN32)
    return _aligned_malloc(n, 64);
#else
    void *p; return posix_memalign(&p, 64, n) ? NULL : p;
#endif
}
static void afree(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}
int cpuh_init(cpuh_t *h, unsigned lines, int lanes)
{
    cpuh_engine();
    memset(h, 0, sizeof *h);
    if (lanes < 1) lanes = 1;
    if (lanes > CPUH_MAXLANES) lanes = CPUH_MAXLANES;
    h->lines = lines; h->lanes = lanes;
    for (int n = 0; n < lanes; n++) if (!(h->pad[n] = amalloc((size_t)lines * 128))) return -1;
    return 0;
}
void cpuh_free(cpuh_t *h) { for (int n = 0; n < CPUH_MAXLANES; n++) { afree(h->pad[n]); h->pad[n] = NULL; } }
void cpuh_job(cpuh_t *h, const uint8_t header[80])
{
    memcpy(h->mid, mid_seed, 32);
    compress(h->mid, header);
    memcpy(h->tail, header + 64, 12);
}
static void seed_start(const cpuh_t *h, uint32_t nonce, uint8_t seed[32], uint64_t start[16])
{
    uint32_t st[8];
    uint8_t tail[16], in[33], dg[32];
    memcpy(tail, h->tail, 12);
    tail[12] = (uint8_t)nonce; tail[13] = (uint8_t)(nonce >> 8); tail[14] = (uint8_t)(nonce >> 16); tail[15] = (uint8_t)(nonce >> 24);
    memcpy(st, h->mid, 32);
    sha_finish(st, tail, 16, 128, seed);
    memcpy(in, seed, 32);
    for (int k = 0; k < 4; k++) {
        in[32] = (uint8_t)k; sha256(in, 33, dg);
        for (int q = 0; q < 4; q++) { uint64_t w = 0; for (int i = 7; i >= 0; i--) w = w << 8 | dg[8 * q + i]; start[4 * k + q] = w; }
    }
}
static void finish(const uint8_t seed[32], const uint64_t v[16], uint8_t out[32])
{
    uint8_t buf[32 + 128]; uint32_t st[8];
    memcpy(buf, seed, 32);
    for (int k = 0; k < 16; k++) for (int i = 0; i < 8; i++) buf[32 + 8 * k + i] = (uint8_t)(v[k] >> (8 * i));
    memcpy(st, mid_res, 32);
    sha_finish(st, buf, sizeof buf, 64, out);
}
/* h->lanes hash insieme: nonce[n] -> out[n] */
void cpuh_hashn(cpuh_t *h, const uint32_t *nonce, uint8_t (*out)[32])
{
    uint8_t seed[CPUH_MAXLANES][32]; uint64_t start[CPUH_MAXLANES][16], v[CPUH_MAXLANES][16];
    const uint64_t *S[CPUH_MAXLANES]; uint64_t *V[CPUH_MAXLANES];
    int N = h->lanes;
    for (int n = 0; n < N; n++) { seed_start(h, nonce[n], seed[n], start[n]); S[n] = start[n]; V[n] = v[n]; }
#ifdef CPUH_X86
    if (simd) (simd == 2 ? cores_512 : cores_avx2)[N](h->pad, h->lines, S, V); else
#endif
    for (int n = 0; n < N; n++) core_scalar(h->pad[n], h->lines, start[n], v[n]);
    for (int n = 0; n < N; n++) finish(seed[n], v[n], out[n]);
}
void cpuh_hash(cpuh_t *h, uint32_t nonce, uint8_t out[32])
{
    uint8_t seed[32]; uint64_t start[16], v[16];
    seed_start(h, nonce, seed, start);
    core_scalar(h->pad[0], h->lines, start, v);
    finish(seed, v, out);
}
/* ogni versione disponibile (scalare, AVX2, AVX-512) con 1..4 hash insieme, contro bound_pow.c (verificato coi vettori ufficiali) */
int cpuh_selftest(unsigned lines)
{
    uint8_t hdr[80], a[CPUH_MAXLANES][32], b[32];
    uint32_t nn[CPUH_MAXLANES];
    int bad = 0, keep = simd;
    for (int i = 0; i < 80; i++) hdr[i] = (uint8_t)(i * 37 + 11);
    for (int lv = 0; lv <= keep; lv++) for (int N = 1; N <= CPUH_MAXLANES; N++) {
        cpuh_t h;
        if (cpuh_init(&h, lines, N)) return -1;
        cpuh_job(&h, hdr); simd = lv;
        for (int n = 0; n < N; n++) nn[n] = 0x9E3779B9u * (uint32_t)(n + 1) + (uint32_t)N;
        cpuh_hashn(&h, nn, a);
        for (int n = 0; n < N; n++) { bp_hash_nonce(hdr, nn[n], b); bad += memcmp(a[n], b, 32) != 0; }
        cpuh_free(&h);
    }
    simd = keep;
    return bad;
}
