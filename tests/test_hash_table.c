#include "unity.h"
#include "kvs/hash_table.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Creation and argument validation */
static void test_ht_create_validation(void) {
    kvs_hash_table_t *ht = NULL;

    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_ht_create(16, NULL, NULL));

    /* Valid table creation with auto-managed slab pool */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));
    TEST_ASSERT_NOT_NULL(ht);
    TEST_ASSERT_EQUAL_UINT(0, kvs_ht_size(ht));

    kvs_ht_destroy(ht);
}

/* Test 2: Basic PUT and GET functionality */
static void test_ht_put_get_basic(void) {
    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    const char *key1 = "user:1001";
    const char *val1 = "Alice Smith";
    const char *key2 = "user:1002";
    const char *val2 = "Bob Jones";

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key1, strlen(key1), val1, strlen(val1)));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key2, strlen(key2), val2, strlen(val2)));
    TEST_ASSERT_EQUAL_UINT(2, kvs_ht_size(ht));

    const void *out_val = NULL;
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_get(ht, key1, strlen(key1), &out_val, &out_len));
    TEST_ASSERT_EQUAL_UINT(strlen(val1), out_len);
    TEST_ASSERT_EQUAL_MEMORY(val1, out_val, out_len);

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_get(ht, key2, strlen(key2), &out_val, &out_len));
    TEST_ASSERT_EQUAL_UINT(strlen(val2), out_len);
    TEST_ASSERT_EQUAL_MEMORY(val2, out_val, out_len);

    /* Query non-existent key */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_NOT_FOUND, kvs_ht_get(ht, "missing", 7, &out_val, &out_len));
    TEST_ASSERT_FALSE(kvs_ht_contains(ht, "missing", 7));

    kvs_ht_destroy(ht);
}

/* Test 3: Overwriting existing keys (updating values) */
static void test_ht_overwrite(void) {
    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    const char *key = "config:timeout";
    const char *val_old = "30";
    const char *val_new = "300000"; /* Larger payload update */

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key, strlen(key), val_old, strlen(val_old)));
    TEST_ASSERT_EQUAL_UINT(1, kvs_ht_size(ht));

    /* Overwrite */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key, strlen(key), val_new, strlen(val_new)));
    TEST_ASSERT_EQUAL_UINT(1, kvs_ht_size(ht));

    const void *out_val = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_get(ht, key, strlen(key), &out_val, &out_len));
    TEST_ASSERT_EQUAL_UINT(strlen(val_new), out_len);
    TEST_ASSERT_EQUAL_MEMORY(val_new, out_val, out_len);

    kvs_ht_destroy(ht);
}

/* Test 4: DELETE functionality */
static void test_ht_delete(void) {
    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    const char *key1 = "k1";
    const char *key2 = "k2";
    const char *key3 = "k3";

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key1, 2, "v1", 2));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key2, 2, "v2", 2));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, key3, 2, "v3", 2));
    TEST_ASSERT_EQUAL_UINT(3, kvs_ht_size(ht));

    /* Delete present key */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_delete(ht, key2, 2));
    TEST_ASSERT_EQUAL_UINT(2, kvs_ht_size(ht));
    TEST_ASSERT_FALSE(kvs_ht_contains(ht, key2, 2));

    /* Deleting already deleted key returns KVS_ERR_NOT_FOUND */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_NOT_FOUND, kvs_ht_delete(ht, key2, 2));

    /* Verify remaining keys still intact */
    TEST_ASSERT_TRUE(kvs_ht_contains(ht, key1, 2));
    TEST_ASSERT_TRUE(kvs_ht_contains(ht, key3, 2));

    kvs_ht_destroy(ht);
}

/* Test 5: Binary key and value payloads with embedded NUL bytes */
static void test_ht_binary_safety(void) {
    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    const uint8_t bin_key[] = {0x00, 0xFF, 0x00, 0x42, 0xAA};
    const uint8_t bin_val[] = {0xDE, 0xAD, 0x00, 0xBE, 0xEF, 0x00, 0x01};

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, (const char *)bin_key, sizeof(bin_key),
                                            bin_val, sizeof(bin_val)));

    const void *out_val = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_get(ht, (const char *)bin_key, sizeof(bin_key),
                                            &out_val, &out_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(bin_val), out_len);
    TEST_ASSERT_EQUAL_MEMORY(bin_val, out_val, out_len);

    kvs_ht_destroy(ht);
}

/* Test 6: Dynamic resizing under high insertion volume */
static void test_ht_dynamic_resizing_stress(void) {
    kvs_hash_table_t *ht = NULL;
    /* Start with tiny initial capacity (16 buckets) */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(16, NULL, &ht));

    const size_t insert_count = 20000;
    char key_buf[32];
    char val_buf[32];

    /* Insert 20,000 keys, forcing repeated automatic table doubling */
    for (size_t i = 0; i < insert_count; i++) {
        snprintf(key_buf, sizeof(key_buf), "stress_key_%08zu", i);
        snprintf(val_buf, sizeof(val_buf), "val_%08zu", i * 3);
        const size_t klen = strlen(key_buf);
        const size_t vlen = strlen(val_buf);

        kvs_status_t status = kvs_ht_put(ht, key_buf, klen, val_buf, vlen);
        TEST_ASSERT_EQUAL_INT(KVS_OK, status);
    }

    TEST_ASSERT_EQUAL_UINT(insert_count, kvs_ht_size(ht));

    /* Verify capacity expanded well beyond initial 16 buckets */
    kvs_ht_stats_t stats;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_get_stats(ht, &stats));
    TEST_ASSERT_TRUE(stats.capacity >= 32768);
    TEST_ASSERT_TRUE(stats.load_factor <= 0.75);

    /* Retrieve and verify all 20,000 keys */
    for (size_t i = 0; i < insert_count; i++) {
        snprintf(key_buf, sizeof(key_buf), "stress_key_%08zu", i);
        snprintf(val_buf, sizeof(val_buf), "val_%08zu", i * 3);
        const size_t klen = strlen(key_buf);
        const size_t vlen = strlen(val_buf);

        const void *out_val = NULL;
        size_t out_len = 0;
        kvs_status_t status = kvs_ht_get(ht, key_buf, klen, &out_val, &out_len);
        TEST_ASSERT_EQUAL_INT(KVS_OK, status);
        TEST_ASSERT_EQUAL_UINT(vlen, out_len);
        TEST_ASSERT_EQUAL_MEMORY(val_buf, out_val, out_len);
    }

    /* Delete half the keys */
    for (size_t i = 0; i < insert_count / 2; i++) {
        snprintf(key_buf, sizeof(key_buf), "stress_key_%08zu", i);
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_delete(ht, key_buf, strlen(key_buf)));
    }
    TEST_ASSERT_EQUAL_UINT(insert_count / 2, kvs_ht_size(ht));

    kvs_ht_destroy(ht);
}

/* Test 7: External slab pool integration */
static void test_ht_external_slab_pool(void) {
    kvs_slab_pool_t *pool = NULL;
    /* Create pool sized to hold sizeof(kvs_node_t) or larger */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(128, 64, 0, &pool));

    kvs_hash_table_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_create(32, pool, &ht));

    for (size_t i = 0; i < 50; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "k_%zu", i);
        snprintf(v, sizeof(v), "v_%zu", i);
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_ht_put(ht, k, strlen(k), v, strlen(v)));
    }

    TEST_ASSERT_EQUAL_UINT(50, kvs_ht_size(ht));

    kvs_ht_destroy(ht);
    kvs_slab_pool_destroy(pool);
}

int main(void) {
    UnityBegin("tests/test_hash_table.c");
    RUN_TEST(test_ht_create_validation);
    RUN_TEST(test_ht_put_get_basic);
    RUN_TEST(test_ht_overwrite);
    RUN_TEST(test_ht_delete);
    RUN_TEST(test_ht_binary_safety);
    RUN_TEST(test_ht_dynamic_resizing_stress);
    RUN_TEST(test_ht_external_slab_pool);
    return UnityEnd();
}
