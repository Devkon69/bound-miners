/* Prova del backend OpenCL: compila il kernel sulla prima GPU OpenCL (BOUND_OPENCL_CPU=1 per POCL) e confronta gli
 * hash con bound_pow.c.   gcc -O2 -Isrc tools/test_ocl.c src/bound_pow.c -ldl -o /tmp/test_ocl && /tmp/test_ocl [RIGHE] [N] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "bound_pow.h"
#include "gpu_common.h"
void bound_cpu_launch(unsigned long long *pad, const unsigned *job, unsigned *found, unsigned *out_hash, unsigned nthreads) { (void)pad; (void)job; (void)found; (void)out_hash; (void)nthreads; }
int main(int c, char **v)
{
    unsigned lines = c > 1 ? (unsigned)atoi(v[1]) : 64, n = c > 2 ? (unsigned)atoi(v[2]) : 256;
    ocl_info_t o[8]; int no = ocl_list(o, 8, 0);
    if (bp_selftest() || bp_set_lines(lines)) return 1;
    printf("dispositivi OpenCL: %d\n", no);
    if (!no) return 1;
    gpu_t g;
    if (gpu_open_ocl(&g, &o[0], lines, n, 1u << 20)) return 1;
    uint8_t h[80]; for (int i = 0; i < 80; i++) h[i] = (uint8_t)(i * 29 + 7);
    make_job(&g, h, 0xFFFFFF00u, 0x0FFFFFFFFFFFFFFFull, 1);
    double t0 = now_s();
    if (launch(&g, n)) return 1;
    double dt = now_s() - t0;
    unsigned bad = 0, below = 0;
    for (unsigned t = 0; t < n; t++) {
        uint8_t ref[32], got[32]; bp_hash_nonce(h, 0xFFFFFF00u + t, ref);
        for (int i = 0; i < 8; i++) { uint32_t w = g.hashes[(size_t)t * 8 + i]; got[4*i] = w >> 24; got[4*i+1] = w >> 16; got[4*i+2] = w >> 8; got[4*i+3] = (uint8_t)w; }
        bad += memcmp(ref, got, 32) != 0;
        below += bp_top64(ref) <= 0x0FFFFFFFFFFFFFFFull;
    }
    printf("%u hash (%u righe) in %.2f s: %u diversi dal riferimento; sotto il target %u, trovati dal kernel %u\n", n, lines, dt, bad, below, g.found[0]);
    return bad || below != g.found[0];
}
