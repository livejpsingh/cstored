#ifndef KVS_TELEMETRY_H
#define KVS_TELEMETRY_H

#include "kvs/common.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread-safe atomic metrics counters */
typedef struct {
    _Atomic uint64_t total_ops;
    _Atomic uint64_t get_ops;
    _Atomic uint64_t put_ops;
    _Atomic uint64_t delete_ops;
    _Atomic uint64_t cache_hits;
    _Atomic uint64_t cache_misses;
} kvs_telemetry_t;

/* Immutable metrics snapshot for reporting and signal dumps */
typedef struct {
    uint64_t total_ops;
    uint64_t get_ops;
    uint64_t put_ops;
    uint64_t delete_ops;
    uint64_t cache_hits;
    uint64_t cache_misses;
    double hit_rate_pct;
} kvs_telemetry_snapshot_t;

/**
 * Initializes a telemetry registry.
 */
void kvs_telemetry_init(kvs_telemetry_t *t);

/**
 * Atomic counter increment functions. Lock-free and wait-free.
 */
void kvs_telemetry_record_get(kvs_telemetry_t *t, bool is_hit);
void kvs_telemetry_record_put(kvs_telemetry_t *t);
void kvs_telemetry_record_delete(kvs_telemetry_t *t);

/**
 * Captures an instantaneous, consistent snapshot of telemetry metrics.
 */
void kvs_telemetry_get_snapshot(const kvs_telemetry_t *t, kvs_telemetry_snapshot_t *out_snapshot);

/**
 * Global singleton telemetry accessors for application-wide observability.
 */
kvs_telemetry_t *kvs_global_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif /* KVS_TELEMETRY_H */
