#ifndef KVS_AOF_H
#define KVS_AOF_H

#include "kvs/common.h"
#include "kvs/concurrent_ht.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic cookie: "AOF1" in big-endian representation */
#define KVS_AOF_MAGIC (0x414F4631U)

/* AOF Opcodes */
typedef enum {
    KVS_AOF_OP_PUT    = 1,
    KVS_AOF_OP_DELETE = 2
} kvs_aof_opcode_t;

/* Durability sync modes */
typedef enum {
    KVS_AOF_SYNC_ALWAYS   = 0,  /* Call fdatasync after every write (RPO = 0) */
    KVS_AOF_SYNC_EVERYSEC = 1,  /* Background thread calls fdatasync every 1s */
    KVS_AOF_SYNC_NO       = 2   /* Relies on OS page-cache writeback */
} kvs_aof_sync_mode_t;

/* Forward declaration of opaque AOF handle */
typedef struct kvs_aof kvs_aof_t;

/* AOF diagnostic metrics */
typedef struct {
    size_t total_writes;        /* Total operations appended */
    size_t total_bytes_written; /* Total bytes committed to disk */
    size_t total_syncs;         /* Number of fdatasync calls executed */
    size_t recovered_ops;       /* Number of operations replayed during recovery */
    size_t truncated_bytes;     /* Corrupted/partial trailing bytes discarded */
} kvs_aof_stats_t;

/**
 * Opens or creates an Append-Only File.
 *
 * @param filepath  Absolute or relative path to log file.
 * @param sync_mode Durability policy (ALWAYS, EVERYSEC, NO).
 * @param out_aof   Pointer to receive created handle.
 * @return KVS_OK on success, or an explicit error status.
 */
KVS_NODISCARD
kvs_status_t kvs_aof_open(
    const char *filepath,
    kvs_aof_sync_mode_t sync_mode,
    kvs_aof_t **out_aof
);

/**
 * Appends a mutation (PUT or DELETE) to the log file with CRC32C framing.
 * Thread-safe across multiple concurrent writers.
 *
 * @param aof     Valid AOF handle.
 * @param op      KVS_AOF_OP_PUT or KVS_AOF_OP_DELETE.
 * @param key     Key byte buffer.
 * @param key_len Key byte length (1 .. KVS_MAX_KEY_LEN).
 * @param val     Value payload bytes (may be NULL if val_len == 0 or op == DELETE).
 * @param val_len Value length in bytes.
 * @return KVS_OK on success, or explicit error.
 */
KVS_NODISCARD
kvs_status_t kvs_aof_append(
    kvs_aof_t *aof,
    kvs_aof_opcode_t op,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
);

/**
 * Flushes OS write buffers to durable media using fdatasync/fsync.
 */
kvs_status_t kvs_aof_sync(kvs_aof_t *aof);

/**
 * Replays all valid operations from the AOF into the provided concurrent hash table.
 * If an incomplete frame is encountered at the end (from an unexpected crash / kill -9),
 * truncates the partial frame to restore file consistency.
 *
 * @param aof Valid AOF handle.
 * @param ht  Concurrent hash table to populate.
 * @return KVS_OK on success, or explicit error.
 */
KVS_NODISCARD
kvs_status_t kvs_aof_recover(kvs_aof_t *aof, kvs_concurrent_ht_t *ht);

/**
 * Compacts the log by snapshotting all live keys from the hash table into a new file,
 * syncing to disk, and atomically replacing the existing log via POSIX rename().
 *
 * @param aof Valid AOF handle.
 * @param ht  Concurrent hash table containing live state.
 * @return KVS_OK on success, or explicit error.
 */
KVS_NODISCARD
kvs_status_t kvs_aof_compact(kvs_aof_t *aof, kvs_concurrent_ht_t *ht);

/**
 * Retrieves observability metrics from the persistence engine.
 */
kvs_status_t kvs_aof_get_stats(const kvs_aof_t *aof, kvs_aof_stats_t *out_stats);

/**
 * Flushes remaining writes, stops background flusher thread, and closes file.
 *
 * @param aof AOF handle (safe to pass NULL).
 */
void kvs_aof_close(kvs_aof_t *aof);

#ifdef __cplusplus
}
#endif

#endif /* KVS_AOF_H */
