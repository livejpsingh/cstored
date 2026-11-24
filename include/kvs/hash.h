#ifndef KVS_HASH_H
#define KVS_HASH_H

#include "kvs/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Computes a 64-bit high-avalanche hash of arbitrary byte sequences.
 * Uses MurmurHash3 (64-bit optimized mix).
 *
 * Invariant: Reads data safely byte-by-byte without unaligned pointer dereferences,
 * preventing SIGBUS / hardware traps on strict-alignment architectures.
 *
 * @param key   Pointer to byte buffer. Must not be NULL if len > 0.
 * @param len   Number of bytes in key.
 * @param seed  Initialization seed (typically randomized per table instance).
 * @return 64-bit unsigned hash value.
 */
uint64_t kvs_hash64(const void *key, size_t len, uint64_t seed);

/**
 * Retrieves a cryptographically secure random 64-bit seed from the OS.
 * On Linux, reads from /dev/urandom (or getrandom syscall).
 * Falls back to high-resolution timestamp if entropy source is temporarily unavailable.
 *
 * @param out_seed Pointer to receive the generated seed.
 * @return KVS_OK on success, or explicit error.
 */
kvs_status_t kvs_hash_generate_seed(uint64_t *out_seed);

#ifdef __cplusplus
}
#endif

#endif /* KVS_HASH_H */
