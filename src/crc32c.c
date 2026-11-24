#include "kvs/crc32c.h"
#include <pthread.h>

#define CRC32C_POLYNOMIAL (0x82F63B78U)

static uint32_t crc32c_table[256];
static pthread_once_t crc32c_init_once = PTHREAD_ONCE_INIT;

/*
 * Generates the 256-entry Castagnoli CRC lookup table dynamically.
 * Eliminates typo risks in precomputed tables while ensuring identical bit-level
 * compliance with the standard iSCSI / RFC 3720 specification.
 */
static void crc32c_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1U) ? (CRC32C_POLYNOMIAL ^ (c >> 1)) : (c >> 1);
        }
        crc32c_table[i] = c;
    }
}

uint32_t kvs_crc32c(uint32_t crc, const void *data, size_t len) {
    if (data == NULL || len == 0) {
        return crc;
    }

    pthread_once(&crc32c_init_once, crc32c_init_table);

    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFU;

    for (size_t i = 0; i < len; i++) {
        c = crc32c_table[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
    }

    return c ^ 0xFFFFFFFFU;
}
