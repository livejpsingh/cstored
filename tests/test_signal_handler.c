#include "unity.h"
#include "kvs/signal_handler.h"
#include "kvs/telemetry.h"
#include "kvs/concurrent_ht.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#if defined(O_BINARY)
#  define TEST_O_BINARY O_BINARY
#else
#  define TEST_O_BINARY 0
#endif

static const char *TEST_STATS_PATH = "test_signal_dump.txt";

void setUp(void) {
    (void)remove(TEST_STATS_PATH);
}

void tearDown(void) {
    (void)remove(TEST_STATS_PATH);
}

/* Test 1: Direct invocation of signal dump into pipe/file */
static void test_signal_dump_execution(void) {
    int fd = open(TEST_STATS_PATH, O_RDWR | O_CREAT | O_TRUNC | TEST_O_BINARY, 0644);
    TEST_ASSERT_TRUE(fd >= 0);

    kvs_telemetry_t telemetry;
    kvs_telemetry_init(&telemetry);

    kvs_telemetry_record_put(&telemetry);
    kvs_telemetry_record_put(&telemetry);
    kvs_telemetry_record_get(&telemetry, true);  /* Hit */
    kvs_telemetry_record_get(&telemetry, true);  /* Hit */
    kvs_telemetry_record_get(&telemetry, false); /* Miss */

    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_put(ht, "alpha", 5, "val1", 4));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_put(ht, "beta", 4, "val2", 4));

    kvs_signal_config_t cfg = {
        .output_fd = fd,
        .telemetry = &telemetry,
        .ht = ht,
        .slab_pool = NULL,
        .aof = NULL
    };

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_signal_handler_register(&cfg));

    /* Trigger signal dump */
#if defined(SIGUSR1)
    raise(SIGUSR1);
    TEST_ASSERT_TRUE(kvs_signal_has_triggered());
#else
    kvs_signal_dump_stats();
#endif

    close(fd);

    /* Read captured output and assert contents */
    FILE *fp = fopen(TEST_STATS_PATH, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char content[2048] = {0};
    size_t read_bytes = fread(content, 1, sizeof(content) - 1, fp);
    fclose(fp);

    TEST_ASSERT_TRUE(read_bytes > 0);
    TEST_ASSERT_NOT_NULL(strstr(content, "cstored RUNTIME TELEMETRY DUMP"));
    TEST_ASSERT_NOT_NULL(strstr(content, "Cache Hits:"));
    TEST_ASSERT_NOT_NULL(strstr(content, "Cache Misses:"));
    TEST_ASSERT_NOT_NULL(strstr(content, "Active Items:"));

    kvs_concurrent_ht_destroy(ht);
}

int main(void) {
    UnityBegin("tests/test_signal_handler.c");
    RUN_TEST(test_signal_dump_execution);
    return UnityEnd();
}
