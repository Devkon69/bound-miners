// bound_kernel.cu - BoundHash (moneta BOUND) su GPU: un thread per hash, uno scratchpad da 512 KiB per thread.
//
// Tutto l'hash e' nel kernel: SHA-256 del seme (dal midstate dei primi 64 byte dell'header), stato iniziale,
// fill, mix e SHA-256 finale. L'algoritmo non ha salti che dipendono dai dati (cambiano solo gli indirizzi):
// i 32 thread di un warp restano allineati e ogni passo e' una lettura e una scrittura di 128 byte a un
// indirizzo casuale dello scratchpad del thread. Il limite e' la banda della VRAM (circa 2,1 MB per hash).
//
// Lo stesso sorgente si compila come C++ per la CPU (-DCPU_SHIM): e' cosi' che il kernel viene verificato
// bit a bit contro bound_pow.c senza una GPU. E si compila anche come OpenCL C (__OPENCL_VERSION__, dal driver
// della scheda a runtime): e' il kernel per le GPU AMD e Intel, dedicate e integrate.
//
// job[0..7]   midstate SHA-256 dopo tag||tag e i primi 64 byte dell'header
// job[8..10]  byte 64..75 dell'header come 3 parole big-endian
// job[11]     base del nonce: il thread t prova base + t
// job[12..13] 64 bit alti del target (alta, bassa): trovato se hash >> 192 <= target64
// job[14]     righe dello scratchpad (4096; 64 su regtest)
// job[15]     bit 0: scrive anche tutti gli hash in out_hash (8 parole big-endian per thread), per i controlli
// job[16..23] stato SHA-256 dopo il blocco tag||tag di "BOUND/BoundHash-result"
// found[0]    numero di nonce trovati; found[1..] i nonce (al piu' FOUND_MAX)
// i cicli sulle 16 parole sono srotolati a mano: con un indice variabile il compilatore terrebbe gli array in memoria
#define FOR16(OP) OP(0) OP(1) OP(2) OP(3) OP(4) OP(5) OP(6) OP(7) OP(8) OP(9) OP(10) OP(11) OP(12) OP(13) OP(14) OP(15)
#define FOR8(OP) OP(0) OP(1) OP(2) OP(3) OP(4) OP(5) OP(6) OP(7)

#if defined(__OPENCL_VERSION__)
typedef ulong u64;
typedef uint u32;
#define DEV inline
#define GMEM __global
#define mulhi64(a, b) mul_hi((ulong)(a), (ulong)(b))
#define MULW(a, b) upsample(mul_hi((uint)(a), (uint)(b)), (uint)(a) * (uint)(b))
#define LD2_(k) { ulong2 a_ = q_[k]; dst_[2 * k] = a_.x; dst_[2 * k + 1] = a_.y; }
#define ST2_(k) { ulong2 a_; a_.x = src_[2 * k]; a_.y = src_[2 * k + 1]; q_[k] = a_; }
#define LOAD_LINE(dst, ptr)  do { u64 *dst_ = (dst); __global const ulong2 *q_ = (__global const ulong2 *)(ptr); FOR8(LD2_) } while (0)
#define STORE_LINE(ptr, src) do { const u64 *src_ = (src); __global ulong2 *q_ = (__global ulong2 *)(ptr); FOR8(ST2_) } while (0)
__constant u32 SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#elif defined(CPU_SHIM)
typedef unsigned long long u64;
typedef unsigned int u32;
#define DEV static inline
static inline u64 mulhi64(u64 a, u64 b) { return (u64)(((unsigned __int128)a * b) >> 64); }
#define MULW(a, b) ((u64)(u32)(a) * (u64)(u32)(b))
#define LD1_(k) dst_[k] = ptr_[k];
#define ST1_(k) ptr_[k] = src_[k];
#define LOAD_LINE(dst, ptr)  do { u64 *dst_ = (dst); const u64 *ptr_ = (ptr); FOR16(LD1_) } while (0)
#define STORE_LINE(ptr, src) do { u64 *ptr_ = (ptr); const u64 *src_ = (src); FOR16(ST1_) } while (0)
#define GMEM
#else
typedef unsigned long long u64;
typedef unsigned int u32;
#define DEV __device__ __forceinline__
#define mulhi64(a, b) __umul64hi(a, b)
// 32 x 32 -> 64 in una istruzione: PTX in linea, perche' partendo da due u64 mascherati il compilatore
// emette una moltiplicazione a 64 bit (mul.lo.s64)
__device__ __forceinline__ u64 mulw_(u32 a, u32 b)
{
    u64 r;
    asm("mul.wide.u32 %0, %1, %2;" : "=l"(r) : "r"(a), "r"(b));
    return r;
}
#define MULW(a, b) mulw_((u32)(a), (u32)(b))
#define LD2_(k) { ulonglong2 a_ = q_[k]; dst_[2 * k] = a_.x; dst_[2 * k + 1] = a_.y; }
#define ST2_(k) { ulonglong2 a_; a_.x = src_[2 * k]; a_.y = src_[2 * k + 1]; q_[k] = a_; }
#define LOAD_LINE(dst, ptr)  do { u64 *dst_ = (dst); const ulonglong2 *q_ = (const ulonglong2 *)(ptr); FOR8(LD2_) } while (0)
#define STORE_LINE(ptr, src) do { const u64 *src_ = (src); ulonglong2 *q_ = (ulonglong2 *)(ptr); FOR8(ST2_) } while (0)
#define GMEM
#endif
#define FOUND_MAX 255
#define RR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define BS32(x) ((((x) & 0xffu) << 24) | (((x) & 0xff00u) << 8) | (((x) >> 8) & 0xff00u) | ((x) >> 24))

DEV void sha_compress(u32 st[8], u32 w[16])
{
#if defined(__OPENCL_VERSION__)
#define K SHA_K
#else
    const u32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#endif
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        u32 wi;
        if (i < 16) wi = w[i];
        else {
            u32 w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            wi = w[i & 15] + (RR32(w15, 7) ^ RR32(w15, 18) ^ (w15 >> 3)) + w[(i + 9) & 15] + (RR32(w2, 17) ^ RR32(w2, 19) ^ (w2 >> 10));
            w[i & 15] = wi;
        }
        u32 t1 = h + (RR32(e, 6) ^ RR32(e, 11) ^ RR32(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + wi;
        u32 t2 = (RR32(a, 2) ^ RR32(a, 13) ^ RR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
#undef K
}

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define BM(x, y) ((x) + (y) + (MULW(x, y) << 1))
#define GG(a, b, c, d) do { \
    a = BM(a, b); d = ROTR64(d ^ a, 32); c = BM(c, d); b = ROTR64(b ^ c, 24); \
    a = BM(a, b); d = ROTR64(d ^ a, 16); c = BM(c, d); b = ROTR64(b ^ c, 63); } while (0)
#define ROUND(v) do { \
    GG(v[0], v[4], v[8], v[12]); GG(v[1], v[5], v[9], v[13]); GG(v[2], v[6], v[10], v[14]); GG(v[3], v[7], v[11], v[15]); \
    GG(v[0], v[5], v[10], v[15]); GG(v[1], v[6], v[11], v[12]); GG(v[2], v[7], v[8], v[13]); GG(v[3], v[4], v[9], v[14]); } while (0)

#define PXV_(k) p[k] ^= v[k];
#define VEP_(k) v[k] = p[k];
// un hash: thread t, scratchpad M (lines x 16 parole). Ritorna 1 se sta sotto il target; h = digest (8 parole BE)
DEV int bound_thread(u32 t, GMEM u64 *M, const u32 *job, u32 h[8])
{
    const u32 L = job[14];
    const u32 nonce = job[11] + t;
    u32 w[16], seed[8];
    // seme: ultimo blocco dell'header (12 byte, nonce little-endian, padding; messaggio di 64 + 80 byte)
    for (int i = 0; i < 8; i++) seed[i] = job[i];
    w[0] = job[8]; w[1] = job[9]; w[2] = job[10]; w[3] = BS32(nonce); w[4] = 0x80000000u;
    for (int i = 5; i < 15; i++) w[i] = 0;
    w[15] = 1152;
    sha_compress(seed, w);
    // stato iniziale: 4 x SHA-256(seme || k), parole da 64 bit little-endian
    u64 s[16], p[16], v[16];
    for (u32 k = 0; k < 4; k++) {
        u32 d[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        for (int i = 0; i < 8; i++) w[i] = seed[i];
        w[8] = (k << 24) | 0x00800000u;
        for (int i = 9; i < 15; i++) w[i] = 0;
        w[15] = 264;
        sha_compress(d, w);
        for (int q = 0; q < 4; q++) s[4 * k + q] = (u64)BS32(d[2 * q]) | ((u64)BS32(d[2 * q + 1]) << 32);
    }
    // fill: M[i] = F(M[i-1] ^ M[ref], i); p tiene l'ingresso x del round, poi la riga
#define OP_(k) p[k] = v[k] = s[k];
    FOR16(OP_)
#undef OP_
    ROUND(v);                                                   // riga 0 (v15 ^= 0)
    FOR16(PXV_)
    STORE_LINE(M, p);
    FOR16(VEP_)
    v[15] ^= 1;
    ROUND(v);                                                   // riga 1 = F(M[0], 1)
    FOR16(PXV_)
    STORE_LINE(M + 16, p);
    for (u32 i = 2; i < L; i++) {
        const u32 ref = (u32)(((u64)(u32)p[0] * (u64)(i - 1)) >> 32);
        LOAD_LINE(v, M + (u64)ref * 16);
        FOR16(PXV_) FOR16(VEP_)
        v[15] ^= i;
        ROUND(v);
        FOR16(PXV_)
        STORE_LINE(M + (u64)i * 16, p);
    }
    // mix
#define OP_(k) v[k] = s[k] ^ p[k];
    FOR16(OP_)
#undef OP_
    for (u32 r = 0; r < L; r++) {
        GMEM u64 *line = M + (u64)((u32)v[0] & (L - 1)) * 16;
        LOAD_LINE(p, line);
#define OP_(k) v[k] ^= p[k];
        FOR16(OP_)
#undef OP_
        const u64 m1 = v[1] | 1;
        v[2] += mulhi64(v[0], m1);
        v[3] ^= v[0] * m1;
        v[15] ^= r;
        ROUND(v);
#define OP_(k) p[k] += v[k];
        FOR16(OP_)
#undef OP_
        STORE_LINE(line, p);
    }
    // risultato: SHA-256(tag2 || tag2 || seme || v), messaggio di 64 + 160 byte
    for (int i = 0; i < 8; i++) h[i] = job[16 + i];
    for (int i = 0; i < 8; i++) w[i] = seed[i];
#define OP_(k) w[8 + 2 * k] = BS32((u32)v[k]); w[9 + 2 * k] = BS32((u32)(v[k] >> 32));
    OP_(0) OP_(1) OP_(2) OP_(3)
#undef OP_
    sha_compress(h, w);
#define OP_(k) w[2 * k] = BS32((u32)v[4 + k]); w[2 * k + 1] = BS32((u32)(v[4 + k] >> 32));
    FOR8(OP_)
#undef OP_
    sha_compress(h, w);
#define OP_(k) w[2 * k] = BS32((u32)v[12 + k]); w[2 * k + 1] = BS32((u32)(v[12 + k] >> 32));
    OP_(0) OP_(1) OP_(2) OP_(3)
#undef OP_
    w[8] = 0x80000000u;
    for (int i = 9; i < 15; i++) w[i] = 0;
    w[15] = 1792;
    sha_compress(h, w);
    // l'hash come numero ha l'ultimo byte del digest come piu' significativo
    const u32 hi = BS32(h[7]), lo = BS32(h[6]);
    return hi < job[12] || (hi == job[12] && lo <= job[13]);
}

#if defined(__OPENCL_VERSION__)
__kernel void bound_kernel(__global u64 *pad, __global const u32 *job, __global u32 *found, __global u32 *out_hash)
{
    const u32 t = (u32)get_global_id(0);
    u32 lj[24];
    for (int i = 0; i < 24; i++) lj[i] = job[i];
    u32 h[8];
    if (bound_thread(t, pad + (u64)t * lj[14] * 16, lj, h)) {
        u32 n = atomic_inc(found);
        if (n < FOUND_MAX) found[1 + n] = lj[11] + t;
    }
    if (lj[15] & 1) for (int i = 0; i < 8; i++) out_hash[(u64)t * 8 + i] = h[i];
}
#elif defined(CPU_SHIM)
extern "C" void bound_cpu_launch(u64 *pad, const u32 *job, u32 *found, u32 *out_hash, u32 nthreads)
{
    for (u32 t = 0; t < nthreads; t++) {
        u32 h[8];
        if (bound_thread(t, pad + (u64)t * job[14] * 16, job, h)) {
            u32 n = found[0]++;
            if (n < FOUND_MAX) found[1 + n] = job[11] + t;
        }
        if (job[15] & 1) for (int i = 0; i < 8; i++) out_hash[(u64)t * 8 + i] = h[i];
    }
}
#else
extern "C" __global__ void bound_kernel(u64 *pad, const u32 *job, u32 *found, u32 *out_hash)
{
    const u32 t = blockIdx.x * blockDim.x + threadIdx.x;
    u32 lj[24];
    for (int i = 0; i < 24; i++) lj[i] = job[i];
    u32 h[8];
    if (bound_thread(t, pad + (u64)t * lj[14] * 16, lj, h)) {
        u32 n = atomicAdd(found, 1u);
        if (n < FOUND_MAX) found[1 + n] = lj[11] + t;
    }
    if (lj[15] & 1) for (int i = 0; i < 8; i++) out_hash[(u64)t * 8 + i] = h[i];
}
#endif
