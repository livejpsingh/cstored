#include "unity.h"
#include "kvs/aof.h"
#include "kvs/concurrent_ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

static const char *TEST_AOF_PATH = "test_store.aof";

void setUp(void) {
    (void)remove(TEST_AOF_PATH);
}

void tearDown(void) {
    (void)remove(TEST_AOF_PATH);
}

/* Test 1: Append operations, sync, and recover into clean table */
static void test_aof_append_and_recover(void) {
    kvs_aof_t *aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_ALWAYS, &aof));
    TEST_ASSERT_NOT_NULL(aof);

    const char *k1 = "user:1";
    const char *v1 = "Alice";
    const char *k2 = "user:2";
    const char *v2 = "Bob";
    const char *k3 = "user:3";
    const char *v3 = "Charlie";

    /* Write 3 entries */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k1, strlen(k1), v1, strlen(v1)));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k2, strlen(k2), v2, strlen(v2)));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k3, strlen(k3), v3, strlen(v3)));

    /* Delete 1 entry */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_DELETE, k2, strlen(k2), NULL, 0));

    kvs_aof_close(aof);

    /* Open new AOF handle and recover into fresh hash table */
    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_NO, &aof));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(aof, ht));

    /* Verify k1 and k3 exist, k2 is deleted */
    TEST_ASSERT_EQUAL_UINT(2, kvs_concurrent_ht_size(ht));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k1, strlen(k1)));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k3, strlen(k3)));
    TEST_ASSERT_FALSE(kvs_concurrent_ht_contains(ht, k2, strlen(k2)));

    void *val_out = NULL;
    size_t val_len = 0;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_get(ht, k1, strlen(k1), &val_out, &val_len));
    TEST_ASSERT_EQUAL_UINT(strlen(v1), val_len);
    TEST_ASSERT_EQUAL_MEMORY(v1, val_out, val_len);
    free(val_out);

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(aof);
}

/* Test 2: AOF Compaction shrinks log size by removing deleted & overwritten entries */
static void test_aof_compaction(void) {
    kvs_aof_t *aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_NO, &aof));

    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(128, 32, &ht));

    /* Write 500 entries to both table and AOF */
    char k[32], v[32];
    for (size_t i = 0; i < 500; i++) {
        snprintf(k, sizeof(k), "comp_k_%zu", i);
        snprintf(v, sizeof(v), "comp_v_%zu", i);
        kvs_concurrent_ht_put(ht, k, strlen(k), v, strlen(v));
        kvs_aof_append(aof, KVS_AOF_OP_PUT, k, strlen(k), v, strlen(v));
    }

    /* Delete 400 entries */
    for (size_t i = 0; i < 400; i++) {
        snprintf(k, sizeof(k), "comp_k_%zu", i);
        kvs_concurrent_ht_delete(ht, k, strlen(k));
        kvs_aof_append(aof, KVS_AOF_OP_DELETE, k, strlen(k), NULL, 0));
    }

    struct stat st_before;
    stat(TEST_AOF_PATH, &st_before);

    /* Compact log */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_compact(aof, ht));

    struct stat st_after;
    stat(TEST_AOF_PATH, &st_after);

    /* Compacted log must be significantly smaller than historical log */
    TEST_ASSERT_TRUE(st_after.st_size < st_before.st_size);

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(aof);

    /* Verify recovery from compacted log restores remaining 100 entries */
    kvs_concurrent_ht_t *ht_rec = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(128, 32, &ht_rec));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_NO, &aof));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(aof, ht_rec));

    TEST_ASSERT_EQUAL_UINT(100, kvs_concurrent_ht_size(ht_rec));

    kvs_concurrent_ht_destroy(ht_rec);
    kvs_aof_close(aof);
}

/* Test 3: Truncation and graceful recovery from torn/partial trailing writes */
static void test_aof_partial_write_truncation(void) {
    kvs_aof_t *aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_ALWAYS, &aof));

    /* Append 2 valid operations */
    const char *k1 = "key_valid_1";
    const char *v1 = "payload_1";
    const char *k2 = "key_valid_2";
    const char *v2 = "payload_2";

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k1, strlen(k1), v1, strlen(v1)));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k2, strlen(k2), v2, strlen(v2)));

    kvs_aof_close(aof);

    /* Artificially append 10 bytes of garbage simulating a crash mid-frame */
    FILE *fp = fopen(TEST_AOF_PATH, "ab");
    TEST_ASSERT_NOT_NULL(fp);
    const char garbage[] = "CORRUPT_BYTES";
    size_t written = fwrite(garbage, 1, sizeof(garbage), fp);
    (void)written;
    fclose(fp);

    /* Recover */
    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(TEST_AOF_PATH, KVS_AOF_SYNC_NO, &aof));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(aof, ht));

    /* Valid items recovered */
    TEST_ASSERT_EQUAL_UINT(2, kvs_concurrent_ht_size(ht));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k1, strlen(k1)));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k2, strlen(k2)));

    kvs_aof_stats_t stats;
    kvs_aof_get_stats(aof, &stats);
    TEST_ASSERT_EQUAL_UINT(2, stats.recovered_ops);
    TEST_ASSERT_TRUE(stats.truncated_bytes > 0);

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(aof);
}

int main(void) {
    UnityBegin("tests/test_aof.c");
    RUN_TEST(test_aof_append_and_recover);
    RUN_TEST(test_aof_compaction);
    RUN_TEST(test_aof_partial_write_truncation);
    return UnityEnd();
}
