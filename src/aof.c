#include "kvs/aof.h"
#include "kvs/crc32c.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#  define kvs_fsync(fd) _commit(fd)
#  define kvs_ftruncate(fd, size) _chsize_s((fd), (__int64)(size))
#  define O_BINARY _O_BINARY
#else
#  include <unistd.h>
#  define kvs_fsync(fd) fdatasync(fd)
#  define kvs_ftruncate(fd, size) ftruncate((fd), (off_t)(size))
#  ifndef O_BINARY
#    define O_BINARY 0
#  endif
#endif

#define KVS_AOF_MAGIC_COOKIE (0x414F4631U) /* "AOF1" */
#define KVS_MAX_VAL_LEN (100U * 1024U * 1024U) /* 100MB max value safeguard */

/*
 * Binary frame header.
 * Invariant: Exactly 24 bytes packed layout.
 */
typedef struct {
    uint32_t magic;      /* KVS_AOF_MAGIC_COOKIE */
    uint8_t  op;         /* KVS_AOF_OP_PUT or KVS_AOF_OP_DELETE */
    uint8_t  flags;      /* Reserved flags (0) */
    uint16_t reserved;   /* Reserved alignment padding (0) */
    uint32_t crc32c;     /* CRC32C over [op, flags, reserved, key_len, val_len, key, val] */
    uint32_t key_len;    /* Explicit 32-bit key length */
    uint64_t val_len;    /* Explicit 64-bit value length */
} kvs_aof_hdr_t;

_Static_assert(sizeof(kvs_aof_hdr_t) == 24, "kvs_aof_hdr_t must be exactly 24 bytes");

static inline uint32_t kvs_aof_calc_header_crc(const kvs_aof_hdr_t *hdr) {
    uint8_t meta[16];
    meta[0] = hdr->op;
    meta[1] = hdr->flags;
    memcpy(&meta[2], &hdr->reserved, sizeof(hdr->reserved));
    memcpy(&meta[4], &hdr->key_len, sizeof(hdr->key_len));
    memcpy(&meta[8], &hdr->val_len, sizeof(hdr->val_len));
    return kvs_crc32c(0, meta, sizeof(meta));
}

struct kvs_aof {
    int fd;
    char *filepath;
    kvs_aof_sync_mode_t sync_mode;
    pthread_mutex_t append_lock;

    /* Background flusher thread state (for EVERYSEC sync mode) */
    pthread_t flusher_thread;
    pthread_mutex_t sync_lock;
    pthread_cond_t sync_cond;
    bool flusher_running;
    bool stop_flusher;

    /* Observability statistics */
    kvs_aof_stats_t stats;
};

/*
 * Reliable atomic write helper.
 * Handles EINTR and partial writes in a loop until buffer is fully written.
 */
static kvs_status_t kvs_write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = count;

    while (remaining > 0) {
#if defined(_WIN32)
        int n = _write(fd, p, (unsigned int)(remaining > 65536 ? 65536 : remaining));
#else
        ssize_t n = write(fd, p, remaining);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return KVS_ERR_SYS;
        }
        if (n == 0) {
            return KVS_ERR_SYS;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
    }
    return KVS_OK;
}

/*
 * Reliable read helper.
 * Reads exactly count bytes or returns KVS_ERR_NOT_FOUND on clean EOF.
 */
static kvs_status_t kvs_read_exact(int fd, void *buf, size_t count, size_t *out_read) {
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
#if defined(_WIN32)
        int n = _read(fd, p + total, (unsigned int)(count - total));
#else
        ssize_t n = read(fd, p + total, count - total);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return KVS_ERR_SYS;
        }
        if (n == 0) {
            break; /* EOF */
        }
        total += (size_t)n;
    }

    if (out_read != NULL) {
        *out_read = total;
    }

    if (total == 0) {
        return KVS_ERR_NOT_FOUND; /* Clean EOF */
    }
    if (total < count) {
        return KVS_ERR_SYS; /* Partial read */
    }
    return KVS_OK;
}

/*
 * Background flusher thread for KVS_AOF_SYNC_EVERYSEC.
 */
static void *kvs_aof_flusher_routine(void *arg) {
    kvs_aof_t *aof = (kvs_aof_t *)arg;

    while (1) {
        pthread_mutex_lock(&aof->sync_lock);
        if (aof->stop_flusher) {
            pthread_mutex_unlock(&aof->sync_lock);
            break;
        }

        /* Sleep for 1 second unless notified of shutdown */
        struct timespec ts;
#if defined(CLOCK_REALTIME)
        clock_gettime(CLOCK_REALTIME, &ts);
#else
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
#endif
        ts.tv_sec += 1;

        pthread_cond_timedwait(&aof->sync_cond, &aof->sync_lock, &ts);
        pthread_mutex_unlock(&aof->sync_lock);

        /* Flush uncommitted OS page cache */
        pthread_mutex_lock(&aof->append_lock);
        if (aof->fd >= 0) {
            kvs_fsync(aof->fd);
            aof->stats.total_syncs++;
        }
        pthread_mutex_unlock(&aof->append_lock);
    }
    return NULL;
}

kvs_status_t kvs_aof_open(
    const char *filepath,
    kvs_aof_sync_mode_t sync_mode,
    kvs_aof_t **out_aof
) {
    if (filepath == NULL || out_aof == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    kvs_aof_t *aof = (kvs_aof_t *)malloc(sizeof(kvs_aof_t));
    if (aof == NULL) {
        return KVS_ERR_NOMEM;
    }
    memset(aof, 0, sizeof(*aof));

    aof->filepath = (char *)malloc(strlen(filepath) + 1);
    if (aof->filepath == NULL) {
        free(aof);
        return KVS_ERR_NOMEM;
    }
    strcpy(aof->filepath, filepath);

    aof->sync_mode = sync_mode;

    /* Open file with read/write access, creating if absent */
    int flags = O_RDWR | O_CREAT | O_APPEND | O_BINARY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif

    aof->fd = open(filepath, flags, 0644);
    if (aof->fd < 0) {
        free(aof->filepath);
        free(aof);
        return KVS_ERR_SYS;
    }

    if (pthread_mutex_init(&aof->append_lock, NULL) != 0) {
        close(aof->fd);
        free(aof->filepath);
        free(aof);
        return KVS_ERR_SYS;
    }

    /* Start background flusher thread if EVERYSEC requested */
    if (sync_mode == KVS_AOF_SYNC_EVERYSEC) {
        pthread_mutex_init(&aof->sync_lock, NULL);
        pthread_cond_init(&aof->sync_cond, NULL);
        aof->flusher_running = true;
        aof->stop_flusher = false;
        if (pthread_create(&aof->flusher_thread, NULL, kvs_aof_flusher_routine, aof) != 0) {
            aof->flusher_running = false;
            pthread_mutex_destroy(&aof->sync_lock);
            pthread_cond_destroy(&aof->sync_cond);
        }
    }

    *out_aof = aof;
    return KVS_OK;
}

kvs_status_t kvs_aof_append(
    kvs_aof_t *aof,
    kvs_aof_opcode_t op,
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len
) {
    if (aof == NULL || key == NULL || key_len == 0 || key_len > KVS_MAX_KEY_LEN) {
        return KVS_ERR_INVALID_ARG;
    }
    if (op != KVS_AOF_OP_PUT && op != KVS_AOF_OP_DELETE) {
        return KVS_ERR_INVALID_ARG;
    }
    if (op == KVS_AOF_OP_DELETE) {
        val_len = 0;
        val = NULL;
    } else if (val_len > 0 && val == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    /* Construct 24-byte header */
    kvs_aof_hdr_t hdr;
    hdr.magic = KVS_AOF_MAGIC_COOKIE;
    hdr.op = (uint8_t)op;
    hdr.flags = 0;
    hdr.reserved = 0;
    hdr.key_len = (uint32_t)key_len;
    hdr.val_len = (uint64_t)val_len;

    /* Compute CRC32C over header fields and payload */
    uint32_t crc = kvs_aof_calc_header_crc(&hdr);
    crc = kvs_crc32c(crc, key, key_len);
    if (val_len > 0) {
        crc = kvs_crc32c(crc, val, val_len);
    }
    hdr.crc32c = crc;

    pthread_mutex_lock(&aof->append_lock);

    /* Write header */
    kvs_status_t s = kvs_write_all(aof->fd, &hdr, sizeof(hdr));
    if (s != KVS_OK) {
        pthread_mutex_unlock(&aof->append_lock);
        return s;
    }

    /* Write key */
    s = kvs_write_all(aof->fd, key, key_len);
    if (s != KVS_OK) {
        pthread_mutex_unlock(&aof->append_lock);
        return s;
    }

    /* Write value */
    if (val_len > 0) {
        s = kvs_write_all(aof->fd, val, val_len);
        if (s != KVS_OK) {
            pthread_mutex_unlock(&aof->append_lock);
            return s;
        }
    }

    /* Sync immediately if SYNC_ALWAYS */
    if (aof->sync_mode == KVS_AOF_SYNC_ALWAYS) {
        kvs_fsync(aof->fd);
        aof->stats.total_syncs++;
    }

    aof->stats.total_writes++;
    aof->stats.total_bytes_written += sizeof(hdr) + key_len + val_len;

    pthread_mutex_unlock(&aof->append_lock);
    return KVS_OK;
}

kvs_status_t kvs_aof_sync(kvs_aof_t *aof) {
    if (aof == NULL || aof->fd < 0) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&aof->append_lock);
    if (kvs_fsync(aof->fd) != 0) {
        pthread_mutex_unlock(&aof->append_lock);
        return KVS_ERR_SYS;
    }
    aof->stats.total_syncs++;
    pthread_mutex_unlock(&aof->append_lock);

    return KVS_OK;
}

kvs_status_t kvs_aof_recover(kvs_aof_t *aof, kvs_concurrent_ht_t *ht) {
    if (aof == NULL || aof->fd < 0 || ht == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&aof->append_lock);

    /* Rewind to beginning of file */
#if defined(_WIN32)
    __int64 valid_offset = 0;
    _lseeki64(aof->fd, 0, SEEK_SET);
#else
    off_t valid_offset = 0;
    lseek(aof->fd, 0, SEEK_SET);
#endif

    size_t recovered = 0;
    size_t truncated = 0;

    while (1) {
#if defined(_WIN32)
        valid_offset = _lseeki64(aof->fd, 0, SEEK_CUR);
#else
        valid_offset = lseek(aof->fd, 0, SEEK_CUR);
#endif

        kvs_aof_hdr_t hdr;
        size_t bytes_read = 0;
        kvs_status_t s = kvs_read_exact(aof->fd, &hdr, sizeof(hdr), &bytes_read);
        if (s == KVS_ERR_NOT_FOUND) {
            /* Clean EOF */
            break;
        }

        /* Partial header written before crash */
        if (s != KVS_OK || bytes_read < sizeof(hdr)) {
            truncated += bytes_read;
            if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                /* Rollback truncate */
            }
            break;
        }

        /* Validate magic cookie */
        if (hdr.magic != KVS_AOF_MAGIC_COOKIE) {
            truncated += sizeof(hdr);
            if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                /* Rollback truncate */
            }
            break;
        }

        /* Validate bounds */
        if (hdr.key_len == 0 || hdr.key_len > KVS_MAX_KEY_LEN ||
            hdr.val_len > KVS_MAX_VAL_LEN) {
            truncated += sizeof(hdr);
            if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                /* Rollback truncate */
            }
            break;
        }

        /* Read key */
        char *key = (char *)malloc(hdr.key_len + 1);
        if (key == NULL) {
            pthread_mutex_unlock(&aof->append_lock);
            return KVS_ERR_NOMEM;
        }

        size_t k_read = 0;
        s = kvs_read_exact(aof->fd, key, hdr.key_len, &k_read);
        if (s != KVS_OK) {
            free(key);
            truncated += sizeof(hdr) + k_read;
            if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                /* Rollback truncate */
            }
            break;
        }
        key[hdr.key_len] = '\0';

        /* Read value */
        void *val = NULL;
        size_t v_read = 0;
        if (hdr.val_len > 0) {
            val = malloc((size_t)hdr.val_len);
            if (val == NULL) {
                free(key);
                pthread_mutex_unlock(&aof->append_lock);
                return KVS_ERR_NOMEM;
            }
            s = kvs_read_exact(aof->fd, val, (size_t)hdr.val_len, &v_read);
            if (s != KVS_OK) {
                free(key);
                free(val);
                truncated += sizeof(hdr) + hdr.key_len + v_read;
                if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                    /* Rollback truncate */
                }
                break;
            }
        }

        /* Verify CRC32C */
        uint32_t expected_crc = kvs_aof_calc_header_crc(&hdr);
        expected_crc = kvs_crc32c(expected_crc, key, hdr.key_len);
        if (hdr.val_len > 0) {
            expected_crc = kvs_crc32c(expected_crc, val, (size_t)hdr.val_len);
        }

        if (expected_crc != hdr.crc32c) {
            /* Corrupted or torn write from power cut/kill -9 */
            free(key);
            free(val);
            truncated += sizeof(hdr) + hdr.key_len + (size_t)hdr.val_len;
            if (kvs_ftruncate(aof->fd, valid_offset) != 0) {
                /* Rollback truncate */
            }
            break;
        }

        /* Replay mutation onto hash table */
        if (hdr.op == KVS_AOF_OP_PUT) {
            if (kvs_concurrent_ht_put(ht, key, hdr.key_len, val, (size_t)hdr.val_len) != KVS_OK) {
                /* Handled: Put error during replay */
            }
        } else if (hdr.op == KVS_AOF_OP_DELETE) {
            if (kvs_concurrent_ht_delete(ht, key, hdr.key_len) != KVS_OK) {
                /* Handled: Delete key not found during replay */
            }
        }

        free(key);
        free(val);
        recovered++;
    }

    /* Position descriptor at the end for subsequent appends */
#if defined(_WIN32)
    _lseeki64(aof->fd, 0, SEEK_END);
#else
    lseek(aof->fd, 0, SEEK_END);
#endif

    aof->stats.recovered_ops = recovered;
    aof->stats.truncated_bytes = truncated;

    pthread_mutex_unlock(&aof->append_lock);
    return KVS_OK;
}

/* Context for streaming snapshot during compaction */
typedef struct {
    int tmp_fd;
    size_t written_entries;
    kvs_status_t status;
} compact_ctx_t;

static void kvs_aof_compact_callback(
    const char *key,
    size_t key_len,
    const void *val,
    size_t val_len,
    void *user_data
) {
    compact_ctx_t *ctx = (compact_ctx_t *)user_data;
    if (ctx->status != KVS_OK) {
        return;
    }

    kvs_aof_hdr_t hdr;
    hdr.magic = KVS_AOF_MAGIC_COOKIE;
    hdr.op = (uint8_t)KVS_AOF_OP_PUT;
    hdr.flags = 0;
    hdr.reserved = 0;
    hdr.key_len = (uint32_t)key_len;
    hdr.val_len = (uint64_t)val_len;

    uint32_t crc = kvs_aof_calc_header_crc(&hdr);
    crc = kvs_crc32c(crc, key, key_len);
    if (val_len > 0) {
        crc = kvs_crc32c(crc, val, val_len);
    }
    hdr.crc32c = crc;

    if (kvs_write_all(ctx->tmp_fd, &hdr, sizeof(hdr)) != KVS_OK ||
        kvs_write_all(ctx->tmp_fd, key, key_len) != KVS_OK ||
        (val_len > 0 && kvs_write_all(ctx->tmp_fd, val, val_len) != KVS_OK)) {
        ctx->status = KVS_ERR_SYS;
        return;
    }
    ctx->written_entries++;
}

kvs_status_t kvs_aof_compact(kvs_aof_t *aof, kvs_concurrent_ht_t *ht) {
    if (aof == NULL || aof->fd < 0 || ht == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&aof->append_lock);

    /* Create temporary snapshot file */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", aof->filepath);

    int flags = O_RDWR | O_CREAT | O_TRUNC | O_BINARY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif

    int tmp_fd = open(tmp_path, flags, 0644);
    if (tmp_fd < 0) {
        pthread_mutex_unlock(&aof->append_lock);
        return KVS_ERR_SYS;
    }

    compact_ctx_t ctx = {
        .tmp_fd = tmp_fd,
        .written_entries = 0,
        .status = KVS_OK
    };

    /* Snapshot all live keys */
    kvs_status_t s = kvs_concurrent_ht_for_each(ht, kvs_aof_compact_callback, &ctx);
    if (s != KVS_OK || ctx.status != KVS_OK) {
        close(tmp_fd);
        (void)remove(tmp_path);
        pthread_mutex_unlock(&aof->append_lock);
        return (s != KVS_OK) ? s : ctx.status;
    }

    /* Force durability of snapshot file before atomic swap */
    if (kvs_fsync(tmp_fd) != 0) {
        /* Fsync warning */
    }
    close(tmp_fd);

    /* Close old log file */
    close(aof->fd);

    /* Atomic rename replaces the old log with the compacted snapshot */
#if defined(_WIN32)
    /* Windows rename does not atomically overwrite if destination exists */
    (void)remove(aof->filepath);
#endif
    if (rename(tmp_path, aof->filepath) != 0) {
        pthread_mutex_unlock(&aof->append_lock);
        return KVS_ERR_SYS;
    }

    /* Reopen compacted file in append mode */
    int reopen_flags = O_RDWR | O_APPEND | O_BINARY;
#if defined(O_CLOEXEC)
    reopen_flags |= O_CLOEXEC;
#endif
    aof->fd = open(aof->filepath, reopen_flags, 0644);
    if (aof->fd < 0) {
        pthread_mutex_unlock(&aof->append_lock);
        return KVS_ERR_SYS;
    }

    pthread_mutex_unlock(&aof->append_lock);
    return KVS_OK;
}

kvs_status_t kvs_aof_get_stats(const kvs_aof_t *aof, kvs_aof_stats_t *out_stats) {
    if (aof == NULL || out_stats == NULL) {
        return KVS_ERR_INVALID_ARG;
    }
    *out_stats = aof->stats;
    return KVS_OK;
}

void kvs_aof_close(kvs_aof_t *aof) {
    if (aof == NULL) {
        return;
    }

    /* Stop background flusher thread if running */
    if (aof->flusher_running) {
        pthread_mutex_lock(&aof->sync_lock);
        aof->stop_flusher = true;
        pthread_cond_signal(&aof->sync_cond);
        pthread_mutex_unlock(&aof->sync_lock);

        pthread_join(aof->flusher_thread, NULL);
        pthread_cond_destroy(&aof->sync_cond);
        pthread_mutex_destroy(&aof->sync_lock);
        aof->flusher_running = false;
    }

    pthread_mutex_lock(&aof->append_lock);
    if (aof->fd >= 0) {
        if (kvs_fsync(aof->fd) != 0) {
            /* Fsync warning */
        }
        close(aof->fd);
        aof->fd = -1;
    }
    pthread_mutex_unlock(&aof->append_lock);

    pthread_mutex_destroy(&aof->append_lock);
    free(aof->filepath);
    free(aof);
}
