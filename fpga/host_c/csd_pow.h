/* csd_pow.h - Compute Substrate (CSD) proof of work for the host program.
 *
 * Header "csd1", 84 bytes:
 *   version u32 LE | prev[32] | merkle[32] | time u64 LE | bits u32 LE | nonce u32 LE
 * PoW: sha256d(header), compared with the target as a big-endian number
 * (byte 0 most significant, no reversal as in Bitcoin).
 * Byte-for-byte the same construction as cairn-miner (src/coinbase.rs,
 * src/stratum/mapping.rs). */
#ifndef CSD_POW_H
#define CSD_POW_H
#include <stdint.h>
#include <stddef.h>

void cp_sha256(const uint8_t *d, size_t n, uint8_t out[32]);
void cp_sha256d(const uint8_t *d, size_t n, uint8_t out[32]);
void cp_compress(uint32_t st[8], const uint8_t block[64]);

/* header from stratum fields: prev already in header byte order */
void cp_header(uint8_t h[84], uint32_t version, const uint8_t prev[32], const uint8_t merkle[32],
               uint64_t time, uint32_t bits, uint32_t nonce);

/* merkle root: coinbase = coinb1 || xn1 || xn2 || coinb2, leaf index 0 */
void cp_merkle(const uint8_t *cb, size_t cbn, const uint8_t (*branch)[32], int nbranch, uint8_t out[32]);

/* FPGA job words: midstate of bytes 0..63, tail W0..W3 of bytes 64..79 */
void cp_fpga_job(const uint8_t h[84], uint32_t mid[8], uint32_t tail[4]);

/* hash of the header with this nonce (header bytes 80..83 replaced) */
void cp_hash_nonce(const uint8_t h[84], uint32_t nonce, uint8_t out[32]);

/* a <= b as big-endian 256-bit numbers */
int cp_be256_le(const uint8_t a[32], const uint8_t b[32]);

/* share target = pdiff1 / round(d), pdiff1 = 0x00000000FFFF0000..., big-endian */
void cp_target_from_diff(double d, uint8_t t[32]);

/* run the built-in known-answer tests, 0 = OK */
int cp_selftest(void);

#endif
