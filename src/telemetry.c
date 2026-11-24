#include "kvs/telemetry.h"
#include <string.h>

static kvs_telemetry_t g_global_telemetry;

void kvs_telemetry_init(kvs_telemetry_t *t) {
    if (t == NULL) {
        return;
    }
    atomic_init(&t->total_ops, 0);
    atomic_init(&t->get_ops, 0);
    atomic_init(&t->put_ops, 0);
    atomic_init(&t->delete_ops, 0);
    atomic_init(&t->cache_hits, 0);
    atomic_init(&t->cache_misses, 0);
}

void kvs_telemetry_record_get(kvs_telemetry_t *t, bool is_hit) {
    if (t == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&t->total_ops, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->get_ops, 1, memory_order_relaxed);
    if (is_hit) {
        atomic_fetch_add_explicit(&t->cache_hits, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&t->cache_misses, 1, memory_order_relaxed);
    }
}

void kvs_telemetry_record_put(kvs_telemetry_t *t) {
    if (t == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&t->total_ops, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->put_ops, 1, memory_order_relaxed);
}

void kvs_telemetry_record_delete(kvs_telemetry_t *t) {
    if (t == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&t->total_ops, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->delete_ops, 1, memory_order_relaxed);
}

void kvs_telemetry_get_snapshot(const kvs_telemetry_t *t, kvs_telemetry_snapshot_t *out_snapshot) {
    if (t == NULL || out_snapshot == NULL) {
        return;
    }
    out_snapshot->total_ops = atomic_load_explicit(&t->total_ops, memory_order_relaxed);
    out_snapshot->get_ops = atomic_load_explicit(&t->get_ops, memory_order_relaxed);
    out_snapshot->put_ops = atomic_load_explicit(&t->put_ops, memory_order_relaxed);
    out_snapshot->delete_ops = atomic_load_explicit(&t->delete_ops, memory_order_relaxed);
    out_snapshot->cache_hits = atomic_load_explicit(&t->cache_hits, memory_order_relaxed);
    out_snapshot->cache_misses = atomic_load_explicit(&t->cache_misses, memory_order_relaxed);

    const uint64_t total_lookups = out_snapshot->cache_hits + out_snapshot->cache_misses;
    if (total_lookups > 0) {
        out_snapshot->hit_rate_pct = ((double)out_snapshot->cache_hits * 100.0) / (double)total_lookups;
    } else {
        out_snapshot->hit_rate_pct = 0.0;
    }
}

kvs_telemetry_t *kvs_global_telemetry(void) {
    return &g_global_telemetry;
}
