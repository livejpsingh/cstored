#include "kvs/hash.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <wincrypt.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* 64-bit Murmur3 mixing constants */
static const uint64_t C1 = 0x87c37b91114253d5ULL;
static const uint64_t C2 = 0x4cf5ad432745937fULL;

static inline uint64_t rotl64(uint64_t x, int8_t r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

/*
 * Reads an 8-byte little-endian block safely without unaligned access traps.
 * Pitfall avoided: *(const uint64_t *)ptr can trigger SIGBUS or undefined behavior
 * on architectures requiring strict word alignment.
 */
static inline uint64_t read_u64_le(const uint8_t *p) {
    uint64_t val;
    memcpy(&val, p, sizeof(val));
    return val;
}

uint64_t kvs_hash64(const void *key, size_t len, uint64_t seed) {
    if (key == NULL || len == 0) {
        return fmix64(seed);
    }

    const uint8_t *data = (const uint8_t *)key;
    const size_t nblocks = len / 8;

    uint64_t h = seed;

    /* Body: process 8-byte chunks */
    for (size_t i = 0; i < nblocks; i++) {
        uint64_t k = read_u64_le(data + (i * 8));

        k *= C1;
        k = rotl64(k, 31);
        k *= C2;

        h ^= k;
        h = rotl64(h, 27);
        h = h * 5 + 0x52dce729ULL;
    }

    /* Tail: process remaining 1..7 bytes */
    const uint8_t *tail = data + (nblocks * 8);
    uint64_t k1 = 0;

    switch (len & 7) {
        case 7:
            k1 ^= ((uint64_t)tail[6]) << 48;
            /* FALLTHROUGH */
        case 6:
            k1 ^= ((uint64_t)tail[5]) << 40;
            /* FALLTHROUGH */
        case 5:
            k1 ^= ((uint64_t)tail[4]) << 32;
            /* FALLTHROUGH */
        case 4:
            k1 ^= ((uint64_t)tail[3]) << 24;
            /* FALLTHROUGH */
        case 3:
            k1 ^= ((uint64_t)tail[2]) << 16;
            /* FALLTHROUGH */
        case 2:
            k1 ^= ((uint64_t)tail[1]) << 8;
            /* FALLTHROUGH */
        case 1:
            k1 ^= ((uint64_t)tail[0]);
            k1 *= C1;
            k1 = rotl64(k1, 31);
            k1 *= C2;
            h ^= k1;
            break;
        default:
            break;
    }

    /* Finalization mix - ensures avalanche across all 64 bits */
    h ^= (uint64_t)len;
    h = fmix64(h);

    return h;
}

kvs_status_t kvs_hash_generate_seed(uint64_t *out_seed) {
    if (out_seed == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

#if defined(_WIN32)
    HCRYPTPROV hCryptProv = 0;
    if (CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptGenRandom(hCryptProv, sizeof(uint64_t), (BYTE *)out_seed)) {
            CryptReleaseContext(hCryptProv, 0);
            return KVS_OK;
        }
        CryptReleaseContext(hCryptProv, 0);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, out_seed, sizeof(uint64_t));
        close(fd);
        if (n == (ssize_t)sizeof(uint64_t)) {
            return KVS_OK;
        }
    }
#endif

    /* Robust fallback in case entropy device is inaccessible */
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 123456789L;
#endif
    uint64_t fallback = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
    *out_seed = fmix64(fallback ^ (uintptr_t)out_seed);
    return KVS_OK;
}
