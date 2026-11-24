#include "unity.h"
#include "kvs/log.h"
#include "kvs/thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TEST_LOG_PATH = "test_run.log";

void setUp(void) {
    remove(TEST_LOG_PATH);
}

void tearDown(void) {
    remove(TEST_LOG_PATH);
}

static void log_task(void *arg) {
    size_t id = (size_t)arg;
    for (int i = 0; i < 100; i++) {
        KVS_LOG_I("Worker task %zu executing iteration %d with payload", id, i);
    }
}

/* Test: Multi-threaded concurrent logging */
static void test_concurrent_logging(void) {
    FILE *fp = fopen(TEST_LOG_PATH, "w+");
    TEST_ASSERT_NOT_NULL(fp);

    kvs_log_init(fp, KVS_LOG_DEBUG);

    KVS_LOG_D("Debug entry that should be written");
    KVS_LOG_I("System initialization complete");
    KVS_LOG_W("Warning alert simulated");
    KVS_LOG_E("Error message test");

    /* Spawn 4 worker threads logging concurrently */
    kvs_thread_pool_t *pool = NULL;
    kvs_thread_pool_config_t cfg = { .num_threads = 4, .max_queue_size = 0 };
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_create(&cfg, &pool));

    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_submit(pool, log_task, (void *)i));
    }

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_thread_pool_wait(pool));
    kvs_thread_pool_destroy(pool);

    /* Reset logger back to stderr before closing file */
    kvs_log_init(stderr, KVS_LOG_INFO);
    fclose(fp);

    /* Verify file is not empty */
    FILE *verify_fp = fopen(TEST_LOG_PATH, "r");
    TEST_ASSERT_NOT_NULL(verify_fp);
    char line[512];
    size_t line_count = 0;
    while (fgets(line, sizeof(line), verify_fp) != NULL) {
        line_count++;
        /* Verify each line contains timestamp and closing bracket */
        TEST_ASSERT_NOT_NULL(strchr(line, '['));
        TEST_ASSERT_NOT_NULL(strchr(line, ']'));
    }
    fclose(verify_fp);

    /* 4 initial logs + 400 worker logs = 404 lines */
    TEST_ASSERT_EQUAL_UINT(404, line_count);
}

int main(void) {
    UnityBegin("tests/test_log.c");
    RUN_TEST(test_concurrent_logging);
    return UnityEnd();
}
