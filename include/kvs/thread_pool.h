#ifndef KVS_THREAD_POOL_H
#define KVS_THREAD_POOL_H

#include "kvs/common.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of opaque thread pool handle */
typedef struct kvs_thread_pool kvs_thread_pool_t;

/* Task function signature */
typedef void (*kvs_task_func_t)(void *arg);

/* Pool configuration parameters */
typedef struct {
    size_t num_threads;         /* Number of worker threads to spawn */
    size_t max_queue_size;      /* Maximum capacity of pending task queue (0 for unbounded) */
} kvs_thread_pool_config_t;

/* Diagnostic statistics */
typedef struct {
    size_t active_workers;      /* Number of worker threads currently executing a task */
    size_t pending_tasks;       /* Number of tasks currently queued waiting for execution */
    size_t completed_tasks;     /* Total tasks completed since pool start */
    bool is_shutting_down;      /* True if shutdown signal has been triggered */
} kvs_thread_pool_stats_t;

/**
 * Creates and starts a worker thread pool.
 *
 * @param config   Configuration options (worker count, queue limits).
 * @param out_pool Pointer to receive created pool handle.
 * @return KVS_OK on success, or an explicit error status.
 */
KVS_NODISCARD
kvs_status_t kvs_thread_pool_create(
    const kvs_thread_pool_config_t *config,
    kvs_thread_pool_t **out_pool
);

/**
 * Submits a task to the work queue for asynchronous execution.
 * Blocks if queue is bounded and currently full.
 *
 * @param pool Valid thread pool handle.
 * @param func Function to execute in worker thread.
 * @param arg  Opaque context argument passed to func.
 * @return KVS_OK on success, KVS_ERR_INVALID_ARG on NULL parameters,
 *         or KVS_ERR_SYS if pool is already shutting down.
 */
KVS_NODISCARD
kvs_status_t kvs_thread_pool_submit(
    kvs_thread_pool_t *pool,
    kvs_task_func_t func,
    void *arg
);

/**
 * Blocks the caller until all queued tasks have finished executing.
 *
 * @param pool Valid thread pool handle.
 * @return KVS_OK on success.
 */
kvs_status_t kvs_thread_pool_wait(kvs_thread_pool_t *pool);

/**
 * Queries runtime metrics and worker load.
 *
 * @param pool      Valid thread pool handle.
 * @param out_stats Pointer to receive statistics snapshot.
 * @return KVS_OK on success.
 */
kvs_status_t kvs_thread_pool_get_stats(
    kvs_thread_pool_t *pool,
    kvs_thread_pool_stats_t *out_stats
);

/**
 * Gracefully shuts down the thread pool.
 * Drains all pending tasks, signals all workers to exit, joins all threads,
 * and releases all allocated synchronization primitives and memory.
 *
 * @param pool Thread pool handle (safe to pass NULL).
 */
void kvs_thread_pool_destroy(kvs_thread_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* KVS_THREAD_POOL_H */
