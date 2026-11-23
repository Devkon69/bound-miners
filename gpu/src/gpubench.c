/* bound-gpubench - prova e misura del kernel CUDA BoundHash (moneta BOUND) su schede NVIDIA.
 *
 * Non serve il CUDA Toolkit: il driver CUDA (nvcuda.dll / libcuda.so.1) viene caricato all'avvio e il kernel e'
 * gia' compilato dentro l'eseguibile (PTX 8.4: driver NVIDIA 550 o piu' recente, schede RTX 20 in su).
 *
 *   bound-gpubench                    selftest sulla CPU, controllo degli hash della GPU, poi la tabella delle
 *                                     velocita' per numero di hash in volo e thread per blocco
 *   --gpu N         scheda da usare (default 0)        --seconds S   durata di ogni misura (default 6)
 *   --batch N       misura solo con N hash in volo     --block N     thread per blocco (default: prova 64 e 128)
 *   --check N       hash controllati sulla CPU (default 96)
 *   --lines N       righe dello scratchpad (default 4096; 64 = regtest, solo per prova)
 *   --cpu-backend   esegue il kernel compilato per la CPU (prova del programma senza GPU, lentissimo)
 * Scrive tutto anche in bound-gpubench.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "bound_pow.h"
#include "kernel_ptx.h"
#define VERSION "0.3"
#include "gpu_common.h"

/* gli hash della GPU contro bound_pow.c: `check` thread sparsi nel lancio, e la lista dei trovati */
static int check_launch(gpu_t *g, unsigned batch, unsigned check)
{
    uint8_t header[80];
    for (int i = 0; i < 80; i++) header[i] = (uint8_t)(i * 29 + 7);
    const uint32_t base = 0xFFFFF000u;                      /* il nonce fa anche il giro oltre 2^32 */
    const uint64_t target64 = 0x00FFFFFFFFFFFFFFull;        /* 1 su 256 sotto il target */
    make_job(g, header, base, target64, 1);
    double t0 = now_s();
    if (launch(g, batch)) return -1;
    double dt = now_s() - t0;
    unsigned bad = 0, step = batch / check ? batch / check : 1, n = 0;
    for (unsigned t = 0; t < batch; t += step, n++) {
        if (t + step >= batch) t = batch - 1;               /* l'ultimo thread sempre */
        uint8_t ref[32], got[32];
        bp_hash_nonce(header, base + t, ref);
        for (int i = 0; i < 8; i++) { uint32_t w = g->hashes[(size_t)t * 8 + i]; got[4*i] = w >> 24; got[4*i+1] = w >> 16; got[4*i+2] = w >> 8; got[4*i+3] = (uint8_t)w; }
        if (memcmp(ref, got, 32)) { if (!bad) say("  thread %u: hash diverso da quello della CPU\n", t); bad++; }
        if (t == batch - 1) { n++; break; }
    }
    /* i trovati: ogni thread il cui hash (dal buffer) sta sotto il target, e nessun altro */
    unsigned exp = 0, fbad = 0;
    for (unsigned t = 0; t < batch; t++) {
        uint32_t h7 = g->hashes[(size_t)t * 8 + 7], h6 = g->hashes[(size_t)t * 8 + 6];
        uint64_t top = ((uint64_t)__builtin_bswap32(h7) << 32) | __builtin_bswap32(h6);
        if (top <= target64) {
            exp++;
            int ok = 0;
            for (unsigned k = 0; k < g->found[0] && k < 255; k++) if (g->found[1 + k] == base + t) ok = 1;
            if (!ok && exp <= 255) fbad++;
        }
    }
    if (g->found[0] != exp) fbad++;
    say("controllo: %u hash in un lancio da %.0f ms, %u confrontati con la CPU: %s; nonce sotto il target %u, attesi %u: %s\n",
        batch, dt * 1e3, n, bad ? "DIVERSI" : "tutti uguali", g->found[0], exp, fbad ? "SBAGLIATI" : "giusti");
    return (bad || fbad) ? 1 : 0;
}

/* velocita' con `batch` hash in volo per `seconds` secondi */
static double measure(gpu_t *g, unsigned batch, double seconds)
{
    uint8_t header[80];
    for (int i = 0; i < 80; i++) header[i] = (uint8_t)(i * 13 + 1);
    uint32_t base = 0;
    double t0 = now_s(), t1 = t0;
    unsigned long long done = 0;
    do {
        make_job(g, header, base, 0, 0);
        if (launch(g, batch)) return -1;
        base += batch; done += batch;
        t1 = now_s();
    } while (t1 - t0 < seconds);
    return (double)done / (t1 - t0);
}

int main(int argc, char **argv)
{
    int dev_i = 0, cpu = 0;
    unsigned lines = 4096, one_batch = 0, one_block = 0, check = 96;
    double seconds = 6;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--gpu")) { dev_i = atoi(v); i++; }
        else if (!strcmp(a, "--seconds")) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--batch")) { one_batch = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--block")) { one_block = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--check")) { check = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--lines")) { lines = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--cpu-backend")) cpu = 1;
        else { printf("bound-gpubench " VERSION ": opzioni --gpu N --seconds S --batch N --block N --check N --lines N --cpu-backend\n"); return 2; }
    }
    logf = fopen("bound-gpubench.log", "w");
    say("bound-gpubench " VERSION " - kernel CUDA BoundHash (BOUND)\n");
    int st = bp_selftest();
    say("selftest BoundHash sulla CPU (vettori ufficiali): %s\n", st ? "FALLITO" : "OK");
    if (st || bp_set_lines(lines)) return 1;

    gpu_t g; memset(&g, 0, sizeof g);
    g.cpu = cpu; g.lines = lines;
    size_t pad_bytes_per_hash = (size_t)lines * 128, freeb = 0, totalb = 0;
    void *ctx = NULL;
    if (!cpu) {
        if (cuda_load()) return 1;
        int ver = 0, count = 0, dev = 0, maj = 0, min = 0, sms = 0;
        char name[128] = "";
        CK(cuInit(0)); cuDriverGetVersion(&ver); CK(cuDeviceGetCount(&count));
        say("driver CUDA %d.%d, %d sched%s\n", ver / 1000, (ver % 1000) / 10, count, count == 1 ? "a" : "e");
        for (int i = 0; i < count; i++) {
            size_t tm = 0; int d;
            cuDeviceGet(&d, i); cuDeviceGetName(name, sizeof name, d); cuDeviceTotalMem(&tm, d);
            say("  GPU %d: %s, %.0f MiB\n", i, name, tm / 1048576.0);
        }
        if (dev_i >= count) { say("la scheda %d non c'e'\n", dev_i); return 1; }
        CK(cuDeviceGet(&dev, dev_i));
        cuDeviceGetAttribute(&maj, 75, dev); cuDeviceGetAttribute(&min, 76, dev); cuDeviceGetAttribute(&sms, 16, dev);
        cuDeviceGetName(name, sizeof name, dev);
        CK(cuCtxCreate(&ctx, 4 /* blocking sync */, dev));
        CK(cuMemGetInfo(&freeb, &totalb));
        say("uso la GPU %d: %s, compute %d.%d, %d SM, memoria libera %.0f di %.0f MiB\n", dev_i, name, maj, min, sms, freeb / 1048576.0, totalb / 1048576.0);
        void *mod = NULL; static char jitlog[8192];
        int opts[2] = { 5 /* error log buffer */, 6 /* its size */ };
        void *vals[2] = { jitlog, (void *)(size_t)sizeof jitlog };
        int e = cuModuleLoadDataEx(&mod, kernel_ptx, 2, opts, vals);
        if (e) { say("il driver non carica il kernel (errore %d): %s\nServe il driver NVIDIA 550 o piu' recente e una scheda RTX 20 o piu' nuova.\n", e, jitlog); return 1; }
        CK(cuModuleGetFunction(&g.fn, mod, "bound_kernel"));
    } else {
        freeb = (size_t)256 << 20;
        say("backend CPU: il kernel gira sulla CPU, serve solo a provare il programma\n");
    }

    /* memoria: tutta quella libera meno 1 GiB, al piu' 40960 hash in volo */
    size_t keep = cpu ? 0 : (size_t)1 << 30;
    size_t maxb = freeb > keep ? (freeb - keep) / (pad_bytes_per_hash + 32) : 0;
    if (maxb > 40960) maxb = 40960;
    maxb &= ~(size_t)255;
    if (one_batch) { if (one_batch > maxb) { say("--batch %u: la memoria basta per %u\n", one_batch, (unsigned)maxb); return 1; } maxb = (one_batch + 255) & ~255u; }
    if (maxb < 256) { say("memoria insufficiente (%.0f MiB liberi)\n", freeb / 1048576.0); return 1; }
    g.batch_alloc = maxb;
    g.hashes = malloc(maxb * 32);
    if (cpu) g.h_pad = malloc(maxb * pad_bytes_per_hash);
    else {
        CK(cuMemAlloc(&g.d_pad, maxb * pad_bytes_per_hash)); CK(cuMemAlloc(&g.d_job, sizeof g.job));
        CK(cuMemAlloc(&g.d_found, sizeof g.found)); CK(cuMemAlloc(&g.d_hash, maxb * 32));
    }
    say("scratchpad: %u righe = %.0f KiB per hash, fino a %u hash in volo (%.1f GiB)\n", lines, pad_bytes_per_hash / 1024.0, (unsigned)maxb,
        maxb * pad_bytes_per_hash / 1073741824.0);

    /* 1. controllo: prima un lancio piccolo (se il kernel fosse lento, un lancio grande farebbe scattare il
     *    timeout del driver video), poi uno pieno */
    g.block = one_block ? one_block : 64;
    if (one_block && maxb >= one_block) maxb = maxb / one_block * one_block;   /* anche il lancio pieno multiplo del blocco */
    unsigned small = maxb < 1024 ? (unsigned)maxb : 1024;
    small = small / g.block * g.block;              /* multiplo del blocco: con 96 o 192 thread 1024 non lo e' (0.2: falso "hash diversi") */
    if (!small) small = g.block;
    int bad = check_launch(&g, small, check < small ? check : small);
    if (bad < 0) return 1;
    double quick = measure(&g, small, 1.0);
    if (quick < 0) return 1;
    say("misura rapida con %u hash in volo: %.0f H/s\n", small, quick);
    if (!bad && maxb > small && quick * 1.5 > maxb / 1.5) {       /* lancio pieno sotto ~1,5 s */
        int b2 = check_launch(&g, (unsigned)maxb, check);
        if (b2 < 0) return 1;
        bad |= b2;
    }
    if (bad) { say("RISULTATO: il kernel sulla GPU NON da' gli stessi hash della CPU. Mandami questo log.\n"); return 1; }

    /* 2. tabella delle velocita' */
    unsigned blocks[2] = { 64, 128 }, nblocks = 2, batches[8], nb = 0;
    if (one_block) { blocks[0] = one_block; nblocks = 1; }
    if (one_batch) batches[nb++] = (unsigned)maxb;
    else {
        for (unsigned b = 2048; b < maxb && nb < 7; b *= 2) batches[nb++] = b;
        batches[nb++] = (unsigned)maxb;
    }
    say("\nhash in volo  thread/blocco      H/s    GB/s di VRAM (2,1 MB per hash)\n");
    double best = 0; unsigned bb = 0, bk = 0;
    for (unsigned i = 0; i < nb; i++)
        for (unsigned k = 0; k < nblocks; k++) {
            if (batches[i] / (quick > 1 ? quick : 1) > 1.5 && !cpu) { say("%12u  %13u  saltato: un lancio durerebbe piu' di 1,5 s\n", batches[i], blocks[k]); continue; }
            g.block = blocks[k];
            double r = measure(&g, batches[i] / g.block * g.block, seconds);
            if (r < 0) return 1;
            say("%12u  %13u  %7.0f  %6.0f\n", batches[i], blocks[k], r, r * 16384.0 * lines / 4096 * 128 / 1e9);
            if (r > best) { best = r; bb = batches[i]; bk = blocks[k]; }
            if (r > quick) quick = r;
        }
    say("\nRISULTATO: %.0f H/s (%.1f kH/s) con %u hash in volo e %u thread per blocco\n", best, best / 1e3, bb, bk);
    say("log in bound-gpubench.log\n");
    if (ctx) cuCtxDestroy(ctx);
    return 0;
}
