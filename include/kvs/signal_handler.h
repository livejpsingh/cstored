#ifndef KVS_SIGNAL_HANDLER_H
#define KVS_SIGNAL_HANDLER_H

#include "kvs/common.h"
#include "kvs/telemetry.h"
#include "kvs/concurrent_ht.h"
#include "kvs/slab.h"
#include "kvs/aof.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration context registered for async-signal-safe dumps.
 * Pointer references must remain valid for the lifetime of the signal handler.
 */
typedef struct {
    int output_fd;                      /* File descriptor to dump to (e.g. STDERR_FILENO) */
    const kvs_telemetry_t *telemetry;   /* Atomic operation counters */
    const kvs_concurrent_ht_t *ht;      /* Live hash table handle */
    const kvs_slab_pool_t *slab_pool;   /* Memory allocator pool */
    const kvs_aof_t *aof;               /* AOF persistence engine */
} kvs_signal_config_t;

/**
 * Registers the SIGUSR1 signal handler using sigaction with SA_RESTART.
 *
 * Invariant: The handler is strictly async-signal-safe. It does NOT call malloc,
 * printf, free, or acquire user-space mutexes.
 *
 * @param config Signal configuration containing target fd and context pointers.
 * @return KVS_OK on success, or KVS_ERR_SYS on failure.
 */
KVS_NODISCARD
kvs_status_t kvs_signal_handler_register(const kvs_signal_config_t *config);

/**
 * Manually invokes the telemetry dump function directly (safe for testing outside signals).
 */
void kvs_signal_dump_stats(void);

/**
 * Returns whether a SIGUSR1 signal was received and handled since the last check.
 */
bool kvs_signal_has_triggered(void);

#ifdef __cplusplus
}
#endif

#endif /* KVS_SIGNAL_HANDLER_H */
