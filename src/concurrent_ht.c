#include "kvs/concurrent_ht.h"
#include "kvs/hash.h"
#include <stdlib.h>
#include <string.h>

#define KVS_CHT_MAGIC (0x4348544D41474943ULL) /* "CHTMAGIC" */
#define KVS_LOAD_FACTOR_NUM   (3U)
#define KVS_LOAD_FACTOR_DENOM (4U) /* 75% load factor */

/* Singly linked node in bucket chain */
typedef struct kvs_cnode {
    uint64_t hash;
    char *key;
    size_t key_len;
    void *val;
    size_t val_len;
    struct kvs_cnode *next;
} kvs_cnode_t;

/*
 * Cache-line padded lock stripe (64 bytes).
 * Invariant: Prevents false sharing cache invalidation across CPU cores
 * when adjacent stripes are simultaneously locked by different threads.
 */
typedef struct {
    pthread_rwlock_t rwlock;
    uint8_t _pad[64 > sizeof(pthread_rwlock_t) ? 64 - sizeof(pthread_rwlock_t) : 1];
} kvs_stripe_t;

struct kvs_concurrent_ht {
    uint64_t magic;
    uint64_t seed;
    _Atomic size_t count;
    size_t capacity;
    kvs_cnode_t **buckets;

    /* Lock Striping Architecture */
    size_t num_stripes;
    size_t stripe_mask;
    kvs_stripe_t *stripes;

    /* Global two-tier resize lock */
    pthread_rwlock_t resize_lock;
};

/* Next power of two helper */
static size_t kvs_cht_next_pow2(size_t n) {
    if (n < 64U) {
        return 64U;
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
 * Computes stripe index from 64-bit hash.
 * Uses high bits of the hash to ensure independence from lower bucket index bits.
 */
static inline size_t kvs_cht_stripe_idx(const kvs_concurrent_ht_t *ht, uint64_t hash) {
    return (size_t)((hash >> 32) & ht->stripe_mask);
}

/*
 * Dynamically resizes the concurrent table.
 * Hierarchical locking: caller MUST have acquired exclusive pthread_rwlock_wrlock(&ht->resize_lock).
 */
static kvs_status_t kvs_cht_resize_internal(kvs_concurrent_ht_t *ht) {
    /* Acquire all stripe write locks in monotonic ascending order to prevent deadlocks */
    for (size_t s = 0; s < ht->num_stripes; s++) {
        pthread_rwlock_wrlock(&ht->stripes[s].rwlock);
    }

    size_t current_count = atomic_load(&ht->count);
    /* Double check load factor after acquiring all locks */
    if (current_count * KVS_LOAD_FACTOR_DENOM < ht->capacity * KVS_LOAD_FACTOR_NUM) {
        for (size_t s = ht->num_stripes; s > 0; s--) {
            pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
        }
        return KVS_OK;
    }

    size_t new_capacity = 0;
    if (kvs_mul_overflow_size(ht->capacity, 2U, &new_capacity)) {
        for (size_t s = ht->num_stripes; s > 0; s--) {
            pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
        }
        return KVS_ERR_OVERFLOW;
    }

    size_t alloc_bytes = 0;
    if (kvs_mul_overflow_size(new_capacity, sizeof(kvs_cnode_t *), &alloc_bytes)) {
        for (size_t s = ht->num_stripes; s > 0; s--) {
            pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
        }
        return KVS_ERR_OVERFLOW;
    }

    kvs_cnode_t **new_buckets = (kvs_cnode_t **)calloc(new_capacity, sizeof(kvs_cnode_t *));
    if (new_buckets == NULL) {
        for (size_t s = ht->num_stripes; s > 0; s--) {
            pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
        }
        return KVS_ERR_NOMEM;
    }

    const size_t new_mask = new_capacity - 1;

    /* Re-link nodes without reallocation or hash recomputation */
    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_cnode_t *curr = ht->buckets[i];
        while (curr != NULL) {
            kvs_cnode_t *next = curr->next;
            const size_t new_idx = (size_t)(curr->hash & new_mask);
            curr->next = new_buckets[new_idx];
            new_buckets[new_idx] = curr;
            curr = next;
        }
    }

    free(ht->buckets);
    ht->buckets = new_buckets;
    ht->capacity = new_capacity;

    /* Release all stripe locks in reverse order */
    for (size_t s = ht->num_stripes; s > 0; s--) {
        pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
    }

    return KVS_OK;
}

kvs_status_t kvs_concurrent_ht_create(
    size_t initial_capacity,
    size_t num_stripes,
    kvs_concurrent_ht_t **out_ht
) {
    if (out_ht == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    if (num_stripes == 0) {
        num_stripes = KVS_DEFAULT_STRIPES;
    }
    num_stripes = kvs_cht_next_pow2(num_stripes);

    kvs_concurrent_ht_t *ht = (kvs_concurrent_ht_t *)malloc(sizeof(kvs_concurrent_ht_t));
    if (ht == NULL) {
        return KVS_ERR_NOMEM;
    }
    memset(ht, 0, sizeof(*ht));

    ht->capacity = kvs_cht_next_pow2(initial_capacity);
    if (ht->capacity < num_stripes) {
        ht->capacity = num_stripes;
    }

    size_t buckets_bytes = 0;
    if (kvs_mul_overflow_size(ht->capacity, sizeof(kvs_cnode_t *), &buckets_bytes)) {
        free(ht);
        return KVS_ERR_OVERFLOW;
    }

    ht->buckets = (kvs_cnode_t **)calloc(ht->capacity, sizeof(kvs_cnode_t *));
    if (ht->buckets == NULL) {
        free(ht);
        return KVS_ERR_NOMEM;
    }

    ht->num_stripes = num_stripes;
    ht->stripe_mask = num_stripes - 1;

    size_t stripes_bytes = 0;
    if (kvs_mul_overflow_size(num_stripes, sizeof(kvs_stripe_t), &stripes_bytes)) {
        free(ht->buckets);
        free(ht);
        return KVS_ERR_OVERFLOW;
    }

    ht->stripes = (kvs_stripe_t *)malloc(stripes_bytes);
    if (ht->stripes == NULL) {
        free(ht->buckets);
        free(ht);
        return KVS_ERR_NOMEM;
    }

    /* Initialize each stripe's RW lock */
    for (size_t i = 0; i < num_stripes; i++) {
        if (pthread_rwlock_init(&ht->stripes[i].rwlock, NULL) != 0) {
            for (size_t j = 0; j < i; j++) {
                pthread_rwlock_destroy(&ht->stripes[j].rwlock);
            }
            free(ht->stripes);
            free(ht->buckets);
            free(ht);
            return KVS_ERR_SYS;
        }
    }

    if (pthread_rwlock_init(&ht->resize_lock, NULL) != 0) {
        for (size_t j = 0; j < num_stripes; j++) {
            pthread_rwlock_destroy(&ht->stripes[j].rwlock);
        }
        free(ht->stripes);
        free(ht->buckets);
        free(ht);
        return KVS_ERR_SYS;
    }

    kvs_hash_generate_seed(&ht->seed);
    atomic_init(&ht->count, 0);
    ht->magic = KVS_CHT_MAGIC;

    *out_ht = ht;
    return KVS_OK;
}

kvs_status_t kvs_concurrent_ht_put(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC || key == NULL || key_len == 0) {
        return KVS_ERR_INVALID_ARG;
    }
    if (key_len > KVS_MAX_KEY_LEN) {
        return KVS_ERR_KEY_TOO_LONG;
    }
    if (val_len > 0 && val == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t stripe_idx = kvs_cht_stripe_idx(ht, hash);

    /* Acquire shared read on global resize */
    pthread_rwlock_rdlock(&ht->resize_lock);

    /* Acquire exclusive write lock on the target stripe */
    pthread_rwlock_wrlock(&ht->stripes[stripe_idx].rwlock);

    const size_t bucket_idx = (size_t)(hash & (ht->capacity - 1));

    /* Search bucket for existing key to update */
    kvs_cnode_t *curr = ht->buckets[bucket_idx];
    while (curr != NULL) {
        if (curr->hash == hash && curr->key_len == key_len &&
            memcmp(curr->key, key, key_len) == 0) {

            /* Allocate new value before freeing old (exception safety) */
            void *new_val = NULL;
            if (val_len > 0) {
                new_val = malloc(val_len);
                if (new_val == NULL) {
                    pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
                    pthread_rwlock_unlock(&ht->resize_lock);
                    return KVS_ERR_NOMEM;
                }
                memcpy(new_val, val, val_len);
            }

            free(curr->val);
            curr->val = new_val;
            curr->val_len = val_len;

            pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
            pthread_rwlock_unlock(&ht->resize_lock);
            return KVS_OK;
        }
        curr = curr->next;
    }

    /* Key absent: allocate new node */
    kvs_cnode_t *node = (kvs_cnode_t *)malloc(sizeof(kvs_cnode_t));
    if (node == NULL) {
        pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
        pthread_rwlock_unlock(&ht->resize_lock);
        return KVS_ERR_NOMEM;
    }

    node->key = (char *)malloc(key_len + 1);
    if (node->key == NULL) {
        free(node);
        pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
        pthread_rwlock_unlock(&ht->resize_lock);
        return KVS_ERR_NOMEM;
    }
    memcpy(node->key, key, key_len);
    node->key[key_len] = '\0';
    node->key_len = key_len;
    node->hash = hash;

    if (val_len > 0) {
        node->val = malloc(val_len);
        if (node->val == NULL) {
            free(node->key);
            free(node);
            pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
            pthread_rwlock_unlock(&ht->resize_lock);
            return KVS_ERR_NOMEM;
        }
        memcpy(node->val, val, val_len);
    } else {
        node->val = NULL;
    }
    node->val_len = val_len;

    /* Push node to bucket */
    node->next = ht->buckets[bucket_idx];
    ht->buckets[bucket_idx] = node;

    const size_t new_cnt = atomic_fetch_add(&ht->count, 1) + 1;

    /* Check if table needs resizing */
    bool needs_resize = (new_cnt * KVS_LOAD_FACTOR_DENOM >= ht->capacity * KVS_LOAD_FACTOR_NUM);

    pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
    pthread_rwlock_unlock(&ht->resize_lock);

    /* If resize threshold reached, upgrade to exclusive resize lock */
    if (needs_resize) {
        pthread_rwlock_wrlock(&ht->resize_lock);
        kvs_cht_resize_internal(ht);
        pthread_rwlock_unlock(&ht->resize_lock);
    }

    return KVS_OK;
}

kvs_status_t kvs_concurrent_ht_get(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len,
    void **out_val,
    size_t *out_val_len
) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC || key == NULL || key_len == 0 ||
        out_val == NULL || out_val_len == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t stripe_idx = kvs_cht_stripe_idx(ht, hash);

    /* Shared read locks on global resize and target stripe */
    pthread_rwlock_rdlock(&ht->resize_lock);
    pthread_rwlock_rdlock(&ht->stripes[stripe_idx].rwlock);

    const size_t bucket_idx = (size_t)(hash & (ht->capacity - 1));

    kvs_cnode_t *curr = ht->buckets[bucket_idx];
    while (curr != NULL) {
        if (curr->hash == hash && curr->key_len == key_len &&
            memcmp(curr->key, key, key_len) == 0) {

            /* Deep copy value out under lock to prevent concurrent free/write race */
            void *copy = NULL;
            if (curr->val_len > 0) {
                copy = malloc(curr->val_len);
                if (copy == NULL) {
                    pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
                    pthread_rwlock_unlock(&ht->resize_lock);
                    return KVS_ERR_NOMEM;
                }
                memcpy(copy, curr->val, curr->val_len);
            }

            *out_val = copy;
            *out_val_len = curr->val_len;

            pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
            pthread_rwlock_unlock(&ht->resize_lock);
            return KVS_OK;
        }
        curr = curr->next;
    }

    pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
    pthread_rwlock_unlock(&ht->resize_lock);

    *out_val = NULL;
    *out_val_len = 0;
    return KVS_ERR_NOT_FOUND;
}

kvs_status_t kvs_concurrent_ht_delete(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len
) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC || key == NULL || key_len == 0) {
        return KVS_ERR_INVALID_ARG;
    }

    const uint64_t hash = kvs_hash64(key, key_len, ht->seed);
    const size_t stripe_idx = kvs_cht_stripe_idx(ht, hash);

    pthread_rwlock_rdlock(&ht->resize_lock);
    pthread_rwlock_wrlock(&ht->stripes[stripe_idx].rwlock);

    const size_t bucket_idx = (size_t)(hash & (ht->capacity - 1));

    kvs_cnode_t *prev = NULL;
    kvs_cnode_t *curr = ht->buckets[bucket_idx];

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
            free(curr);

            atomic_fetch_sub(&ht->count, 1);

            pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
            pthread_rwlock_unlock(&ht->resize_lock);
            return KVS_OK;
        }
        prev = curr;
        curr = curr->next;
    }

    pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
    pthread_rwlock_unlock(&ht->resize_lock);
    return KVS_ERR_NOT_FOUND;
}

bool kvs_concurrent_ht_contains(
    kvs_concurrent_ht_t *ht,
    const char *key,
    size_t key_len
) {
    void *val = NULL;
    size_t val_len = 0;
    kvs_status_t s = kvs_concurrent_ht_get(ht, key, key_len, &val, &val_len);
    if (s == KVS_OK) {
        free(val);
        return true;
    }
    return false;
}

size_t kvs_concurrent_ht_size(const kvs_concurrent_ht_t *ht) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC) {
        return 0;
    }
    return atomic_load(&ht->count);
}

kvs_status_t kvs_concurrent_ht_for_each(
    kvs_concurrent_ht_t *ht,
    kvs_entry_cb_t cb,
    void *user_data
) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC || cb == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_rwlock_rdlock(&ht->resize_lock);

    /* Acquire all stripe read locks in ascending order */
    for (size_t s = 0; s < ht->num_stripes; s++) {
        pthread_rwlock_rdlock(&ht->stripes[s].rwlock);
    }

    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_cnode_t *curr = ht->buckets[i];
        while (curr != NULL) {
            cb(curr->key, curr->key_len, curr->val, curr->val_len, user_data);
            curr = curr->next;
        }
    }

    for (size_t s = ht->num_stripes; s > 0; s--) {
        pthread_rwlock_unlock(&ht->stripes[s - 1].rwlock);
    }

    pthread_rwlock_unlock(&ht->resize_lock);
    return KVS_OK;
}

void kvs_concurrent_ht_destroy(kvs_concurrent_ht_t *ht) {
    if (ht == NULL || ht->magic != KVS_CHT_MAGIC) {
        return;
    }

    /* Free all bucket nodes */
    for (size_t i = 0; i < ht->capacity; i++) {
        kvs_cnode_t *curr = ht->buckets[i];
        while (curr != NULL) {
            kvs_cnode_t *next = curr->next;
            free(curr->key);
            free(curr->val);
            free(curr);
            curr = next;
        }
    }
    free(ht->buckets);

    /* Destroy all stripe RW locks */
    for (size_t i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_destroy(&ht->stripes[i].rwlock);
    }
    free(ht->stripes);

    pthread_rwlock_destroy(&ht->resize_lock);

    ht->magic = 0;
    free(ht);
}
