#include "unity.h"
#include "kvs/aof.h"
#include "kvs/concurrent_ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/types.h>
#  define HAS_POSIX_FORK 1
#else
#  define HAS_POSIX_FORK 0
#endif

static const char *CRASH_AOF_PATH = "crash_test.aof";

void setUp(void) {
    remove(CRASH_AOF_PATH);
}

void tearDown(void) {
    remove(CRASH_AOF_PATH);
}

/*
 * Test 1: Simulates hard termination (kill -9) during active stream of writes.
 * If running on Linux/POSIX, actually forks a child process and kills it with SIGKILL.
 * Then parent recovers and verifies log integrity.
 */
static void test_crash_kill_9_simulation(void) {
#if HAS_POSIX_FORK
    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        /* Child process: continuously stream writes to AOF */
        kvs_aof_t *child_aof = NULL;
        kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_NO, &child_aof);
        char key[64], val[64];

        for (size_t i = 0; i < 500000; i++) {
            snprintf(key, sizeof(key), "k_%zu", i);
            snprintf(val, sizeof(val), "val_payload_%zu", i);
            kvs_aof_append(child_aof, KVS_AOF_OP_PUT, key, strlen(key), val, strlen(val));
        }

        kvs_aof_close(child_aof);
        _exit(0);
    }

    /* Parent process: sleep briefly so child is mid-write, then kill -9 */
    struct timespec req = {0, 15000000L}; /* 15ms */
    nanosleep(&req, NULL);
    kill(pid, SIGKILL);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Verify child was terminated by SIGKILL */
    TEST_ASSERT_TRUE(WIFSIGNALED(status));
    TEST_ASSERT_EQUAL_INT(SIGKILL, WTERMSIG(status));
#else
    /* Non-POSIX / Windows emulation of torn write */
    kvs_aof_t *mock_aof = NULL;
    kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_ALWAYS, &mock_aof);
    for (size_t i = 0; i < 500; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "k_%zu", i);
        snprintf(v, sizeof(v), "val_%zu", i);
        kvs_aof_append(mock_aof, KVS_AOF_OP_PUT, k, strlen(k), v, strlen(v));
    }
    kvs_aof_close(mock_aof);

    /* Inject half-written torn frame */
    FILE *fp = fopen(CRASH_AOF_PATH, "ab");
    uint8_t torn_data[17] = {0x31, 0x46, 0x4F, 0x41, 0x01, 0x00, 0x00, 0x00};
    fwrite(torn_data, 1, sizeof(torn_data), fp);
    fclose(fp);
#endif

    /* Recovery phase: Open the crashed AOF file */
    kvs_aof_t *recover_aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_ALWAYS, &recover_aof));

    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(1024, 64, &ht));

    /* Must successfully recover up to the last intact transaction boundary */
    kvs_status_t rec_status = kvs_aof_recover(recover_aof, ht);
    TEST_ASSERT_EQUAL_INT(KVS_OK, rec_status);

    kvs_aof_stats_t stats;
    kvs_aof_get_stats(recover_aof, &stats);
    TEST_ASSERT_TRUE(stats.recovered_ops > 0);

    /* The table size must match recovered ops */
    TEST_ASSERT_EQUAL_UINT(stats.recovered_ops, kvs_concurrent_ht_size(ht));

    /* Append a new transaction to the recovered file to verify append capability */
    const char *new_k = "post_crash_key";
    const char *new_v = "post_crash_value";
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(recover_aof, KVS_AOF_OP_PUT, new_k, strlen(new_k), new_v, strlen(new_v)));

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(recover_aof);

    /* Re-open and verify second recovery incorporates the post-crash key */
    kvs_concurrent_ht_t *ht2 = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(1024, 64, &ht2));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_NO, &recover_aof));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(recover_aof, ht2));

    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht2, new_k, strlen(new_k)));
    TEST_ASSERT_EQUAL_UINT(stats.recovered_ops + 1, kvs_concurrent_ht_size(ht2));

    kvs_concurrent_ht_destroy(ht2);
    kvs_aof_close(recover_aof);
}

/* Test 2: Corrupted CRC detection */
static void test_crash_corrupted_crc_detection(void) {
    kvs_aof_t *aof = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_ALWAYS, &aof));

    const char *k1 = "crc_test_k1";
    const char *v1 = "payload_1";
    const char *k2 = "crc_test_k2";
    const char *v2 = "payload_2";

    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k1, strlen(k1), v1, strlen(v1)));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_append(aof, KVS_AOF_OP_PUT, k2, strlen(k2), v2, strlen(v2)));

    kvs_aof_close(aof);

    /* Tamper with a byte inside the second record's payload */
    FILE *fp = fopen(CRASH_AOF_PATH, "r+b");
    TEST_ASSERT_NOT_NULL(fp);
    fseek(fp, -4, SEEK_END);
    fputc(0xFF, fp); /* Corrupt byte */
    fclose(fp);

    /* Recover: Second record must fail CRC check and get truncated; first record survives */
    kvs_concurrent_ht_t *ht = NULL;
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_concurrent_ht_create(64, 16, &ht));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_open(CRASH_AOF_PATH, KVS_AOF_SYNC_NO, &aof));
    TEST_ASSERT_EQUAL_INT(KVS_OK, kvs_aof_recover(aof, ht));

    TEST_ASSERT_EQUAL_UINT(1, kvs_concurrent_ht_size(ht));
    TEST_ASSERT_TRUE(kvs_concurrent_ht_contains(ht, k1, strlen(k1)));
    TEST_ASSERT_FALSE(kvs_concurrent_ht_contains(ht, k2, strlen(k2)));

    kvs_concurrent_ht_destroy(ht);
    kvs_aof_close(aof);
}

int main(void) {
    UnityBegin("tests/test_crash_recovery.c");
    RUN_TEST(test_crash_kill_9_simulation);
    RUN_TEST(test_crash_corrupted_crc_detection);
    return UnityEnd();
}
