#include "unity.h"
#include "kvs/crc32c.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Test 1: RFC 3720 standard test vector "123456789" */
static void test_crc32c_rfc_vector(void) {
    const char *input = "123456789";
    const uint32_t expected = 0xE3069283U;

    uint32_t actual = kvs_crc32c(0, input, strlen(input));
    TEST_ASSERT_EQUAL_HEX32(expected, actual);
}

/* Test 2: 32 bytes of zeroes */
static void test_crc32c_32_zeroes(void) {
    const uint8_t zeroes[32] = {0};
    /* Expected CRC32C for 32 zeros: 0x8A9136AA */
    const uint32_t expected = 0x8A9136AAU;

    uint32_t actual = kvs_crc32c(0, zeroes, sizeof(zeroes));
    TEST_ASSERT_EQUAL_HEX32(expected, actual);
}

/* Test 3: Incremental / Streaming CRC calculation */
static void test_crc32c_streaming(void) {
    const char *part1 = "1234";
    const char *part2 = "56789";
    const uint32_t expected = 0xE3069283U;

    uint32_t c1 = kvs_crc32c(0, part1, strlen(part1));
    uint32_t c2 = kvs_crc32c(c1, part2, strlen(part2));

    TEST_ASSERT_EQUAL_HEX32(expected, c2);
}

/* Test 4: NULL and empty buffers */
static void test_crc32c_empty(void) {
    TEST_ASSERT_EQUAL_HEX32(0, kvs_crc32c(0, NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, kvs_crc32c(0x12345678, NULL, 0));
}

int main(void) {
    UnityBegin("tests/test_crc32c.c");
    RUN_TEST(test_crc32c_rfc_vector);
    RUN_TEST(test_crc32c_32_zeroes);
    RUN_TEST(test_crc32c_streaming);
    RUN_TEST(test_crc32c_empty);
    return UnityEnd();
}
