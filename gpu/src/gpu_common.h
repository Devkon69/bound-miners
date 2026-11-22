/* gpu_common.h - parte comune di bound-gpubench e bound-gpuminer: orologio, log, driver CUDA caricato a runtime
 * (nvcuda.dll / libcuda.so.1), preparazione del lavoro e lancio del kernel. */
#ifndef GPU_COMMON_H
#define GPU_COMMON_H
#ifdef _WIN32
#include <windows.h>
static double now_s(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static void *lib_open(void) { return (void *)LoadLibraryA("nvcuda.dll"); }
static void *lib_sym(void *l, const char *n) { return (void *)GetProcAddress((HMODULE)l, n); }
#define CUDALIB "nvcuda.dll"
#else
#include <dlfcn.h>
#include <time.h>
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *lib_open(void) { return dlopen("libcuda.so.1", RTLD_NOW); }
static void *lib_sym(void *l, const char *n) { return dlsym(l, n); }
#define CUDALIB "libcuda.so.1"
#endif

void bp_tag_mid(const char *tag, uint32_t st[8]);
void bound_cpu_launch(unsigned long long *pad, const unsigned *job, unsigned *found, unsigned *out_hash, unsigned nthreads);

static FILE *logf;
static void say(const char *fmt, ...)
{
    va_list a; char b[1024];
    va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fputs(b, stdout); fflush(stdout);
    if (logf) { fputs(b, logf); fflush(logf); }
}

/* ------------------------------------------------------------------ driver CUDA */
typedef unsigned long long dptr;
static int (*cuInit)(unsigned), (*cuDriverGetVersion)(int *), (*cuDeviceGetCount)(int *), (*cuDeviceGet)(int *, int),
    (*cuDeviceGetName)(char *, int, int), (*cuDeviceTotalMem)(size_t *, int), (*cuDeviceGetAttribute)(int *, int, int),
    (*cuCtxCreate)(void **, unsigned, int), (*cuCtxDestroy)(void *), (*cuCtxSynchronize)(void),
    (*cuModuleLoadDataEx)(void **, const void *, unsigned, int *, void **), (*cuModuleGetFunction)(void **, void *, const char *),
    (*cuMemAlloc)(dptr *, size_t), (*cuMemFree)(dptr), (*cuMemGetInfo)(size_t *, size_t *),
    (*cuMemcpyHtoD)(dptr, const void *, size_t), (*cuMemcpyDtoH)(void *, dptr, size_t),
    (*cuLaunchKernel)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void *, void **, void **);

static int cuda_load(void)
{
    void *l = lib_open();
    if (!l) { say("driver CUDA non trovato (" CUDALIB "): serve una scheda NVIDIA con il driver installato\n"); return -1; }
#define S(v, n) do { *(void **)&v = lib_sym(l, n); if (!v) { say("nel driver manca %s\n", n); return -1; } } while (0)
    S(cuInit, "cuInit"); S(cuDriverGetVersion, "cuDriverGetVersion"); S(cuDeviceGetCount, "cuDeviceGetCount"); S(cuDeviceGet, "cuDeviceGet");
    S(cuDeviceGetName, "cuDeviceGetName"); S(cuDeviceTotalMem, "cuDeviceTotalMem_v2"); S(cuDeviceGetAttribute, "cuDeviceGetAttribute");
    S(cuCtxCreate, "cuCtxCreate_v2"); S(cuCtxDestroy, "cuCtxDestroy_v2"); S(cuCtxSynchronize, "cuCtxSynchronize");
    S(cuModuleLoadDataEx, "cuModuleLoadDataEx"); S(cuModuleGetFunction, "cuModuleGetFunction");
    S(cuMemAlloc, "cuMemAlloc_v2"); S(cuMemFree, "cuMemFree_v2"); S(cuMemGetInfo, "cuMemGetInfo_v2");
    S(cuMemcpyHtoD, "cuMemcpyHtoD_v2"); S(cuMemcpyDtoH, "cuMemcpyDtoH_v2"); S(cuLaunchKernel, "cuLaunchKernel");
#undef S
    return 0;
}
#define CK(call) do { int e_ = (call); if (e_) { say("errore CUDA %d in %s\n", e_, #call); return -1; } } while (0)

/* ------------------------------------------------------------------ un lancio: batch hash con nonce base.. */
typedef struct {
    int cpu;                    /* backend CPU (prova) */
    unsigned lines, block;
    size_t batch_alloc;
    void *fn;
    dptr d_pad, d_job, d_found, d_hash;
    unsigned long long *h_pad;  /* backend CPU */
    uint32_t job[24], found[256];
    uint32_t *hashes;           /* batch x 8 parole, quando chiesto */
} gpu_t;

static void make_job(gpu_t *g, const uint8_t header[80], uint32_t base, uint64_t target64, int want_hashes)
{
    uint8_t st[BP_STATE_BYTES];
    bp_job_state(header, st);
    for (int i = 0; i < 11; i++) g->job[i] = (uint32_t)st[4*i] << 24 | (uint32_t)st[4*i+1] << 16 | (uint32_t)st[4*i+2] << 8 | st[4*i+3];
    g->job[11] = base; g->job[12] = (uint32_t)(target64 >> 32); g->job[13] = (uint32_t)target64;
    g->job[14] = g->lines; g->job[15] = want_hashes ? 1 : 0;
    bp_tag_mid("BOUND/BoundHash-result", g->job + 16);
}

static int launch(gpu_t *g, unsigned batch)
{
    memset(g->found, 0, sizeof g->found);
    if (g->cpu) {
        bound_cpu_launch(g->h_pad, g->job, g->found, g->hashes, batch);
        return 0;
    }
    CK(cuMemcpyHtoD(g->d_job, g->job, sizeof g->job));
    CK(cuMemcpyHtoD(g->d_found, g->found, sizeof g->found));
    void *params[4] = { &g->d_pad, &g->d_job, &g->d_found, &g->d_hash };
    CK(cuLaunchKernel(g->fn, batch / g->block, 1, 1, g->block, 1, 1, 0, NULL, params, NULL));
    CK(cuCtxSynchronize());
    CK(cuMemcpyDtoH(g->found, g->d_found, sizeof g->found));
    if (g->job[15] & 1) CK(cuMemcpyDtoH(g->hashes, g->d_hash, (size_t)batch * 32));
    return 0;
}

/* apre la scheda dev_i (o il backend CPU), carica il kernel e alloca gli scratchpad per `want` hash in volo
 * (0 = quanti ne stanno nella memoria libera meno 1 GiB, al piu' 40960). Ritorna 0 se va bene. */
static void *gpu_ctx;
static size_t gpu_cap = 40960;   /* tetto degli hash in volo quando want = 0 */
__attribute__((unused)) static int gpu_open(gpu_t *g, int dev_i, int cpu, unsigned lines, unsigned want, const void *ptx)
{
    memset(g, 0, sizeof *g);
    g->cpu = cpu; g->lines = lines; g->block = 128;
    size_t per = (size_t)lines * 128, freeb = 0, totalb = 0;
    if (!cpu) {
        if (cuda_load()) return -1;
        int ver = 0, count = 0, dev = 0, maj = 0, min = 0, sms = 0;
        char name[128] = "";
        CK(cuInit(0)); cuDriverGetVersion(&ver); CK(cuDeviceGetCount(&count));
        if (dev_i >= count) { say("la scheda %d non c'e' (%d schede)\n", dev_i, count); return -1; }
        CK(cuDeviceGet(&dev, dev_i));
        cuDeviceGetAttribute(&maj, 75, dev); cuDeviceGetAttribute(&min, 76, dev); cuDeviceGetAttribute(&sms, 16, dev);
        cuDeviceGetName(name, sizeof name, dev);
        CK(cuCtxCreate(&gpu_ctx, 4 /* blocking sync */, dev));
        CK(cuMemGetInfo(&freeb, &totalb));
        say("GPU %d: %s, compute %d.%d, %d SM, memoria libera %.0f di %.0f MiB, driver CUDA %d.%d\n", dev_i, name, maj, min, sms,
            freeb / 1048576.0, totalb / 1048576.0, ver / 1000, (ver % 1000) / 10);
        void *mod = NULL; static char jitlog[8192];
        int opts[2] = { 5, 6 };
        void *vals[2] = { jitlog, (void *)(size_t)sizeof jitlog };
        int e = cuModuleLoadDataEx(&mod, ptx, 2, opts, vals);
        if (e) { say("il driver non carica il kernel (errore %d): %s\nServe il driver NVIDIA 550 o piu' recente e una scheda RTX 20 o piu' nuova.\n", e, jitlog); return -1; }
        CK(cuModuleGetFunction(&g->fn, mod, "bound_kernel"));
    } else freeb = (size_t)512 << 20;
    size_t keep = cpu ? 0 : (size_t)1 << 30;
    size_t maxb = freeb > keep ? (freeb - keep) / (per + 32) : 0;
    if (maxb > 40960) maxb = 40960;
    if (!want && maxb > gpu_cap) maxb = gpu_cap;
    if (want) { if (want > maxb) { say("%u hash in volo non stanno in memoria (al piu' %u)\n", want, (unsigned)maxb); return -1; } maxb = want; }
    maxb &= ~(size_t)255;
    if (maxb < 256) { say("memoria insufficiente (%.0f MiB liberi)\n", freeb / 1048576.0); return -1; }
    g->batch_alloc = maxb;
    g->hashes = malloc(maxb * 32);
    if (cpu) g->h_pad = malloc(maxb * per);
    else {
        CK(cuMemAlloc(&g->d_pad, maxb * per)); CK(cuMemAlloc(&g->d_job, sizeof g->job));
        CK(cuMemAlloc(&g->d_found, sizeof g->found)); CK(cuMemAlloc(&g->d_hash, maxb * 32));
    }
    return 0;
}
#endif
