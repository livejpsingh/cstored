#ifndef KVS_HASH_TABLE_H
#define KVS_HASH_TABLE_H

#include "kvs/common.h"
#include "kvs/slab.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of opaque hash table */
typedef struct kvs_hash_table kvs_hash_table_t;

/* Diagnostic metrics for table inspection and tuning */
typedef struct {
    size_t count;               /* Number of active key-value pairs */
    size_t capacity;            /* Current number of allocated buckets */
    size_t occupied_buckets;    /* Buckets containing at least one node */
    size_t max_chain_length;    /* Maximum collision chain depth observed */
    double load_factor;         /* (double)count / (double)capacity */
} kvs_ht_stats_t;

/**
 * Creates a new hash table.
 *
 * @param initial_capacity  Suggested bucket count (rounded up to power of two, min 16).
 * @param node_pool         Optional slab pool for node allocations (pass NULL to auto-manage).
 * @param out_ht            Pointer to receive created table handle.
 * @return KVS_OK on success, or an explicit error status.
 */
KVS_NODISCARD
kvs_status_t kvs_ht_create(
    size_t initial_capacity,
    kvs_slab_pool_t *node_pool,
    kvs_hash_table_t **out_ht
);

/**
 * Inserts or updates a key-value pair.
 * Copies both key and value into managed memory.
 * If key already exists, updates value with strong exception safety.
 *
 * @param ht       Valid hash table handle.
 * @param key      Binary or string key bytes.
 * @param key_len  Key byte length (1 .. KVS_MAX_KEY_LEN).
 * @param val      Pointer to value payload (may be NULL if val_len == 0).
 * @param val_len  Value byte length.
 * @return KVS_OK on success, or explicit error.
 */
KVS_NODISCARD
kvs_status_t kvs_ht_put(
    kvs_hash_table_t *ht,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
);

/**
 * Retrieves the value associated with a key.
 *
 * @param ht          Valid hash table handle.
 * @param key         Key byte pointer.
 * @param key_len     Key byte length.
 * @param out_val     Pointer to receive pointer to internal value buffer (read-only reference).
 * @param out_val_len Pointer to receive value byte length.
 * @return KVS_OK on success, KVS_ERR_NOT_FOUND if absent, or KVS_ERR_INVALID_ARG.
 */
KVS_NODISCARD
kvs_status_t kvs_ht_get(
    const kvs_hash_table_t *ht,
    const char *key,
    size_t key_len,
    const void **out_val,
    size_t *out_val_len
);

/**
 * Deletes a key-value entry from the table.
 * Frees key, value, and node back to respective allocators.
 *
 * @param ht      Valid hash table handle.
 * @param key     Key byte pointer.
 * @param key_len Key byte length.
 * @return KVS_OK if deleted, KVS_ERR_NOT_FOUND if absent.
 */
KVS_NODISCARD
kvs_status_t kvs_ht_delete(
    kvs_hash_table_t *ht,
    const char *key,
    size_t key_len
);

/**
 * Checks whether a key is present in the table.
 */
bool kvs_ht_contains(
    const kvs_hash_table_t *ht,
    const char *key,
    size_t key_len
);

/**
 * Returns current count of stored items.
 */
size_t kvs_ht_size(const kvs_hash_table_t *ht);

/**
 * Gathers detailed internal statistics for observability and profiling.
 */
kvs_status_t kvs_ht_get_stats(
    const kvs_hash_table_t *ht,
    kvs_ht_stats_t *out_stats
);

/**
 * Destroys hash table, deallocating all entries, buckets, and internal pools.
 *
 * @param ht Table handle (safe to pass NULL).
 */
void kvs_ht_destroy(kvs_hash_table_t *ht);

#ifdef __cplusplus
}
#endif

#endif /* KVS_HASH_TABLE_H */
