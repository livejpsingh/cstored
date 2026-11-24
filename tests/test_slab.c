#include "unity.h"
#include "kvs/slab.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Validation of creation parameters */
static void test_slab_create_validation(void) {
    kvs_slab_pool_t *pool = NULL;
    
    /* NULL out parameter */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_pool_create(64, 10, 2, NULL));

    /* Zero chunk size */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_pool_create(0, 10, 2, &pool));

    /* Zero chunks per slab */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_pool_create(64, 0, 2, &pool));

    /* Valid creation */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(64, 10, 2, &pool));
    TEST_ASSERT_NOT_NULL(pool);

    kvs_slab_pool_destroy(pool);
}

/* Test 2: Verify 16-byte alignment of every allocated chunk */
static void test_slab_alignment(void) {
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(37, 32, 2, &pool));

    void *ptrs[32];
    for (size_t i = 0; i < 32; i++) {
        ptrs[i] = kvs_slab_alloc(pool);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
        /* Every pointer must strictly satisfy 16-byte alignment */
        TEST_ASSERT_EQUAL_UINT(0, ((uintptr_t)ptrs[i]) % 16);
    }

    for (size_t i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptrs[i]));
    }

    kvs_slab_pool_destroy(pool);
}

/* Test 3: Data integrity and memory safety within chunks */
static void test_slab_read_write(void) {
    const size_t chunk_size = 128;
    const size_t count = 50;
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(chunk_size, 16, 0, &pool));

    uint8_t *ptrs[50];
    for (size_t i = 0; i < count; i++) {
        ptrs[i] = (uint8_t *)kvs_slab_alloc(pool);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
        /* Fill payload with test pattern */
        memset(ptrs[i], (int)(i & 0xFF), chunk_size);
    }

    /* Verify payload integrity remained undisturbed across allocations */
    for (size_t i = 0; i < count; i++) {
        for (size_t b = 0; b < chunk_size; b++) {
            TEST_ASSERT_EQUAL_UINT((uint8_t)(i & 0xFF), ptrs[i][b]);
        }
    }

    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptrs[i]));
    }

    kvs_slab_pool_destroy(pool);
}

/* Test 4: Pool exhaustion when max_slabs is enforced */
static void test_slab_exhaustion(void) {
    kvs_slab_pool_t *pool = NULL;
    const size_t chunks_per_slab = 4;
    const size_t max_slabs = 2;
    /* Total capacity = 8 chunks */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(64, chunks_per_slab, max_slabs, &pool));

    void *ptrs[8];
    for (size_t i = 0; i < 8; i++) {
        ptrs[i] = kvs_slab_alloc(pool);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    /* 9th allocation must fail with NULL due to max_slabs limit */
    void *overflow_ptr = kvs_slab_alloc(pool);
    TEST_ASSERT_NULL(overflow_ptr);

    /* Free one chunk and verify allocation succeeds immediately */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptrs[0]));
    ptrs[0] = kvs_slab_alloc(pool);
    TEST_ASSERT_NOT_NULL(ptrs[0]);

    for (size_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptrs[i]));
    }

    kvs_slab_pool_destroy(pool);
}

/* Test 5: Double-free detection */
static void test_slab_double_free_detection(void) {
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(64, 8, 1, &pool));

    void *ptr = kvs_slab_alloc(pool);
    TEST_ASSERT_NOT_NULL(ptr);

    /* First free succeeds */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptr));

    /* Second free must return KVS_ERR_DOUBLE_FREE */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_DOUBLE_FREE, kvs_slab_free(pool, ptr));

    kvs_slab_pool_destroy(pool);
}

/* Test 6: Foreign and unaligned pointer rejection */
static void test_slab_foreign_pointer_rejection(void) {
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(64, 8, 1, &pool));

    /* Stack pointer */
    int stack_var = 42;
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_free(pool, &stack_var));

    /* NULL pointer */
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_free(pool, NULL));

    /* Valid allocation then offset pointer by 1 byte (unaligned) */
    void *ptr = kvs_slab_alloc(pool);
    TEST_ASSERT_NOT_NULL(ptr);
    uint8_t *unaligned_ptr = ((uint8_t *)ptr) + 1;
    TEST_ASSERT_EQUAL_INT(KVS_ERR_INVALID_ARG, kvs_slab_free(pool, unaligned_ptr));

    /* Clean up legitimate pointer */
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, ptr));

    kvs_slab_pool_destroy(pool);
}

/* Test 7: Memory stats accounting */
static void test_slab_stats_accounting(void) {
    kvs_slab_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_create(128, 10, 5, &pool));

    kvs_slab_stats_t stats;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_stats(pool, &stats));
    TEST_ASSERT_EQUAL_UINT(128, stats.chunk_size);
    TEST_ASSERT_EQUAL_UINT(10, stats.chunks_per_slab);
    TEST_ASSERT_EQUAL_UINT(1, stats.total_slabs);
    TEST_ASSERT_EQUAL_UINT(10, stats.total_chunks);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_allocations);

    void *p1 = kvs_slab_alloc(pool);
    void *p2 = kvs_slab_alloc(pool);
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_stats(pool, &stats));
    TEST_ASSERT_EQUAL_UINT(2, stats.active_allocations);

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, p1));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_stats(pool, &stats));
    TEST_ASSERT_EQUAL_UINT(1, stats.active_allocations);

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_free(pool, p2));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_slab_pool_stats(pool, &stats));
    TEST_ASSERT_EQUAL_UINT(0, stats.active_allocations);

    kvs_slab_pool_destroy(pool);
}

int main(void) {
    UnityBegin("tests/test_slab.c");
    RUN_TEST(test_slab_create_validation);
    RUN_TEST(test_slab_alignment);
    RUN_TEST(test_slab_read_write);
    RUN_TEST(test_slab_exhaustion);
    RUN_TEST(test_slab_double_free_detection);
    RUN_TEST(test_slab_foreign_pointer_rejection);
    RUN_TEST(test_slab_stats_accounting);
    return UnityEnd();
}
