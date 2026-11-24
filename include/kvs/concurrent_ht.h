#ifndef KVS_CONCURRENT_HT_H
#define KVS_CONCURRENT_HT_H

#include "kvs/common.h"
#include "kvs/slab.h"
#include <stdatomic.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of thread-safe concurrent table handle */
typedef struct kvs_concurrent_ht kvs_concurrent_ht_t;

/* Default number of stripes (must be power of two, e.g. 64) */
#define KVS_DEFAULT_STRIPES (64U)

/**
 * Creates a concurrent hash table with fine-grained lock striping.
 *
 * @param initial_capacity Bucket count (minimum 64, rounded to power of two).
 * @param num_stripes      Number of striped RW locks (0 for default 64).
 * @param out_ht           Pointer to receive created table handle.
 * @return KVS_OK on success, or an explicit error status.
 */
KVS_NODISCARD
kvs_status_t kvs_concurrent_ht_create(
    size_t initial_capacity,
    size_t num_stripes,
    kvs_concurrent_ht_t **out_ht
);

/**
 * Inserts or updates a key-value pair under fine-grained stripe write lock.
 *
 * @param ht      Valid concurrent table handle.
 * @param key     Key byte buffer.
 * @param key_len Key byte length (1 .. KVS_MAX_KEY_LEN).
 * @param val     Value payload bytes.
 * @param val_len Value length in bytes.
 * @return KVS_OK on success, or explicit error code.
 */
KVS_NODISCARD
kvs_status_t kvs_concurrent_ht_put(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
);

/**
 * Thread-safe value retrieval. Deep-copies the value into caller-owned memory
 * to prevent race conditions where another thread overwrites or deletes the key.
 *
 * @param ht          Valid concurrent table handle.
 * @param key         Key byte buffer.
 * @param key_len     Key byte length.
 * @param out_val     Pointer to receive heap-allocated copy of value (caller must free()).
 * @param out_val_len Pointer to receive length in bytes of value.
 * @return KVS_OK on success, KVS_ERR_NOT_FOUND if key does not exist.
 */
KVS_NODISCARD
kvs_status_t kvs_concurrent_ht_get(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len,
    void **out_val,
    size_t *out_val_len
);

/**
 * Removes a key-value entry under fine-grained stripe write lock.
 *
 * @param ht      Valid concurrent table handle.
 * @param key     Key byte buffer.
 * @param key_len Key byte length.
 * @return KVS_OK if deleted, KVS_ERR_NOT_FOUND if absent.
 */
KVS_NODISCARD
kvs_status_t kvs_concurrent_ht_delete(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len
);

/**
 * Checks key existence under fine-grained stripe read lock.
 */
bool kvs_concurrent_ht_contains(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len
);

/**
 * Returns current total item count atomically.
 */
size_t kvs_concurrent_ht_size(const kvs_concurrent_ht_t *ht);

/**
 * Callback signature for iterating over live key-value pairs during log compaction.
 */
typedef void (*kvs_entry_cb_t)(
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len,
    void *user_data
);

/**
 * Traverses all live entries in the concurrent table under shared read locks.
 * Used for AOF log compaction and snapshotting.
 */
kvs_status_t kvs_concurrent_ht_for_each(
    kvs_concurrent_ht_t *ht,
    kvs_entry_cb_t cb,
    void *user_data
);

/**
 * Destroys concurrent table, releases all entries and synchronization primitives.
 * Must only be called after all worker threads have completed operations.
 *
 * @param ht Table handle (safe to pass NULL).
 */
void kvs_concurrent_ht_destroy(kvs_concurrent_ht_t *ht);

#ifdef __cplusplus
}
#endif

#endif /* KVS_CONCURRENT_HT_H */
