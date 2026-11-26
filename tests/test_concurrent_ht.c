#include "unity.h"
#include "kvs/concurrent_ht.h"
#include "kvs/thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Basic single-threaded CRUD on concurrent table */
static void test_cht_basic_crud(void) {
    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));
    TEST_ASSERT_NOT_NULL(ht);

    const char *k = "alpha";
    const char *v = "beta";

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_put(ht, k, strlen(k), v, strlen(v)));
    TEST_ASSERT_EQUAL_UINT(1, kvs_concurrent_ht_size(ht));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k, strlen(k)));

    void *out_v = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_get(ht, k, strlen(k), &out_v, &out_len));
    TEST_ASSERT_EQUAL_UINT(strlen(v), out_len);
    TEST_ASSERT_EQUAL_MEMORY(v, out_v, out_len);
    free(out_v);

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_delete(ht, k, strlen(k)));
    TEST_ASSERT_EQUAL_UINT(0, kvs_concurrent_ht_size(ht));
    TEST_ASSERT_FALSE(kvs_concurrent_ht_contains(ht, k, strlen(k)));

    kvs_concurrent_ht_destroy(ht);
}

typedef struct {
    kvs_concurrent_ht_t *ht;
    size_t thread_id;
    size_t ops_per_thread;
    atomic_size_t read_successes;
    atomic_size_t write_successes;
} stress_worker_arg_t;

/* Worker function simulating a concurrent client session */
static void client_worker_task(void *arg) {
    stress_worker_arg_t *w = (stress_worker_arg_t *)arg;
    char key[48];
    char val[48];

    /* Phase A: Insert thread-specific keys */
    for (size_t i = 0; i < w->ops_per_thread; i++) {
        snprintf(key, sizeof(key), "worker_%zu_key_%zu", w->thread_id, i);
        snprintf(val, sizeof(val), "val_payload_%zu_%zu", w->thread_id, i);

        kvs_status_t s = kvs_concurrent_ht_put(w->ht, key, strlen(key), val, strlen(val));
        if (s == KVS_OK) {
            atomic_fetch_add(&w->write_successes, 1);
        }
    }

    /* Phase B: Read back and verify own keys */
    for (size_t i = 0; i < w->ops_per_thread; i++) {
        snprintf(key, sizeof(key), "worker_%zu_key_%zu", w->thread_id, i);
        snprintf(val, sizeof(val), "val_payload_%zu_%zu", w->thread_id, i);

        void *out_v = NULL;
        size_t out_len = 0;
        kvs_status_t s = kvs_concurrent_ht_get(w->ht, key, strlen(key), &out_v, &out_len);
        if (s == KVS_OK) {
            if (out_len == strlen(val) && memcmp(out_v, val, out_len) == 0) {
                atomic_fetch_add(&w->read_successes, 1);
            }
            free(out_v);
        }
    }

    /* Phase C: Overwrite half the keys */
    for (size_t i = 0; i < w->ops_per_thread / 2; i++) {
        snprintf(key, sizeof(key), "worker_%zu_key_%zu", w->thread_id, i);
        snprintf(val, sizeof(val), "updated_val_%zu_%zu", w->thread_id, i);
        if (kvs_concurrent_ht_put(w->ht, key, strlen(key), val, strlen(val)) != KVS_OK) {
            /* Handled */
        }
    }

    /* Phase D: Delete a quarter of the keys */
    for (size_t i = 0; i < w->ops_per_thread / 4; i++) {
        snprintf(key, sizeof(key), "worker_%zu_key_%zu", w->thread_id, i);
        if (kvs_concurrent_ht_delete(w->ht, key, strlen(key)) != KVS_OK) {
            /* Handled */
        }
    }
}

/* Test 2: Multi-threaded concurrent stress test across thread pool */
static void test_cht_concurrent_stress(void) {
    kvs_concurrent_ht_t *ht = NULL;
    /* Start with 64 buckets and 32 stripes to trigger concurrent rehashing */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 32, &ht));

    const size_t num_workers = 8;
    const size_t ops_per_worker = 1000;

    kvs_thread_pool_t *pool = NULL;
    kvs_thread_pool_config_t cfg = {
        .num_threads = num_workers,
        .max_queue_size = 0
    };
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_create(&cfg, &pool));

    stress_worker_arg_t args[8];
    for (size_t i = 0; i < num_workers; i++) {
        args[i].ht = ht;
        args[i].thread_id = i;
        args[i].ops_per_thread = ops_per_worker;
        atomic_init(&args[i].read_successes, 0);
        atomic_init(&args[i].write_successes, 0);

        kvs_status_t s = kvs_thread_pool_submit(pool, client_worker_task, &args[i]);
        TEST_ASSERT_EQUAL_INT(KVS_OK, s);
    }

    /* Wait for all 8 concurrent clients to complete */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_wait(pool));

    /* Verify all writes and reads were successful with zero races */
    for (size_t i = 0; i < num_workers; i++) {
        TEST_ASSERT_EQUAL_UINT(ops_per_worker, atomic_load(&args[i].write_successes));
        TEST_ASSERT_EQUAL_UINT(ops_per_worker, atomic_load(&args[i].read_successes));
    }

    /* Expected remaining count = 8 * (1000 - 250 deleted) = 6000 */
    const size_t expected_remaining = num_workers * (ops_per_worker - (ops_per_worker / 4));
    TEST_ASSERT_EQUAL_UINT(expected_remaining, kvs_concurrent_ht_size(ht));

    kvs_thread_pool_destroy(pool);
    kvs_concurrent_ht_destroy(ht);
}

int main(void) {
    UnityBegin("tests/test_concurrent_ht.c");
    RUN_TEST(test_cht_basic_crud);
    RUN_TEST(test_cht_concurrent_stress);
    return UnityEnd();
}
