#ifndef KVS_COMMON_H
#define KVS_COMMON_H

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compiler attribute macros for static analysis and optimization hints.
 * Enforces return value checks at compile-time to prevent unhandled errors.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define KVS_NODISCARD __attribute__((warn_unused_result))
#  define KVS_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define KVS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define KVS_FALLTHROUGH __attribute__((fallthrough))
#else
#  define KVS_NODISCARD
#  define KVS_LIKELY(x)   (x)
#  define KVS_UNLIKELY(x) (x)
#  define KVS_FALLTHROUGH ((void)0)
#endif

/*
 * Typed error status enumeration.
 * Explicit return codes allow callers to distinguish operational edge cases
 * (e.g., key not found) from fatal failures (out of memory, system error).
 */
typedef enum {
    KVS_OK               =  0,
    KVS_ERR_NOMEM        = -1,
    KVS_ERR_INVALID_ARG  = -2,
    KVS_ERR_NOT_FOUND    = -3,
    KVS_ERR_KEY_EXISTS   = -4,
    KVS_ERR_KEY_TOO_LONG = -5,
    KVS_ERR_VAL_TOO_LONG = -6,
    KVS_ERR_OVERFLOW     = -7,
    KVS_ERR_DOUBLE_FREE  = -8,
    KVS_ERR_SYS          = -9
} kvs_status_t;

/* Standard maximum key length to prevent excessive allocation / memory abuse */
#define KVS_MAX_KEY_LEN (4096U)

/*
 * Default memory alignment for custom allocators.
 * Always align to max_align_t (16 bytes on 64-bit systems) to satisfy
 * SIMD vector register requirements and prevent unaligned access penalties/traps.
 */
#define KVS_DEFAULT_ALIGNMENT (_Alignof(max_align_t))

/*
 * Safe alignment helper.
 * Invariant: 'alignment' must be a non-zero power of two.
 * Avoids UB in pointer adjustments.
 */
static inline size_t kvs_align_up(size_t size, size_t alignment) {
    if (alignment == 0) {
        return size;
    }
    const size_t mask = alignment - 1;
    /* Pitfall avoided: check integer overflow during alignment roundup */
    if (size > SIZE_MAX - mask) {
        return 0; /* Overflow indicator */
    }
    return (size + mask) & ~mask;
}

/*
 * Overflow-safe multiplication helper for size_t.
 * Pitfall avoided: integer overflow when calculating allocation sizes (count * elem_size).
 */
static inline bool kvs_mul_overflow_size(size_t a, size_t b, size_t *out) {
    if (out == NULL) {
        return true;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a > 0 && b > SIZE_MAX / a) {
        return true;
    }
    *out = a * b;
    return false;
#endif
}

/*
 * Overflow-safe addition helper for size_t.
 */
static inline bool kvs_add_overflow_size(size_t a, size_t b, size_t *out) {
    if (out == NULL) {
        return true;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, out);
#else
    if (SIZE_MAX - a < b) {
        return true;
    }
    *out = a + b;
    return false;
#endif
}

/*
 * Return a human-readable diagnostic string for any kvs_status_t.
 */
const char *kvs_status_str(kvs_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* KVS_COMMON_H */
