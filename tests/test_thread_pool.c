#include "unity.h"
#include "kvs/thread_pool.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Configuration validation */
static void test_tp_create_validation(void) {
    kvs_thread_pool_t *pool = NULL;
    kvs_thread_pool_config_t cfg = {0, 0};

    /* NULL out_pool */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_thread_pool_create(&cfg, NULL));

    /* 0 threads */
    cfg.num_threads = 0;
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_thread_pool_create(&cfg, &pool));

    /* Valid creation */
    cfg.num_threads = 4;
    cfg.max_queue_size = 64;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_create(&cfg, &pool));
    TEST_ASSERT_NOT_NULL(pool);

    kvs_thread_pool_destroy(pool);
}

typedef struct {
    atomic_int counter;
} counter_ctx_t;

static void inc_counter_task(void *arg) {
    counter_ctx_t *ctx = (counter_ctx_t *)arg;
    atomic_fetch_add(&ctx->counter, 1);
}

/* Test 2: Concurrent task execution with atomic counter */
static void test_tp_concurrent_tasks(void) {
    kvs_thread_pool_t *pool = NULL;
    kvs_thread_pool_config_t cfg = {
        .num_threads = 8,
        .max_queue_size = 0 /* Unbounded */
    };
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_create(&cfg, &pool));

    counter_ctx_t ctx;
    atomic_init(&ctx.counter, 0);

    const int total_tasks = 2000;
    for (int i = 0; i < total_tasks; i++) {
        kvs_status_t s = kvs_thread_pool_submit(pool, inc_counter_task, &ctx);
        TEST_ASSERT_EQUAL_INT(KVS_OK, s);
    }

    /* Wait for all tasks to finish */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_wait(pool));
    TEST_ASSERT_EQUAL_INT(total_tasks, atomic_load(&ctx.counter));

    kvs_thread_pool_stats_t stats;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_get_stats(pool, &stats));
    TEST_ASSERT_EQUAL_UINT(0, stats.pending_tasks);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_workers);
    TEST_ASSERT_EQUAL_UINT(total_tasks, stats.completed_tasks);

    kvs_thread_pool_destroy(pool);
}

/* Test 3: Multiple wait cycles */
static void test_tp_multiple_wait_cycles(void) {
    kvs_thread_pool_t *pool = NULL;
    kvs_thread_pool_config_t cfg = { .num_threads = 4, .max_queue_size = 128 };
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_create(&cfg, &pool));

    counter_ctx_t ctx;
    atomic_init(&ctx.counter, 0);

    /* Batch 1 */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_submit(pool, inc_counter_task, &ctx));
    }
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_wait(pool));
    TEST_ASSERT_EQUAL_INT(100, atomic_load(&ctx.counter));

    /* Batch 2 */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_submit(pool, inc_counter_task, &ctx));
    }
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_wait(pool));
    TEST_ASSERT_EQUAL_INT(200, atomic_load(&ctx.counter));

    kvs_thread_pool_destroy(pool);
}

int main(void) {
    UnityBegin("tests/test_thread_pool.c");
    RUN_TEST(test_tp_create_validation);
    RUN_TEST(test_tp_concurrent_tasks);
    RUN_TEST(test_tp_multiple_wait_cycles);
    return UnityEnd();
}
