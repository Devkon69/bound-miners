/* bound_pow.h - BOUND proof of work (BoundHash) for the host program.
 *
 *   seed  = SHA256(T || T || header_80), T = SHA256("BOUND/BoundHash")
 *   S     = 4 x SHA256(seed || k)                      16 words of 64 bit
 *   fill  M[i] = F(M[i-1] ^ M[ref], i)                 4096 lines of 128 bytes (512 KiB)
 *   mix   4096 steps: v ^= M[j]; 64x64 multiply; round; M[j] += v
 *   hash  = SHA256(T2 || T2 || seed || v), T2 = SHA256("BOUND/BoundHash-result")
 *   valid if Uint256_LE(hash) <= target               (hash[31] = most significant byte)
 *
 * Header 80 bytes, Bitcoin layout: version u32 | prev[32] | merkle[32] | time u32 | bits u32 | nonce u32.
 * FPGA job (200 bytes, the rest zero): SHA-256 state after T||T and the first 64 header bytes
 * (8 big-endian words), header bytes 64..75, nonce base (4 bytes LE, zero).
 * Merkle tree: leaf = TaggedHash("BOUND/wtxid", tx), node = TaggedHash("BOUND/merkle", left || right).
 */
#ifndef BOUND_POW_H
#define BOUND_POW_H
#include <stdint.h>
#include <stddef.h>

#define BP_STATE_BYTES 200
#define BP_CLOCKS_PER_HASH 90660.0     /* bitstream v1.0 with BM2 0, RDLAT 1, MULLAT 2 */

/* scratchpad lines: 4096 on mainnet and testnet (default), 64 on regtest. Power of two, 2..2^20. */
int  bp_set_lines(unsigned lines);
void bp_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
void bp_tagged_hash(const char *tag, const uint8_t *msg, size_t len, uint8_t out[32]);
/* BoundHash of a header (nonce already in bytes 76..79) */
void bp_boundhash(const uint8_t header[80], uint8_t out[32]);
/* BoundHash of the header with this nonce (bytes 76..79 replaced, little-endian) */
void bp_hash_nonce(const uint8_t header[80], uint32_t nonce, uint8_t out[32]);
/* FPGA job state (nonce field ignored) */
void bp_job_state(const uint8_t header[80], uint8_t state[BP_STATE_BYTES]);
/* what the FPGA computes from the job state for a nonce (same result as bp_hash_nonce) */
void bp_fpga_hash(const uint8_t state[BP_STATE_BYTES], uint32_t nonce, uint8_t out[32]);
/* merkle root from the coinbase bytes and the branch that joins it to the root */
void bp_merkle(const uint8_t *cb, size_t cbn, const uint8_t (*branch)[32], int nbranch, uint8_t out[32]);
/* top 64 bits of the little-endian 256-bit number (bytes 24..31) */
uint64_t bp_top64(const uint8_t h[32]);
/* a <= b as little-endian 256-bit numbers */
int bp_le256_le(const uint8_t a[32], const uint8_t b[32]);
/* share target (little-endian) = pdiff1 / d, pdiff1 = 0xFFFF << 208; d may be far below 1 */
void bp_target_from_diff(double d, uint8_t t[32]);
/* share difficulty of a hash: pdiff1 / hash from its top 64 bits */
double bp_diff_of(const uint8_t h[32]);
/* the official vectors of tests/test_boundhash.cpp (64 and 4096 lines), 0 = OK. Restores the lines. */
int bp_selftest(void);

#endif
