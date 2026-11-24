#ifndef KVS_LOG_H
#define KVS_LOG_H

#include "kvs/common.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVS_LOG_DEBUG = 0,
    KVS_LOG_INFO  = 1,
    KVS_LOG_WARN  = 2,
    KVS_LOG_ERROR = 3,
    KVS_LOG_FATAL = 4
} kvs_log_level_t;

/**
 * Initializes the structured logging subsystem.
 *
 * @param out_stream Target output stream (e.g. stderr, stdout, or opened log FILE*).
 * @param min_level  Minimum log level to emit.
 */
void kvs_log_init(FILE *out_stream, kvs_log_level_t min_level);

/**
 * Changes active minimum log level dynamically at runtime.
 */
void kvs_log_set_level(kvs_log_level_t min_level);

/**
 * Core logging dispatch function.
 * Thread-safe: serializes output to prevent interleaved / torn log lines.
 * Zero heap allocation: formats messages strictly on stack buffers.
 */
void kvs_log_write(
    kvs_log_level_t level,
    const char *file,
    int line,
    const char *fmt,
    ...
)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
;

/* Ergonomic logging macros */
#define KVS_LOG_D(fmt, ...) kvs_log_write(KVS_LOG_DEBUG, __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define KVS_LOG_I(fmt, ...) kvs_log_write(KVS_LOG_INFO,  __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define KVS_LOG_W(fmt, ...) kvs_log_write(KVS_LOG_WARN,  __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define KVS_LOG_E(fmt, ...) kvs_log_write(KVS_LOG_ERROR, __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define KVS_LOG_F(fmt, ...) kvs_log_write(KVS_LOG_FATAL, __FILE__, __LINE__, (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* KVS_LOG_H */
