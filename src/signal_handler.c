#include "kvs/signal_handler.h"
#include <unistd.h>
#include <string.h>

#if defined(_WIN32)
#  define STDERR_FD 2
#else
#  define STDERR_FD STDERR_FILENO
#endif

static volatile sig_atomic_t g_sigusr1_count = 0;
static kvs_signal_config_t g_signal_config = {
    .output_fd = STDERR_FD,
    .telemetry = NULL,
    .ht = NULL,
    .slab_pool = NULL,
    .aof = NULL
};

/*
 * Async-signal-safe raw write.
 * Guaranteed safe to execute from inside POSIX signal context.
 */
static void safe_write_raw(int fd, const char *buf, size_t len) {
    size_t remaining = len;
    const char *p = buf;
    while (remaining > 0) {
#if defined(_WIN32)
        int n = _write(fd, p, (unsigned int)remaining);
#else
        ssize_t n = write(fd, p, remaining);
#endif
        if (n <= 0) {
            break;
        }
        p += n;
        remaining -= (size_t)n;
    }
}

static void safe_write_str(int fd, const char *str) {
    if (str == NULL) return;
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    safe_write_raw(fd, str, len);
}

/*
 * Async-signal-safe unsigned integer to ASCII conversion.
 * Avoids snprintf, printf, and heap allocation.
 */
static void safe_write_u64(int fd, uint64_t val) {
    char buf[32];
    size_t idx = 0;
    if (val == 0) {
        safe_write_raw(fd, "0", 1);
        return;
    }
    while (val > 0 && idx < sizeof(buf)) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    /* Reverse digits in-place */
    for (size_t i = 0; i < idx / 2; i++) {
        char tmp = buf[i];
        buf[i] = buf[idx - 1 - i];
        buf[idx - 1 - i] = tmp;
    }
    safe_write_raw(fd, buf, idx);
}

/*
 * Core async-signal-safe telemetry emitter.
 */
void kvs_signal_dump_stats(void) {
    int fd = (g_signal_config.output_fd >= 0) ? g_signal_config.output_fd : STDERR_FD;

    safe_write_str(fd, "\n=======================================================\n");
    safe_write_str(fd, "           cstored RUNTIME TELEMETRY DUMP              \n");
    safe_write_str(fd, "=======================================================\n");

    /* Telemetry Counters */
    if (g_signal_config.telemetry != NULL) {
        kvs_telemetry_snapshot_t snap;
        kvs_telemetry_get_snapshot(g_signal_config.telemetry, &snap);

        safe_write_str(fd, "[Metrics & Cache Efficiency]\n");
        safe_write_str(fd, "  Total Operations: ");
        safe_write_u64(fd, snap.total_ops);
        safe_write_str(fd, "\n  GET Operations:   ");
        safe_write_u64(fd, snap.get_ops);
        safe_write_str(fd, "\n  PUT Operations:   ");
        safe_write_u64(fd, snap.put_ops);
        safe_write_str(fd, "\n  DELETE Operations:");
        safe_write_u64(fd, snap.delete_ops);
        safe_write_str(fd, "\n  Cache Hits:       ");
        safe_write_u64(fd, snap.cache_hits);
        safe_write_str(fd, "\n  Cache Misses:     ");
        safe_write_u64(fd, snap.cache_misses);
        safe_write_str(fd, "\n  Hit Rate:         ");
        safe_write_u64(fd, (uint64_t)snap.hit_rate_pct);
        safe_write_str(fd, " %\n");
    }

    /* Hash Table Size */
    if (g_signal_config.ht != NULL) {
        size_t count = kvs_concurrent_ht_size(g_signal_config.ht);
        safe_write_str(fd, "[Hash Table]\n");
        safe_write_str(fd, "  Active Items:     ");
        safe_write_u64(fd, count);
        safe_write_str(fd, "\n");
    }

    /* Memory Pool Stats */
    if (g_signal_config.slab_pool != NULL) {
        kvs_slab_stats_t s;
        if (kvs_slab_pool_stats(g_signal_config.slab_pool, &s) == KVS_OK) {
            safe_write_str(fd, "[Slab Allocator]\n");
            safe_write_str(fd, "  Active Chunks:    ");
            safe_write_u64(fd, s.active_allocations);
            safe_write_str(fd, "\n  Total Chunks:     ");
            safe_write_u64(fd, s.total_chunks);
            safe_write_str(fd, "\n  Mapped Memory:    ");
            safe_write_u64(fd, s.total_bytes_mapped);
            safe_write_str(fd, " bytes\n");
        }
    }

    /* AOF Persistence Stats */
    if (g_signal_config.aof != NULL) {
        kvs_aof_stats_t a_stats;
        if (kvs_aof_get_stats(g_signal_config.aof, &a_stats) == KVS_OK) {
            safe_write_str(fd, "[AOF Persistence]\n");
            safe_write_str(fd, "  Total Writes:     ");
            safe_write_u64(fd, a_stats.total_writes);
            safe_write_str(fd, "\n  Bytes Committed:  ");
            safe_write_u64(fd, a_stats.total_bytes_written);
            safe_write_str(fd, "\n  Syncs Executed:   ");
            safe_write_u64(fd, a_stats.total_syncs);
            safe_write_str(fd, "\n");
        }
    }

    safe_write_str(fd, "=======================================================\n\n");
}

/*
 * Signal handler callback.
 */
static void sigusr1_handler(int sig) {
    (void)sig;
    g_sigusr1_count++;
    kvs_signal_dump_stats();
}

kvs_status_t kvs_signal_handler_register(const kvs_signal_config_t *config) {
    if (config != NULL) {
        g_signal_config = *config;
    }

#if defined(SIGUSR1)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        return KVS_ERR_SYS;
    }
#endif

    return KVS_OK;
}

bool kvs_signal_has_triggered(void) {
    return (g_sigusr1_count > 0);
}
