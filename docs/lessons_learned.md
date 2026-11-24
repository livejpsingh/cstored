# Lessons Learned & Systems Pitfalls Avoided: `cstored`

This document serves as an exhaustive technical catalog of 12 critical C hazards, concurrency anti-patterns, and systems bugs identified and architecturally mitigated during the development of `cstored`. It is intended for systems engineers, C programming mentors, and code reviewers.

---

## 1. Hardware Bus Traps (`SIGBUS`) from Unaligned Memory Reads

### The Hazard
In performance-sensitive code (such as hash algorithms), developers frequently read word-sized integers by casting arbitrary byte pointers:
```c
/* FATAL ANTI-PATTERN */
uint64_t val = *(const uint64_t *)(data + offset);
```

### The Hardware Mechanics
On x86 architectures, unaligned memory loads incur microarchitectural penalties (split cache-line loads across L1/L2 boundaries). However, on strict-alignment RISC architectures (e.g. ARMv6, SPARC, MIPS), an unaligned word load generates a hardware trap that the Linux kernel surfaces as an immediate `SIGBUS` (Bus Error) aborting the process.

### The `cstored` Architectural Solution
In [src/hash.c](file:///c:/workspace/public/cstored/src/hash.c), word loading is wrapped in an alignment-safe helper:
```c
static inline uint64_t read_u64_le(const uint8_t *p) {
    uint64_t val;
    memcpy(&val, p, sizeof(val));
    return val;
}
```
Modern compilers (GCC and Clang) recognize this pattern and optimize `memcpy(&val, p, 8)` directly into a single native register load instruction while guaranteeing zero undefined behavior and zero hardware traps across all CPU targets.

---

## 2. Integer Overflow in Dynamic Buffer Sizing

### The Hazard
Calculating allocation sizes via multiplication without bounds validation:
```c
/* FATAL ANTI-PATTERN */
size_t bytes = count * sizeof(kvs_node_t);
void *ptr = malloc(bytes);
```

### The System Mechanics
If `count` is sufficiently large (e.g. during an aggressive capacity resize or adversarial input), `count * sizeof(T)` wraps around `SIZE_MAX`. For example, $2^{30} \times 8 = 2^{33}$, which on a 32-bit `size_t` truncates to $0$ or a small integer. `malloc` allocates a tiny buffer, and subsequent iterations write past its bounds, causing a severe heap buffer overflow (CWE-190).

### The `cstored` Architectural Solution
In [include/kvs/common.h](file:///c:/workspace/public/cstored/include/kvs/common.h), all arithmetic uses compiler-intrinsic checked arithmetic:
```c
static inline bool kvs_mul_overflow_size(size_t a, size_t b, size_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a > 0 && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
#endif
}
```
Every allocation in the slab allocator, hash table, and thread pool verifies `kvs_mul_overflow_size()` before invoking memory allocation syscalls, aborting with `KVS_ERR_OVERFLOW`.

---

## 3. Double-Free & Foreign Pointer Memory Corruption

### The Hazard
Returning an already freed chunk back to an allocator free-list or passing an arbitrary heap/stack pointer into `slab_free()`.

### The System Mechanics
In naive free-list allocators, adding an already freed chunk back to the head of the free list creates an immediate circular reference (`chunk->next == chunk`). Subsequent allocations return the exact same memory address to two independent caller threads, resulting in catastrophic state overwrites and data corruption.

### The `cstored` Architectural Solution
In [src/slab.c](file:///c:/workspace/public/cstored/src/slab.c), every chunk is prefixed with an in-band 64-bit state cookie:
```c
#define KVS_CHUNK_MAGIC_ALLOCATED (0xCAFEBABE74657374ULL)
#define KVS_CHUNK_MAGIC_FREE      (0xDEADBEEF534C4142ULL)

kvs_status_t kvs_slab_free(kvs_slab_pool_t *pool, void *ptr) {
    kvs_chunk_hdr_t *hdr = (kvs_chunk_hdr_t *)(((uint8_t *)ptr) - hdr_offset);
    if (hdr->magic == KVS_CHUNK_MAGIC_FREE) {
        return KVS_ERR_DOUBLE_FREE; /* Immediate deterministic abort */
    }
    if (hdr->magic != KVS_CHUNK_MAGIC_ALLOCATED) {
        return KVS_ERR_INVALID_ARG; /* Foreign pointer rejected */
    }
    /* Bounds validation: pointer must reside within its owning slab page */
    if ((uint8_t *)hdr < hdr->owning_slab->payload_start ||
        (uint8_t *)hdr >= hdr->owning_slab->payload_end) {
        return KVS_ERR_INVALID_ARG;
    }
    hdr->magic = KVS_CHUNK_MAGIC_FREE;
    /* Re-link into free-list */
}
```

---

## 4. Use-After-Free in Concurrent Reads (Copy-Out Mandatory)

### The Hazard
Returning an internal reference (`const void **out_val`) to caller code in a multi-threaded database:
```c
/* FATAL ANTI-PATTERN IN CONCURRENT STORES */
kvs_status_t kvs_get(kvs_t *ht, const char *key, const void **out_val) {
    lock_read(ht);
    node = find(key);
    *out_val = node->val; /* Returning internal pointer */
    unlock_read(ht);
    return KVS_OK;
}
```

### The Concurrency Mechanics
The moment `unlock_read()` executes, a concurrent writer thread on another core can invoke `delete()` or `put()`, freeing `node->val`. The reading caller now dereferences a dangling pointer, leading to a silent use-after-free read or memory crash (CWE-416).

### The `cstored` Architectural Solution
In [src/concurrent_ht.c](file:///c:/workspace/public/cstored/src/concurrent_ht.c), `kvs_concurrent_ht_get()` enforces **copy-out memory semantics**:
```c
/* Copy value payload out under the protection of the stripe read lock */
void *copy = malloc(curr->val_len);
memcpy(copy, curr->val, curr->val_len);
*out_val = copy;
pthread_rwlock_unlock(&ht->stripes[stripe_idx].rwlock);
```
The caller receives independent ownership of a pristine heap buffer, guaranteeing zero use-after-free vulnerabilities under peak concurrent traffic.

---

## 5. False Sharing & L1/L2 Cache Invalidation

### The Hazard
Allocating an array of synchronization primitives contiguously on the heap:
```c
/* ANTI-PATTERN: Adjacent locks sharing cache lines */
pthread_rwlock_t stripes[64];
```

### The Hardware Mechanics
Modern x86 and ARM processors maintain cache coherence at the granularity of **64-byte cache lines** using the MESI protocol. A `pthread_rwlock_t` on 64-bit Linux occupies 56 bytes. If `stripes[0]` and `stripes[1]` share a single 64-byte cache line:
* Core 1 acquiring `stripes[0]` forces the CPU bus to invalidate Core 2's L1 cache line containing `stripes[1]`.
* Even though Core 1 and Core 2 operate on completely disjoint keys, their throughput collapses to single-thread speeds due to continuous cache ping-pong.

### The `cstored` Architectural Solution
In [src/concurrent_ht.c](file:///c:/workspace/public/cstored/src/concurrent_ht.c), each stripe is explicitly isolated to its own 64-byte cache boundary:
```c
typedef struct {
    pthread_rwlock_t rwlock;
    uint8_t _pad[64 > sizeof(pthread_rwlock_t) ? 64 - sizeof(pthread_rwlock_t) : 1];
} kvs_stripe_t;
```
Combined with `_Alignas(64)`, this ensures that every lock stripe resides on an independent cache line.

---

## 6. Deadlocks During Dynamic Hash Table Resizing

### The Hazard
Resizing a multi-threaded hash table while worker threads hold bucket locks:
* Thread A (writer) locks Bucket 5 and attempts to lock Bucket 20.
* Thread B (rehashing) locks Bucket 20 and attempts to lock Bucket 5.
* The threads enter an unrecoverable **lock-order inversion deadlock**.

### The `cstored` Architectural Solution
`cstored` enforces a **Two-Tier Hierarchical Locking Protocol**:
1. All normal operations acquire Tier 1 (`resize_lock`) in shared read mode (`rdlock`), then acquire their specific Tier 2 stripe lock (`wrlock` or `rdlock`).
2. The rehashing thread acquires Tier 1 (`resize_lock`) in exclusive write mode (`wrlock`). This blocks new operations and waits for all active reads/writes across all 64 stripes to drain.
3. The resizing thread then acquires all 64 stripe write locks in **strict monotonic ascending order** ($0 \to 1 \to \dots \to 63$).
4. The table doubles, nodes are re-linked using cached hashes, and locks are released in reverse order.
5. Inversion deadlocks are mathematically impossible.

---

## 7. Spurious & Lost Wakeups in Condition Variables

### The Hazard
Evaluating condition variable wait states using `if` instead of `while`:
```c
/* FATAL ANTI-PATTERN */
if (queue_empty) {
    pthread_cond_wait(&cond, &mutex);
}
```

### The POSIX Mechanics
Under POSIX specification, `pthread_cond_wait()` can return successfully **even if no thread signaled the condition variable** (a "spurious wakeup" caused by internal kernel context switching or signal deliveries). In an `if` block, a thread proceeds assuming work exists, attempts to pop a `NULL` task pointer, and crashes.

### The `cstored` Architectural Solution
In [src/thread_pool.c](file:///c:/workspace/public/cstored/src/thread_pool.c), all wait loops evaluate state predicates inside strict `while` loops under mutex protection:
```c
while (!pool->shutdown && pool->queue_head == NULL) {
    pthread_cond_wait(&pool->not_empty, &pool->lock);
}
```

---

## 8. Thread Descriptor Leaks on Shutdown

### The Hazard
Calling `pthread_detach()` on background threads or omitting `pthread_join()`.

### The System Mechanics
When a thread terminates, its kernel stack, TLS storage, and thread descriptor (struct `pthread`) remain allocated until another thread calls `pthread_join()`. Failing to join threads causes virtual memory leakage. Conversely, using `pthread_detach()` creates shutdown races where background threads execute against partially dismantled memory structures during `main()` exit.

### The `cstored` Architectural Solution
In `kvs_thread_pool_destroy()`:
```c
pool->shutdown = true;
pthread_cond_broadcast(&pool->not_empty);
pthread_mutex_unlock(&pool->lock);

for (size_t i = 0; i < pool->num_threads; i++) {
    pthread_join(pool->threads[i], NULL);
}
```
Every created worker thread is deterministically joined before destroying synchronization primitives.

---

## 9. Signal Handler Deadlocks in Telemetry Handlers

### The Hazard
Calling standard I/O functions (`printf`, `snprintf`, `malloc`) inside a signal handler:
```c
/* FATAL ANTI-PATTERN */
void sig_handler(int sig) {
    printf("Received signal %d\n", sig);
}
```

### The POSIX Mechanics
POSIX defines a strict list of **async-signal-safe** functions. Functions like `printf()` and `malloc()` internally acquire non-reentrant glibc locks. If `SIGUSR1` interrupts a thread that is currently holding the heap mutex inside `malloc()`, the signal handler immediately attempts to re-acquire that same mutex, permanently deadlocking the process.

### The `cstored` Architectural Solution
In [src/signal_handler.c](file:///c:/workspace/public/cstored/src/signal_handler.c), the `SIGUSR1` handler executes **zero dynamic heap allocations and zero standard I/O calls**:
* Converts numbers to strings via a custom stack-based integer encoder (`safe_write_u64`).
* Emits text exclusively via the async-signal-safe POSIX `write()` syscall.

---

## 10. Torn Writes on Sudden Process Crash (`kill -9`)

### The Hazard
A server power outage or `kill -9` terminating the process midway through writing an append-only transaction.

### The Storage Mechanics
When `write()` is interrupted by an abrupt process termination, only a partial fragment of the binary frame reaches disk storage. On subsequent restart, naive log readers parse corrupted data, corrupting database state.

### The `cstored` Architectural Solution
In [src/aof.c](file:///c:/workspace/public/cstored/src/aof.c):
1. Every frame carries a 24-byte header with a 32-bit Castagnoli `crc32c` checksum computed over opcode, lengths, key, and value.
2. During recovery, `kvs_aof_recover()` verifies header bounds and checks the CRC32C.
3. The moment a partial frame, unexpected EOF, or checksum mismatch is detected, the engine invokes `ftruncate()` at the exact byte offset of the last known valid transaction, cleanly discarding partial writes and restoring pristine state.

---

## 11. Compaction Race Conditions & File Loss

### The Hazard
Compacting an active log file by overwriting the file directly or deleting and recreating it in-place.

### The Storage Mechanics
If a crash occurs midway through an in-place rewrite, the previous transaction history is already destroyed, resulting in catastrophic unrecoverable data loss.

### The `cstored` Architectural Solution
In `kvs_aof_compact()`:
1. Streams live key-value pairs into a temporary staging file (`<path>.tmp`).
2. Calls `fdatasync()` to ensure the temporary file is durably committed to physical media.
3. Uses the atomic POSIX syscall `rename(tmp_path, target_path)`.
4. If a crash occurs at any moment, the filesystem guarantees that either the original log or the fully committed new log exists—never a half-written file.

---

## 12. Interrupted Syscalls & Partial Socket/File Deliveries

### The Hazard
Invoking `write(fd, buf, len)` or `read(fd, buf, len)` and assuming the call completes the full byte count in a single pass.

### The POSIX Mechanics
Slow devices, pipes, sockets, and standard files under heavy I/O can be interrupted by signals (`EINTR`) or deliver fewer bytes than requested (partial write). Treating `write()` as a one-shot operation silently drops trailing bytes.

### The `cstored` Architectural Solution
All I/O operations in `cstored` are wrapped in looping delivery functions:
```c
static kvs_status_t kvs_write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue; /* Retry on signal */
            return KVS_ERR_SYS;
        }
        if (n == 0) return KVS_ERR_SYS;
        p += n;
        remaining -= (size_t)n;
    }
    return KVS_OK;
}
```
