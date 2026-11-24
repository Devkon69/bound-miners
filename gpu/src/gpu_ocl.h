/* gpu_ocl.h - backend OpenCL di bound-gpuminer: GPU AMD (dedicate e integrate), Intel (Arc e integrate) e, se si vuole,
 * NVIDIA. La libreria OpenCL (OpenCL.dll / libOpenCL.so.1) arriva col driver della scheda e si carica a runtime: non serve
 * niente all'utente. Il kernel e' lo stesso sorgente di bound_kernel.cu (ramo __OPENCL_VERSION__), compilato dal driver
 * all'avvio (qualche secondo). Va incluso dopo gpu_common.h. */
#ifndef GPU_OCL_H
#define GPU_OCL_H
#include "kernel_cl.h"

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
#define CL_DEVICE_TYPE_CPU 2
#define CL_DEVICE_TYPE_GPU 4
#define CL_PLATFORM_NAME 0x0902
#define CL_DEVICE_TYPE 0x1000
#define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
#define CL_DEVICE_MAX_WORK_GROUP_SIZE 0x1004
#define CL_DEVICE_MAX_MEM_ALLOC_SIZE 0x1010
#define CL_DEVICE_GLOBAL_MEM_SIZE 0x101F
#define CL_DEVICE_HOST_UNIFIED_MEMORY 0x1035
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DEVICE_VERSION 0x102F
#define CL_DEVICE_BOARD_NAME_AMD 0x4038
#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_MEM_READ_WRITE 1

static cl_int (*clGetPlatformIDs)(cl_uint, void **, cl_uint *), (*clGetPlatformInfo)(void *, cl_uint, size_t, void *, size_t *),
    (*clGetDeviceIDs)(void *, cl_ulong, cl_uint, void **, cl_uint *), (*clGetDeviceInfo)(void *, cl_uint, size_t, void *, size_t *),
    (*clBuildProgram)(void *, cl_uint, void **, const char *, void *, void *),
    (*clGetProgramBuildInfo)(void *, void *, cl_uint, size_t, void *, size_t *), (*clReleaseMemObject)(void *),
    (*clSetKernelArg)(void *, cl_uint, size_t, const void *),
    (*clEnqueueWriteBuffer)(void *, void *, cl_uint, size_t, size_t, const void *, cl_uint, void *, void *),
    (*clEnqueueReadBuffer)(void *, void *, cl_uint, size_t, size_t, void *, cl_uint, void *, void *),
    (*clEnqueueNDRangeKernel)(void *, void *, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, void *, void *),
    (*clFinish)(void *);
static void *(*clCreateContext)(const intptr_t *, cl_uint, void **, void *, void *, cl_int *),
    *(*clCreateCommandQueue)(void *, void *, cl_ulong, cl_int *),
    *(*clCreateProgramWithSource)(void *, cl_uint, const char **, const size_t *, cl_int *),
    *(*clCreateKernel)(void *, const char *, cl_int *), *(*clCreateBuffer)(void *, cl_ulong, size_t, void *, cl_int *);

__attribute__((unused)) static int ocl_load(void)
{
    static int done = 0;
    if (done) return done > 0 ? 0 : -1;
    done = -1;
    const char *env = getenv("BOUND_OPENCL_LIB");
#ifdef _WIN32
    void *l = (void *)LoadLibraryA(env ? env : "OpenCL.dll");
#else
    void *l = dlopen(env ? env : "libOpenCL.so.1", RTLD_NOW);
    if (!l && !env) l = dlopen("libOpenCL.so", RTLD_NOW);
#endif
    if (!l) return -1;
#define S(v, n) do { *(void **)&v = lib_sym(l, n); if (!v) return -1; } while (0)
    S(clGetPlatformIDs, "clGetPlatformIDs"); S(clGetPlatformInfo, "clGetPlatformInfo"); S(clGetDeviceIDs, "clGetDeviceIDs");
    S(clGetDeviceInfo, "clGetDeviceInfo"); S(clCreateContext, "clCreateContext"); S(clCreateCommandQueue, "clCreateCommandQueue");
    S(clCreateProgramWithSource, "clCreateProgramWithSource"); S(clBuildProgram, "clBuildProgram");
    S(clGetProgramBuildInfo, "clGetProgramBuildInfo"); S(clCreateKernel, "clCreateKernel"); S(clCreateBuffer, "clCreateBuffer");
    S(clReleaseMemObject, "clReleaseMemObject"); S(clSetKernelArg, "clSetKernelArg"); S(clEnqueueWriteBuffer, "clEnqueueWriteBuffer");
    S(clEnqueueReadBuffer, "clEnqueueReadBuffer"); S(clEnqueueNDRangeKernel, "clEnqueueNDRangeKernel"); S(clFinish, "clFinish");
#undef S
    done = 1;
    return 0;
}

typedef struct {
    void *plat, *dev;
    char name[160], vendor[96], platname[96];
    unsigned cus, unified;
    cl_ulong gmem, maxalloc;
    int is_nvidia, is_cpu;
} ocl_info_t;

/* le GPU OpenCL di tutti i driver installati; skip_nvidia: le NVIDIA le usa gia' CUDA. Con BOUND_OPENCL_CPU=1 anche i
 * dispositivi OpenCL di tipo CPU (solo per le prove, per esempio POCL). */
__attribute__((unused)) static int ocl_list(ocl_info_t *out, int max, int skip_nvidia)
{
    void *pl[16]; cl_uint np = 0; int n = 0;
    if (ocl_load() || clGetPlatformIDs(16, pl, &np) || !np) return 0;
    cl_ulong types = CL_DEVICE_TYPE_GPU | (getenv("BOUND_OPENCL_CPU") ? CL_DEVICE_TYPE_CPU : 0);
    for (cl_uint p = 0; p < np && p < 16; p++) {
        void *dv[16]; cl_uint nd = 0; char pname[96] = "";
        clGetPlatformInfo(pl[p], CL_PLATFORM_NAME, sizeof pname, pname, NULL);
        if (clGetDeviceIDs(pl[p], types, 16, dv, &nd) || !nd) continue;
        for (cl_uint d = 0; d < nd && d < 16 && n < max; d++) {
            ocl_info_t *o = &out[n];
            memset(o, 0, sizeof *o);
            o->plat = pl[p]; o->dev = dv[d];
            snprintf(o->platname, sizeof o->platname, "%s", pname);
            clGetDeviceInfo(dv[d], CL_DEVICE_NAME, sizeof o->name, o->name, NULL);
            char board[160] = "";
            if (!clGetDeviceInfo(dv[d], CL_DEVICE_BOARD_NAME_AMD, sizeof board, board, NULL) && board[0])  /* AMD: "gfx1100" -> nome vero */
                snprintf(o->name, sizeof o->name, "%s", board);
            clGetDeviceInfo(dv[d], CL_DEVICE_VENDOR, sizeof o->vendor, o->vendor, NULL);
            clGetDeviceInfo(dv[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof o->cus, &o->cus, NULL);
            clGetDeviceInfo(dv[d], CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof o->unified, &o->unified, NULL);
            clGetDeviceInfo(dv[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof o->gmem, &o->gmem, NULL);
            clGetDeviceInfo(dv[d], CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof o->maxalloc, &o->maxalloc, NULL);
            cl_ulong ty = 0; clGetDeviceInfo(dv[d], CL_DEVICE_TYPE, sizeof ty, &ty, NULL);
            o->is_cpu = (ty & CL_DEVICE_TYPE_GPU) == 0;
            o->is_nvidia = strstr(o->vendor, "NVIDIA") || strstr(pname, "NVIDIA");
            if (skip_nvidia && o->is_nvidia) continue;
            n++;
        }
    }
    /* solo per le prove: BOUND_OPENCL_DUP=N fa vedere ogni dispositivo N volte (piu' schede con una sola) */
    int dup = getenv("BOUND_OPENCL_DUP") ? atoi(getenv("BOUND_OPENCL_DUP")) : 1, n0 = n;
    for (int r = 1; r < dup; r++) for (int i = 0; i < n0 && n < max; i++) { out[n] = out[i]; snprintf(out[n].name + strlen(out[n].name) > out[n].name + 140 ? out[n].name + 140 : out[n].name + strlen(out[n].name), 16, " #%d", r + 1); n++; }
    return n;
}

#define OK_CL(call) do { cl_int e_ = (call); if (e_) { say("%s: errore OpenCL %d in %s\n", g->name, (int)e_, #call); return -1; } } while (0)

static int ocl_alloc_pad(gpu_t *g, size_t n)
{
    cl_int e = 0;
    if (g->cl_pad) { clReleaseMemObject(g->cl_pad); g->cl_pad = NULL; }
    if (g->cl_hash) { clReleaseMemObject(g->cl_hash); g->cl_hash = NULL; }
    g->cl_pad = clCreateBuffer(g->cl_ctx, CL_MEM_READ_WRITE, n * g->lines * 128, NULL, &e);
    if (e || !g->cl_pad) { say("%s: memoria della scheda insufficiente per %u hash in volo (errore %d)\n", g->name, (unsigned)n, (int)e); return -1; }
    g->cl_hash = clCreateBuffer(g->cl_ctx, CL_MEM_READ_WRITE, n * 32, NULL, &e);
    if (e || !g->cl_hash) return -1;
    return 0;
}

/* apre la GPU OpenCL o, compila il kernel e alloca gli scratchpad per `want` hash in volo (0 = scelta automatica) */
__attribute__((unused)) static int gpu_open_ocl(gpu_t *g, const ocl_info_t *o, unsigned lines, unsigned want, unsigned cap)
{
    memset(g, 0, sizeof *g);
    g->kind = 1; g->lines = lines; g->block = 64;
    snprintf(g->name, sizeof g->name, "%s", o->name);
    size_t per = (size_t)lines * 128;
    say("%s (OpenCL, %s): %u unita' di calcolo, memoria %.0f MiB (blocco massimo %.0f MiB)%s\n", o->name, o->platname, o->cus,
        o->gmem / 1048576.0, o->maxalloc / 1048576.0, o->unified ? ", integrata (memoria condivisa col sistema)" : "");
    cl_int e = 0; void *dev = o->dev;
    intptr_t props[3] = { 0x1084 /* CL_CONTEXT_PLATFORM */, (intptr_t)o->plat, 0 };
    g->cl_ctx = clCreateContext(props, 1, &dev, NULL, NULL, &e);
    if (e || !g->cl_ctx) { say("%s: contesto OpenCL non creato (errore %d)\n", g->name, (int)e); return -1; }
    g->cl_q = clCreateCommandQueue(g->cl_ctx, dev, 0, &e);
    if (e || !g->cl_q) { say("%s: coda OpenCL non creata (errore %d)\n", g->name, (int)e); return -1; }
    say("%s: compilo il kernel (la prima volta puo' richiedere qualche decina di secondi)\n", g->name);
    const char *src = kernel_cl; size_t len = strlen(kernel_cl);
    void *prog = clCreateProgramWithSource(g->cl_ctx, 1, &src, &len, &e);
    if (e || !prog) { say("%s: programma OpenCL non creato (errore %d)\n", g->name, (int)e); return -1; }
    e = clBuildProgram(prog, 1, &dev, "", NULL, NULL);
    if (e) {
        static char blog[16384]; size_t bl = 0;
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof blog - 1, blog, &bl);
        blog[bl < sizeof blog ? bl : sizeof blog - 1] = 0;
        say("%s: il driver non compila il kernel (errore %d):\n%s\n", g->name, (int)e, blog);
        return -1;
    }
    g->cl_k = clCreateKernel(prog, "bound_kernel", &e);
    if (e || !g->cl_k) { say("%s: kernel non trovato (errore %d)\n", g->name, (int)e); return -1; }
    /* hash in volo: 64 per unita' di calcolo (come 8192 sulle 128 SM della 4090), nei limiti della memoria;
     * sulle integrate al piu' 2048 (1 GiB di RAM di sistema) */
    size_t maxb = o->maxalloc / per, memb = (size_t)(o->gmem * 0.6) / per;
    if (memb < maxb) maxb = memb;
    if (o->unified && maxb > 2048) maxb = 2048;
    if (maxb > cap) maxb = cap;
    size_t n = want ? want : (size_t)(o->cus ? o->cus : 16) * 64;
    if (n > maxb) n = maxb;
    n = n / g->block * g->block;
    if (n < g->block) { say("%s: memoria insufficiente\n", g->name); return -1; }
    g->batch_alloc = n;
    g->hashes = malloc(n * 32);
    if (ocl_alloc_pad(g, n)) return -1;
    g->cl_job = clCreateBuffer(g->cl_ctx, CL_MEM_READ_WRITE, sizeof g->job, NULL, &e); if (e) return -1;
    g->cl_found = clCreateBuffer(g->cl_ctx, CL_MEM_READ_WRITE, sizeof g->found, NULL, &e); if (e) return -1;
    OK_CL(clSetKernelArg(g->cl_k, 1, sizeof(void *), &g->cl_job));
    OK_CL(clSetKernelArg(g->cl_k, 2, sizeof(void *), &g->cl_found));
    return 0;
}

static int launch_ocl(gpu_t *g, unsigned batch)
{
    size_t gws = batch, lws = g->block;
    OK_CL(clSetKernelArg(g->cl_k, 0, sizeof(void *), &g->cl_pad));
    OK_CL(clSetKernelArg(g->cl_k, 3, sizeof(void *), &g->cl_hash));
    OK_CL(clEnqueueWriteBuffer(g->cl_q, g->cl_job, 0, 0, sizeof g->job, g->job, 0, NULL, NULL));
    OK_CL(clEnqueueWriteBuffer(g->cl_q, g->cl_found, 0, 0, sizeof g->found, g->found, 0, NULL, NULL));
    OK_CL(clEnqueueNDRangeKernel(g->cl_q, g->cl_k, 1, NULL, &gws, &lws, 0, NULL, NULL));
    OK_CL(clEnqueueReadBuffer(g->cl_q, g->cl_found, 1, 0, sizeof g->found, g->found, 0, NULL, NULL));
    if (g->job[15] & 1) OK_CL(clEnqueueReadBuffer(g->cl_q, g->cl_hash, 1, 0, (size_t)batch * 32, g->hashes, 0, NULL, NULL));
    return 0;
}
#endif
