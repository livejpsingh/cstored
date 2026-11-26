#include "kvs/thread_pool.h"
#include <stdlib.h>
#include <string.h>

#define KVS_TP_MAGIC (0x54504F4F4C4D4147ULL) /* "TPOOLMAG" */

/* Singly linked task node in the FIFO queue */
typedef struct kvs_task_node {
    kvs_task_func_t func;
    void *arg;
    struct kvs_task_node *next;
} kvs_task_node_t;

/* Thread Pool Internal State */
struct kvs_thread_pool {
    uint64_t magic;
    pthread_t *threads;
    size_t num_threads;

    /* Task FIFO Queue */
    kvs_task_node_t *queue_head;
    kvs_task_node_t *queue_tail;
    size_t queue_size;
    size_t max_queue_size;

    /* Synchronization Primitives */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;   /* Signaled when task is added */
    pthread_cond_t not_full;    /* Signaled when task is popped */
    pthread_cond_t all_idle;    /* Signaled when queue is empty and active_workers == 0 */

    /* Lifecycle and Accounting */
    size_t active_workers;
    size_t completed_tasks;
    bool shutdown;
};

/*
 * Worker thread execution loop.
 * Avoids lost wakeups by acquiring lock before wait.
 * Avoids spurious wakeups by evaluating shutdown and queue state in while loop.
 */
static void *kvs_worker_routine(void *arg) {
    kvs_thread_pool_t *pool = (kvs_thread_pool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->lock);

        /* Wait until there is a task or a shutdown signal */
        while (!pool->shutdown && pool->queue_head == NULL) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }

        /* If shutting down and no more tasks remain, exit worker */
        if (pool->shutdown && pool->queue_head == NULL) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /* Pop task from FIFO head */
        kvs_task_node_t *task = pool->queue_head;
        pool->queue_head = task->next;
        if (pool->queue_head == NULL) {
            pool->queue_tail = NULL;
        }
        pool->queue_size--;
        pool->active_workers++;

        /* Signal producers that queue has space if bounded */
        if (pool->max_queue_size > 0) {
            pthread_cond_signal(&pool->not_full);
        }

        pthread_mutex_unlock(&pool->lock);

        /* Execute task outside the lock to maximize concurrency */
        if (task->func != NULL) {
            task->func(task->arg);
        }
        free(task);

        /* Mark task completed */
        pthread_mutex_lock(&pool->lock);
        pool->active_workers--;
        pool->completed_tasks++;

        /* If no active tasks and queue is drained, notify wait() */
        if (pool->queue_size == 0 && pool->active_workers == 0) {
            pthread_cond_broadcast(&pool->all_idle);
        }

        pthread_mutex_unlock(&pool->lock);
    }

    return NULL;
}

kvs_status_t kvs_thread_pool_create(
    const kvs_thread_pool_config_t *config,
    kvs_thread_pool_t **out_pool
) {
    if (config == NULL || out_pool == NULL || config->num_threads == 0) {
        return KVS_ERR_INVALID_ARG;
    }

    /* Prevent excessive thread creation */
    if (config->num_threads > 1024) {
        return KVS_ERR_INVALID_ARG;
    }

    kvs_thread_pool_t *pool = (kvs_thread_pool_t *)malloc(sizeof(kvs_thread_pool_t));
    if (pool == NULL) {
        return KVS_ERR_NOMEM;
    }
    memset(pool, 0, sizeof(*pool));

    pool->magic = KVS_TP_MAGIC;
    pool->num_threads = config->num_threads;
    pool->max_queue_size = config->max_queue_size;

    size_t threads_bytes = 0;
    if (kvs_mul_overflow_size(config->num_threads, sizeof(pthread_t), &threads_bytes)) {
        free(pool);
        return KVS_ERR_OVERFLOW;
    }

    pool->threads = (pthread_t *)malloc(threads_bytes);
    if (pool->threads == NULL) {
        free(pool);
        return KVS_ERR_NOMEM;
    }

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool->threads);
        free(pool);
        return KVS_ERR_SYS;
    }

    if (pthread_cond_init(&pool->not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->not_full, NULL) != 0 ||
        pthread_cond_init(&pool->all_idle, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        free(pool);
        return KVS_ERR_SYS;
    }

    /* Spawn worker threads */
    size_t spawned = 0;
    for (size_t i = 0; i < config->num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, kvs_worker_routine, pool) != 0) {
            /* If thread creation fails midway, gracefully shut down already spawned threads */
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->not_empty);
            for (size_t j = 0; j < spawned; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->all_idle);
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->lock);
            free(pool->threads);
            free(pool);
            return KVS_ERR_SYS;
        }
        spawned++;
    }

    *out_pool = pool;
    return KVS_OK;
}

kvs_status_t kvs_thread_pool_submit(
    kvs_thread_pool_t *pool,
    kvs_task_func_t func,
    void *arg
) {
    if (pool == NULL || pool->magic != KVS_TP_MAGIC || func == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    kvs_task_node_t *node = (kvs_task_node_t *)malloc(sizeof(kvs_task_node_t));
    if (node == NULL) {
        return KVS_ERR_NOMEM;
    }
    node->func = func;
    node->arg = arg;
    node->next = NULL;

    pthread_mutex_lock(&pool->lock);

    /* If queue is bounded, block until space is available or pool is shutdown */
    while (!pool->shutdown && pool->max_queue_size > 0 &&
           pool->queue_size >= pool->max_queue_size) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }

    /* Reject new tasks if pool has been instructed to shutdown */
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        free(node);
        return KVS_ERR_SYS;
    }

    /* Append to tail of FIFO queue */
    if (pool->queue_tail == NULL) {
        pool->queue_head = node;
        pool->queue_tail = node;
    } else {
        pool->queue_tail->next = node;
        pool->queue_tail = node;
    }
    pool->queue_size++;

    /* Signal one waiting worker */
    pthread_cond_signal(&pool->not_empty);

    pthread_mutex_unlock(&pool->lock);
    return KVS_OK;
}

kvs_status_t kvs_thread_pool_wait(kvs_thread_pool_t *pool) {
    if (pool == NULL || pool->magic != KVS_TP_MAGIC) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&pool->lock);
    while (pool->queue_size > 0 || pool->active_workers > 0) {
        pthread_cond_wait(&pool->all_idle, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);

    return KVS_OK;
}

kvs_status_t kvs_thread_pool_get_stats(
    kvs_thread_pool_t *pool,
    kvs_thread_pool_stats_t *out_stats
) {
    if (pool == NULL || pool->magic != KVS_TP_MAGIC || out_stats == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&pool->lock);
    out_stats->active_workers = pool->active_workers;
    out_stats->pending_tasks = pool->queue_size;
    out_stats->completed_tasks = pool->completed_tasks;
    out_stats->is_shutting_down = pool->shutdown;
    pthread_mutex_unlock(&pool->lock);

    return KVS_OK;
}

void kvs_thread_pool_destroy(kvs_thread_pool_t *pool) {
    if (pool == NULL || pool->magic != KVS_TP_MAGIC) {
        return;
    }

    /* Set shutdown flag and broadcast to wake up all sleeping workers */
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);

    /* Join each worker thread (guarantees zero thread leaks) */
    for (size_t i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Free any remaining unexecuted task nodes */
    kvs_task_node_t *curr = pool->queue_head;
    while (curr != NULL) {
        kvs_task_node_t *next = curr->next;
        free(curr);
        curr = next;
    }

    pthread_cond_destroy(&pool->all_idle);
    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->lock);

    free(pool->threads);
    pool->magic = 0;
    free(pool);
}
