#ifndef KVS_SLAB_H
#define KVS_SLAB_H

#include "kvs/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of opaque slab pool handle */
typedef struct kvs_slab_pool kvs_slab_pool_t;

/* Diagnostic statistics structure */
typedef struct {
    size_t chunk_size;          /* Configured user payload size per chunk */
    size_t chunks_per_slab;     /* Number of chunks allocated per slab page */
    size_t total_slabs;         /* Number of active mapped slabs */
    size_t total_chunks;        /* Total capacity across all slabs */
    size_t active_allocations;  /* Currently allocated chunks */
    size_t total_bytes_mapped;  /* Total memory mapped via OS pages */
} kvs_slab_stats_t;

/**
 * Creates a new slab allocation pool.
 *
 * @param chunk_size       Size in bytes of each chunk payload (will be aligned to 16B).
 * @param chunks_per_slab  Number of chunks per mapped slab page (minimum 1).
 * @param max_slabs        Maximum allowed slabs (0 for unlimited).
 * @param out_pool         Pointer to receive the created pool handle.
 * @return KVS_OK on success, or an explicit error status code.
 */
KVS_NODISCARD
kvs_status_t kvs_slab_pool_create(
    size_t chunk_size,
    size_t chunks_per_slab,
    size_t max_slabs,
    kvs_slab_pool_t **out_pool
);

/**
 * Allocates a single chunk from the slab pool in O(1) time.
 *
 * @param pool Valid pool handle.
 * @return Pointer to 16-byte aligned memory payload, or NULL if exhausted/error.
 */
KVS_NODISCARD
void *kvs_slab_alloc(kvs_slab_pool_t *pool);

/**
 * Returns an allocated chunk to the slab pool in O(1) time.
 * Detects double-frees, foreign pointers, and corrupted chunk headers.
 *
 * @param pool Valid pool handle.
 * @param ptr  Pointer previously returned by kvs_slab_alloc.
 * @return KVS_OK on success, KVS_ERR_DOUBLE_FREE if freed twice,
 *         or KVS_ERR_INVALID_ARG if ptr is NULL or does not belong to pool.
 */
KVS_NODISCARD
kvs_status_t kvs_slab_free(kvs_slab_pool_t *pool, void *ptr);

/**
 * Queries current allocation statistics and memory accounting.
 *
 * @param pool      Valid pool handle.
 * @param out_stats Pointer to receive statistics snapshot.
 * @return KVS_OK on success, or KVS_ERR_INVALID_ARG if parameters are NULL.
 */
kvs_status_t kvs_slab_pool_stats(const kvs_slab_pool_t *pool, kvs_slab_stats_t *out_stats);

/**
 * Destroys the slab pool and unmaps all underlying OS pages.
 * Reclaims all memory deterministically with zero leaks.
 *
 * @param pool Pool handle to destroy (safe to pass NULL).
 */
void kvs_slab_pool_destroy(kvs_slab_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* KVS_SLAB_H */
