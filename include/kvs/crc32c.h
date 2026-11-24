#ifndef KVS_CRC32C_H
#define KVS_CRC32C_H

#include "kvs/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Computes the 32-bit Castagnoli CRC (CRC32C) over a buffer.
 * Polynomial: 0x1EDC6F41 (reversed: 0x82F63B78), standard RFC 3720 / iSCSI.
 *
 * @param crc  Initial CRC value (pass 0 for a new computation, or previous CRC for streaming).
 * @param data Pointer to input byte buffer.
 * @param len  Byte count of data.
 * @return Computed 32-bit checksum.
 */
uint32_t kvs_crc32c(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KVS_CRC32C_H */
