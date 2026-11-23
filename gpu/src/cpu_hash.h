/* BoundHash sulla CPU, thread-safe (un cpuh_t per thread). Vedi cpu_hash.c. */
#ifndef CPU_HASH_H
#define CPU_HASH_H
#include <stdint.h>
#define CPUH_MAXLANES 4
typedef struct { uint64_t *pad[CPUH_MAXLANES]; unsigned lines; int lanes; uint32_t mid[8]; uint8_t tail[12]; } cpuh_t;
const char *cpuh_engine(void);                          /* "AVX-512", "AVX2" o "scalare"; chiamarla una volta prima dei thread */
int  cpuh_init(cpuh_t *h, unsigned lines, int lanes);   /* lanes = hash calcolati insieme (1..4); 0 = ok */
void cpuh_free(cpuh_t *h);
void cpuh_job(cpuh_t *h, const uint8_t header[80]);     /* contano i primi 76 byte; il nonce lo mette cpuh_hashn */
void cpuh_hashn(cpuh_t *h, const uint32_t *nonce, uint8_t (*out)[32]);   /* h->lanes hash insieme */
void cpuh_hash(cpuh_t *h, uint32_t nonce, uint8_t out[32]);              /* uno solo, scalare */
int  cpuh_selftest(unsigned lines);                     /* 0 = tutte le versioni uguali a bound_pow.c */
#endif
