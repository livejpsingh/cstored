#include "unity.h"
#include "kvs/hash.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Determinism - same key and seed must always produce identical hash */
static void test_hash_determinism(void) {
    const char *key = "session:user:1002394";
    const size_t len = strlen(key);
    const uint64_t seed = 0x123456789ABCDEF0ULL;

    uint64_t h1 = kvs_hash64(key, len, seed);
    uint64_t h2 = kvs_hash64(key, len, seed);

    TEST_ASSERT_EQUAL_UINT64(h1, h2);
}

/* Test 2: Seed sensitivity - different seeds produce completely different hashes */
static void test_hash_seed_sensitivity(void) {
    const char *key = "database:cache:item_99";
    const size_t len = strlen(key);

    uint64_t h1 = kvs_hash64(key, len, 1ULL);
    uint64_t h2 = kvs_hash64(key, len, 2ULL);

    TEST_ASSERT_NOT_EQUAL(h1, h2);
}

/* Test 3: Avalanche effect - 1-bit change in input causes > 25% bit changes in output */
static void test_hash_avalanche(void) {
    char key1[] = "The quick brown fox jumps over the lazy dog";
    char key2[] = "The quick brown fox jumps over the lazy doh"; /* 1 bit flipped ('g' -> 'h') */
    const size_t len = strlen(key1);

    uint64_t h1 = kvs_hash64(key1, len, 42ULL);
    uint64_t h2 = kvs_hash64(key2, len, 42ULL);

    uint64_t diff = h1 ^ h2;
    int bit_diff_count = 0;
    for (int i = 0; i < 64; i++) {
        if ((diff >> i) & 1ULL) {
            bit_diff_count++;
        }
    }

    /*
     * For a good hash function, flipping 1 bit should flip ~32 bits (50%).
     * We assert at least 20 bits flipped to prove avalanche property.
     */
    TEST_ASSERT_TRUE(bit_diff_count >= 20);
}

/* Test 4: Binary safety - keys containing internal NUL bytes */
static void test_hash_binary_keys(void) {
    const uint8_t bin_key1[] = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x42};
    const uint8_t bin_key2[] = {0x00, 0x01, 0x00, 0xFE, 0x00, 0x42};

    uint64_t h1 = kvs_hash64(bin_key1, sizeof(bin_key1), 999ULL);
    uint64_t h2 = kvs_hash64(bin_key2, sizeof(bin_key2), 999ULL);

    TEST_ASSERT_NOT_EQUAL(h1, h2);
}

/* Test 5: Random seed generation from OS entropy */
static void test_hash_seed_generation(void) {
    uint64_t s1 = 0;
    uint64_t s2 = 0;

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_hash_generate_seed(&s1));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_hash_generate_seed(&s2));

    /* Two entropy queries should not return 0 or duplicate */
    TEST_ASSERT_TRUE(s1 != 0);
    TEST_ASSERT_TRUE(s2 != 0);
    TEST_ASSERT_NOT_EQUAL(s1, s2);
}

int main(void) {
    UnityBegin("tests/test_hash.c");
    RUN_TEST(test_hash_determinism);
    RUN_TEST(test_hash_seed_sensitivity);
    RUN_TEST(test_hash_avalanche);
    RUN_TEST(test_hash_binary_keys);
    RUN_TEST(test_hash_seed_generation);
    return UnityEnd();
}
