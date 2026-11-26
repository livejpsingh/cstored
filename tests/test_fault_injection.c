#include "unity.h"
#include "kvs/slab.h"
#include "kvs/hash_table.h"
#include "kvs/concurrent_ht.h"
#include "kvs/aof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *FAULT_AOF_PATH = "fault_injection.aof";

void setUp(void) {
    (void)remove(FAULT_AOF_PATH);
}

void tearDown(void) {
    (void)remove(FAULT_AOF_PATH);
}

/* Test 1: Key length boundary enforcement */
static void test_key_length_overflow_defense(void) {
    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    char huge_key[KVS_MAX_KEY_LEN + 10];
    memset(huge_key, 'A', sizeof(huge_key));

    /* Exceeds KVS_MAX_KEY_LEN (4096) */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_KEY_TOO_LONG,
                          kvs_ht_put(ht, huge_key, sizeof(huge_key), "val", 3));

    /* Exactly KVS_MAX_KEY_LEN succeeds */
    TEST_ASSERT_EQUAL_INT(KVS_OK,
                          kvs_ht_put(ht, huge_key, KVS_MAX_KEY_LEN, "val", 3));

    kvs_ht_destroy(ht);
}

/* Test 2: Slab allocator defensive bounds and double-free validation */
static void test_slab_fault_injection(void) {
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(64, 4, 1, &pool));

    void *p1 = kvs_slab_alloc(pool);
    TEST_ASSERT_NOT_NULL(p1);

    /* Free once */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, p1));

    /* Double free attempt */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_DOUBLE_FREE, kvs_slab_free(pool, p1));

    /* Foreign heap pointer */
    void *heap_mem = malloc(64);
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_free(pool, heap_mem));
    free(heap_mem);

    kvs_slab_pool_destroy(pool);
}

/* Test 3: Corrupted AOF header magic and truncated recovery */
static void test_aof_corrupt_magic_recovery(void) {
    /* Write an AOF file with invalid magic cookie */
    FILE *fp = fopen(FAULT_AOF_PATH, "wb");
    TEST_ASSERT_NOT_NULL(fp);

    uint8_t bad_frame[24] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT(sizeof(bad_frame), fwrite(bad_frame, 1, sizeof(bad_frame), fp));
    fclose(fp);

    kvs_aof_t *aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(FAULT_AOF_PATH, KVS_AOF_SYNC_NO, &aof));

    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));

    /* Recovery must not crash; it should truncate the corrupt frame */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(aof, ht));
    TEST_ASSERT_EQUAL_UINT(0, kvs_concurrent_ht_size(ht));

    kvs_aof_stats_t stats;
    kvs_aof_get_stats(aof, &stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.recovered_ops);
    TEST_ASSERT_EQUAL_UINT(sizeof(bad_frame), stats.truncated_bytes);

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(aof);
}

/* Test 4: Defensive handling of NULL and zero arguments */
static void test_null_argument_resilience(void) {
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_ht_put(NULL, "k", 1, "v", 1));
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_ht_delete(NULL, "k", 1));
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_concurrent_ht_put(NULL, "k", 1, "v", 1));
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_aof_append(NULL, KVS_AOF_OP_PUT, "k", 1, "v", 1));
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_aof_sync(NULL));
}

int main(void) {
    UnityBegin("tests/test_fault_injection.c");
    RUN_TEST(test_key_length_overflow_defense);
    RUN_TEST(test_slab_fault_injection);
    RUN_TEST(test_aof_corrupt_magic_recovery);
    RUN_TEST(test_null_argument_resilience);
    return UnityEnd();
}
