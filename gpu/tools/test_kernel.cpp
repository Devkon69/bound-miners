// il kernel compilato per la CPU contro bound_pow.c: ogni hash di ogni thread, 64 e 4096 righe
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "../src/bound_pow.h"
void bound_cpu_launch(unsigned long long *pad, const unsigned *job, unsigned *found, unsigned *out_hash, unsigned nthreads);
void bp_tag_mid(const char *tag, unsigned st[8]);
}
static int run(unsigned lines, unsigned n, unsigned base)
{
    unsigned char hdr[80]; for (int i = 0; i < 80; i++) hdr[i] = (unsigned char)(i * 37 + lines + base);
    unsigned char st[BP_STATE_BYTES]; bp_set_lines(lines); bp_job_state(hdr, st);
    unsigned job[24];
    for (int i = 0; i < 11; i++) job[i] = (unsigned)st[4*i] << 24 | (unsigned)st[4*i+1] << 16 | (unsigned)st[4*i+2] << 8 | st[4*i+3];
    job[11] = base; job[12] = 0x0fffffffu; job[13] = 0x12345678u; job[14] = lines; job[15] = 1;
    bp_tag_mid("BOUND/BoundHash-result", job + 16);
    std::vector<unsigned long long> pad((size_t)n * lines * 16);
    std::vector<unsigned> out((size_t)n * 8), found(256, 0);
    bound_cpu_launch(pad.data(), job, found.data(), out.data(), n);
    int bad = 0; unsigned nf = 0;
    for (unsigned t = 0; t < n; t++) {
        unsigned char ref[32], got[32];
        bp_hash_nonce(hdr, base + t, ref);
        for (int i = 0; i < 8; i++) { unsigned w = out[(size_t)t * 8 + i]; got[4*i] = w >> 24; got[4*i+1] = w >> 16; got[4*i+2] = w >> 8; got[4*i+3] = w; }
        if (memcmp(ref, got, 32)) bad++;
        unsigned long long top = bp_top64(ref);
        if (top <= (((unsigned long long)job[12] << 32) | job[13])) { if (nf >= found[0] || found[1 + nf] != base + t) bad++; nf++; }
    }
    if (nf != found[0]) bad++;
    printf("righe %u, %u thread, base %08x: %u trovati, %s\n", lines, n, base, found[0], bad ? "DIVERSO" : "uguale a bound_pow.c");
    return bad;
}
int main() { int b = run(64, 300, 0) + run(64, 100, 0xfffffff0u) + run(4096, 24, 77) + run(2, 16, 5); printf(b ? "FAIL\n" : "PASS\n"); return b != 0; }
