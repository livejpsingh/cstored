#include "kvs/hash_table.h"
#include "kvs/hash.h"
#include <stdlib.h>
#include <string.h>

#define KVS_HT_MAGIC               (0x4B56535441424C45ULL) /* "KVSTABLE" */
#define KVS_HT_MIN_CAPACITY        (16U)
#define KVS_HT_MAX_CAPACITY        (1ULL << 31) /* Limit to prevent 32-bit overflow */
#define KVS_HT_LOAD_FACTOR_NUM     (3U)
#define KVS_HT_LOAD_FACTOR_DENOM   (4U) /* 75% load factor threshold */

/* Singly linked bucket node */
typedef struct kvs_node {
    uint64_t hash;              /* Cached 64-bit hash for zero-cost rehashing */
    char *key;                  /* Owned deep copy of key */
    size_t key_len;             /* Explicit byte length of key */
    void *val;                  /* Owned deep copy of value payload */
    size_t val_len;             /* Value byte count */
    struct kvs_node *next;      /* Collision chain pointer */
} kvs_node_t;

/* Core Hash Table */
struct kvs_hash_table {
    uint64_t magic;
    uint64_t seed;              /* Per-table hash seed to mitigate HashDoS */
    size_t count;               /* Number of active elements */
    size_t capacity;            /* Power-of-two bucket capacity */
    kvs_node_t **buckets;       /* Array of bucket head pointers */
    kvs_slab_pool_t *node_pool; /* Slab pool for O(1) node allocations */
    bool owns_node_pool;        /* True if pool was created internally */
};

/*
 * Rounds up to next power of two.
 * Enforces minimum capacity.
 */
static size_t kvs_ht_next_pow2(size_t n) {
    if (n < KVS_HT_MIN_CAPACITY) {
        return KVS_HT_MIN_CAPACITY;
    }
    if (n >= KVS_HT_MAX_CAPACITY) {
        return KVS_HT_MAX_CAPACITY;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFFU
    n |= n >> 32;
#endif
    n++;
    return n;
}

/*
 * Allocates a node from slab pool or standard heap.
 */
static kvs_node_t *kvs_ht_alloc_node(kvs_hash_table_t *ht) {
    if (ht->node_pool != NULL) {
        return (kvs_node_t *)kvs_slab_alloc(ht->node_pool);
    }
    return (kvs_node_t *)malloc(sizeof(kvs_node_t));
}

/*
 * Frees a node to slab pool or standard heap.
 */
static void kvs_ht_free_node(kvs_hash_table_t *ht, kvs_node_t *node) {
    if (node == NULL) {
        return;
    }
    if (ht->node_pool != NULL) {
        if (kvs_slab_free(ht->node_pool, node) != KVS_OK) {
            /* Ignore status during node deallocation */
        }
    } else {
        free(node);
    }
}

/*
 * Dynamically resizes the table by doubling bucket capacity.
 * Invariant: Never recomputes hashes or reallocates nodes; simply re-links buckets.
 * If memory allocation for the new bucket array fails, the table remains untouched and valid.
 */
static kvs_status_t kvs_ht_resize(kvs_hash_table_t *ht) {
    if (ht->capacity >= KVS_HT_MAX_CAPACITY) {
        return KVS_OK; /* Cannot expand further; tolerate longer chains */
    }

    size_t new_capacity = 0;
    if (kvs_mul_overflow_size(ht->capacity, 2U, &new_capacity)) {
        return KVS_ERR_OVERFLOW;
    }

    size_t alloc_bytes = 0;
    if (kvs_mul_overflow_size(new_capacity, sizeof(kvs_node_t *), &alloc_bytes)) {
        return KVS_ERR_OVERFLOW;
    }

    /* Pitfall avoided: calloc guarantees all bucket heads start cleanly as NULL */
    kvs_node_t **new_buckets = (kvs_node_t **)calloc(new_capacity, sizeof(kvs_node_t *));
    if (new_buckets == NULL) {
        return KVS_ERR_NOMEM; /* Graceful degradation */
    }

    const size_t new_mask = new_capacity - 1;

    /* Re-link existing nodes into new bucket array */
    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_node_t *curr = ht->buckets[i];
        while (curr != NULL) {
            kvs_node_t *next = curr->next;
            const size_t new_idx = (size_t)(curr->hash & new_mask);

            curr->next = new_buckets[new_idx];
            new_buckets[new_idx] = curr;

            curr = next;
        }
    }

    free(ht->buckets);
    ht->buckets = new_buckets;
    ht->capacity = new_capacity;

    return KVS_OK;
}

kvs_status_t kvs_ht_create(
    size_t initial_capacity,
    kvs_slab_pool_t *node_pool,
    kvs_hash_table_t **out_ht
) {
    if (out_ht == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    kvs_hash_table_t *ht = (kvs_hash_table_t *)malloc(sizeof(kvs_hash_table_t));
    if (ht == NULL) {
        return KVS_ERR_NOMEM;
    }
    memset(ht, 0, sizeof(*ht));

    ht->capacity = kvs_ht_next_pow2(initial_capacity);

    size_t alloc_bytes = 0;
    if (kvs_mul_overflow_size(ht->capacity, sizeof(kvs_node_t *), &alloc_bytes)) {
        free(ht);
        return KVS_ERR_OVERFLOW;
    }

    ht->buckets = (kvs_node_t **)calloc(ht->capacity, sizeof(kvs_node_t *));
    if (ht->buckets == NULL) {
        free(ht);
        return KVS_ERR_NOMEM;
    }

    /* If no pool is provided, create a dedicated internal slab pool for nodes */
    if (node_pool == NULL) {
        kvs_status_t status = kvs_slab_pool_create(sizeof(kvs_node_t), 128, 0, &ht->node_pool);
        if (status != KVS_OK) {
            free(ht->buckets);
            free(ht);
            return status;
        }
        ht->owns_node_pool = true;
    } else {
        ht->node_pool = node_pool;
        ht->owns_node_pool = false;
    }

    /* Generate randomized seed for HashDoS collision protection */
    kvs_hash_generate_seed(&ht->seed);
    ht->count = 0;
    ht->magic = KVS_HT_MAGIC;

    *out_ht = ht;
    return KVS_OK;
}

kvs_status_t kvs_ht_put(
    kvs_hash_table_t *ht,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC || key == NULL || key_len == 0) {
        return KVS_ERR_INVALID_ARG;
    }

    if (key_len > KVS_MAX_KEY_LEN) {
        return KVS_ERR_KEY_TOO_LONG;
    }

    if (val_len > 0 && val == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t mask = ht->capacity - 1;
    const size_t bucket_idx = (size_t)(hash & mask);

    /* Search bucket for existing key */
    kvs_node_t *curr = ht->buckets[bucket_idx];
    while (curr != NULL) {
        if (curr->hash == hash && curr->key_len == key_len &&
            memcmp(curr->key, key, key_len) == 0) {

            /*
             * Update existing key:
             * Pitfall avoided: Allocate new value first before freeing old value.
             * If allocation fails, table remains intact and no data is corrupted.
             */
            void *new_val = NULL;
            if (val_len > 0) {
                new_val = malloc(val_len);
                if (new_val == NULL) {
                    return KVS_ERR_NOMEM;
                }
                memcpy(new_val, val, val_len);
            }

            free(curr->val);
            curr->val = new_val;
            curr->val_len = val_len;
            return KVS_OK;
        }
        curr = curr->next;
    }

    /* Check load factor threshold: count / capacity >= 3/4 */
    if ((ht->count + 1) * KVS_HT_LOAD_FACTOR_DENOM >= ht->capacity * KVS_HT_LOAD_FACTOR_NUM) {
        if (kvs_ht_resize(ht) != KVS_OK) {
            /* Degradation under memory pressure: continue with current capacity */
        }
    }

    /* Recalculate index post-potential-resize */
    const size_t final_idx = (size_t)(hash & (ht->capacity - 1));

    /* Allocate node */
    kvs_node_t *new_node = kvs_ht_alloc_node(ht);
    if (new_node == NULL) {
        return KVS_ERR_NOMEM;
    }

    /* Allocate deep copy of key (+1 byte for convenient NUL-termination) */
    new_node->key = (char *)malloc(key_len + 1);
    if (new_node->key == NULL) {
        kvs_ht_free_node(ht, new_node);
        return KVS_ERR_NOMEM;
    }
    memcpy(new_node->key, key, key_len);
    new_node->key[key_len] = '\0';
    new_node->key_len = key_len;
    new_node->hash = hash;

    /* Allocate deep copy of value */
    if (val_len > 0) {
        new_node->val = malloc(val_len);
        if (new_node->val == NULL) {
            free(new_node->key);
            kvs_ht_free_node(ht, new_node);
            return KVS_ERR_NOMEM;
        }
        memcpy(new_node->val, val, val_len);
    } else {
        new_node->val = NULL;
    }
    new_node->val_len = val_len;

    /* Push node to front of bucket list */
    new_node->next = ht->buckets[final_idx];
    ht->buckets[final_idx] = new_node;
    ht->count++;

    return KVS_OK;
}

kvs_status_t kvs_ht_get(
    const kvs_hash_table_t *ht,
    const char *key,
    size_t key_len,
    const void **out_val,
    size_t *out_val_len
) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC || key == NULL || key_len == 0 ||
        out_val == NULL || out_val_len == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t bucket_idx = (size_t)(hash & (ht->capacity - 1));

    kvs_node_t *curr = ht->buckets[bucket_idx];
    while (curr != NULL) {
        if (curr->hash == hash && curr->key_len == key_len &&
            memcmp(curr->key, key, key_len) == 0) {
            *out_val = curr->val;
            *out_val_len = curr->val_len;
            return KVS_OK;
        }
        curr = curr->next;
    }

    *out_val = NULL;
    *out_val_len = 0;
    return KVS_ERR_NOT_FOUND;
}

kvs_status_t kvs_ht_delete(
    kvs_hash_table_t *ht,
    const char *key,
    size_t key_len
) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC || key == NULL || key_len == 0) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t bucket_idx = (size_t)(hash & (ht->capacity - 1));

    kvs_node_t *prev = NULL;
    kvs_node_t *curr = ht->buckets[bucket_idx];

    while (curr != NULL) {
        if (curr->hash == hash && curr->key_len == key_len &&
            memcmp(curr->key, key, key_len) == 0) {

            if (prev == NULL) {
                ht->buckets[bucket_idx] = curr->next;
            } else {
                prev->next = curr->next;
            }

            free(curr->key);
            free(curr->val);
            kvs_ht_free_node(ht, curr);
            ht->count--;
            return KVS_OK;
        }
        prev = curr;
        curr = curr->next;
    }

    return KVS_ERR_NOT_FOUND;
}

bool kvs_ht_contains(
    const kvs_hash_table_t *ht,
    const char *key,
    size_t key_len
) {
    const void *val = NULL;
    size_t val_len = 0;
    return (kvs_ht_get(ht, key, key_len, &val, &val_len) == KVS_OK);
}

size_t kvs_ht_size(const kvs_hash_table_t *ht) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC) {
        return 0;
    }
    return ht->count;
}

kvs_status_t kvs_ht_get_stats(
    const kvs_hash_table_t *ht,
    kvs_ht_stats_t *out_stats
) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC || out_stats == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    out_stats->count = ht->count;
    out_stats->capacity = ht->capacity;
    out_stats->occupied_buckets = 0;
    out_stats->max_chain_length = 0;

    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_node_t *curr = ht->buckets[i];
        if (curr != NULL) {
            out_stats->occupied_buckets++;
            size_t chain = 0;
            while (curr != NULL) {
                chain++;
                curr = curr->next;
            }
            if (chain > out_stats->max_chain_length) {
                out_stats->max_chain_length = chain;
            }
        }
    }

    out_stats->load_factor = (ht->capacity > 0)
        ? ((double)ht->count / (double)ht->capacity)
        : 0.0;

    return KVS_OK;
}

void kvs_ht_destroy(kvs_hash_table_t *ht) {
    if (ht == NULL || ht->magic != KVS_HT_MAGIC) {
        return;
    }

    /* Free all bucket nodes and deep-copied keys and values */
    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_node_t *curr = ht->buckets[i];
        while (curr != NULL) {
            kvs_node_t *next = curr->next;
            free(curr->key);
            free(curr->val);
            kvs_ht_free_node(ht, curr);
            curr = next;
        }
    }

    free(ht->buckets);

    if (ht->owns_node_pool && ht->node_pool != NULL) {
        kvs_slab_pool_destroy(ht->node_pool);
    }

    ht->magic = 0;
    free(ht);
}
