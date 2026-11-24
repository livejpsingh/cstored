# Architecture Deep-Dive & System Specifications: `cstored`

## 1. High-Level System Architecture

`cstored` is an embedded in-memory key-value database built as a cohesive stack of modular, zero-dependency C17 subsystems:

```
+---------------------------------------------------------------------------------------+
|                               Client Operations / API Layer                           |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
|                            Thread Pool (kvs_thread_pool_t)                            |
|  - Worker Thread Queue (FIFO)                                                         |
|  - Synchronization: pthread_mutex_t + pthread_cond_t (not_empty, not_full, all_idle)  |
|  - Deterministic pthread_join lifecycle                                               |
+---------------------------------------------------------------------------------------+
                                           |
                                           v
+---------------------------------------------------------------------------------------+
|                    Fine-Grained Concurrent Table (kvs_concurrent_ht_t)                |
|  - Two-tier hierarchical lock protocol (resize_lock + stripe_lock)                    |
|  - 64 Cache-line padded (_Alignas(64)) pthread_rwlock_t stripes                       |
|  - Power-of-two bucket array with bitwise masking (hash & (capacity - 1))             |
|  - Singly-linked separate chaining collision chains                                   |
|  - Cached 64-bit MurmurHash3 for zero-rehash overhead                                  |
+---------------------------------------------------------------------------------------+
          |                                  |                                  |
(Node Allocations)                    (Value Buffers)                  (Durable Mutations)
          v                                  v                                  v
+-----------------------+          +-------------------+              +-------------------+
|  Slab Memory Pool     |          | OS Heap / mmap    |              |  AOF Persistence  |
|  (kvs_slab_pool_t)    |          +-------------------+              |  (kvs_aof_t)      |
|  - Fixed chunk stride |                                             |  - 24B Binary Hdr |
|  - Anonymous OS pages |                                             |  - Castagnoli CRC |
|  - In-band cookie ck  |                                             |  - Auto-Truncate  |
+-----------------------+                                             |  - .tmp Compaction|
                                                                      +-------------------+
                                                                                |
                                                                                v
                                                                      +-------------------+
                                                                      | Disk Storage      |
                                                                      | (kvs.aof)         |
                                                                      +-------------------+
```

---

## 2. Subsystem Deep-Dive

### 2.1 Custom Slab Memory Allocator (`include/kvs/slab.h`, `src/slab.c`)
* **Purpose:** Eliminates heap fragmentation and system call overhead for frequent node allocations.
* **OS Memory Mapping:** Allocates large anonymous pages (`mmap(MAP_ANONYMOUS | MAP_PRIVATE)` on POSIX, `VirtualAlloc` on Windows) rounded up to page boundaries (`sysconf(_SC_PAGESIZE)`).
* **Chunk Header Layout:**
  Every allocated chunk is prefixed by a 32-byte aligned header:
  ```c
  typedef struct kvs_chunk_hdr {
      uint64_t magic;               /* 0xCAFEBABE... (Allocated) or 0xDEADBEEF... (Free) */
      struct kvs_slab *owning_slab; /* Direct pointer to parent slab page */
      struct kvs_chunk_hdr *next;   /* Singly linked free-list pointer */
      uint64_t _padding;            /* Alignment padding to 32 bytes */
  } kvs_chunk_hdr_t;
  ```
* **Memory Safety & Detection:**
  * **Double-Free Detection:** Freeing a chunk whose `magic == KVS_CHUNK_MAGIC_FREE` immediately returns `KVS_ERR_DOUBLE_FREE`.
  * **Pointer Validation:** Checks that `(uint8_t *)ptr - hdr_offset` is strictly within `[slab->payload_start, slab->payload_end)`.
  * **Deterministic Teardown:** `kvs_slab_pool_destroy()` unmaps every allocated OS page, guaranteeing zero memory leaks.

---

### 2.2 64-Bit Hashing & Collision Mitigation (`include/kvs/hash.h`, `src/hash.c`)
* **Algorithm:** 64-bit MurmurHash3 mix.
* **Hardware Trapping Avoidance:** Implements `read_u64_le()` using `memcpy` to ensure alignment-safe byte loading, avoiding `SIGBUS` exceptions on architectures with strict alignment requirements (e.g. ARM, SPARC).
* **HashDoS Resistance:** Tables initialize with a cryptographically secure 64-bit random seed retrieved from operating system entropy (`/dev/urandom` / `CryptGenRandom`).

---

### 2.3 Fine-Grained Lock Striping (`include/kvs/concurrent_ht.h`, `src/concurrent_ht.c`)

```
Bucket Array: [ B0 | B1 | B2 | B3 | ... | B_N-1 ]
                |    |    |    |
                +----+----+----+---- Partitioned into 64 Stripes
                               |
Stripe Array: [ S0 | S1 | S2 | S3 | ... | S63 ]
              Each Stripe = pthread_rwlock_t + Cache-Line Padding (64 Bytes)
```

* **Stripe Selection:**
  ```c
  stripe_idx = (size_t)((hash >> 32) & (NUM_STRIPES - 1));
  ```
  Using the high 32 bits ensures that stripe selection is independent of lower bucket indexing bits `(hash & (capacity - 1))`.
* **Two-Tier Locking Hierarchy:**
  1. `resize_lock` (Tier 1): Guards the bucket array pointer and capacity during rehashing.
  2. `stripe[s].rwlock` (Tier 2): Guards individual bucket partitions.
* **Deadlock-Free Invariant:** Normal mutations acquire `resize_lock` (shared read) $\to$ `stripe[s]` (exclusive write). Dynamic resize acquires `resize_lock` (exclusive write) $\to$ all 64 stripes in strictly ascending index order ($0 \to 63$).
* **False Sharing Mitigation:** Each stripe is isolated with `_Alignas(64)` padding, preventing CPU core cache-line invalidation during high-frequency parallel writes.

---

### 2.4 Fixed-Worker Thread Pool (`include/kvs/thread_pool.h`, `src/thread_pool.c`)
* **Architecture:** Fixed number of worker threads reading from a thread-safe FIFO task queue.
* **Synchronization:**
  * `pthread_mutex_t lock`
  * `pthread_cond_t not_empty` (signals workers when tasks are available)
  * `pthread_cond_t not_full` (signals producers if queue is bounded)
  * `pthread_cond_t all_idle` (signals callers of `kvs_thread_pool_wait()`)
* **Graceful Draining & Join Invariant:** On shutdown, the pool sets `shutdown = true`, broadcasts to wake all workers, drains the queue, and invokes `pthread_join()` on every thread handle. Zero thread descriptor leaks.

---

### 2.5 AOF Persistence & Crash Recovery (`include/kvs/aof.h`, `src/aof.c`)

#### Binary Frame Layout (24 Bytes)
```
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Magic (0x414F4631)                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Opcode     |     Flags     |           Reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Castagnoli CRC32C                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Key Length                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Value Length (64-bit)                     |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Key Payload ...                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Value Payload ...                                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### Durability Policies
* `KVS_AOF_SYNC_ALWAYS`: `fdatasync()` per write (RPO = 0).
* `KVS_AOF_SYNC_EVERYSEC`: Dedicated background flusher thread flushes every 1s.
* `KVS_AOF_SYNC_NO`: Relies on OS page-cache writeback.

#### Crash Recovery Workflow
```
[Open Log File]
       |
       v
[Read 24-Byte Header] ---> (Clean EOF) ---> [Complete Recovery]
       |                                           ^
       | (Truncated Header or Invalid Magic)       |
       v                                           |
[Truncate File at Last Valid Offset (ftruncate)] --+
       |
       +---> [Read Key & Value Payloads]
                   |
                   v
             [Validate CRC32C]
              /             \
       (Mismatch)         (Match)
          /                 \
[Truncate at Last Offset]   [Replay Mutation into Table]
          |                          |
          v                          v
  [Complete Recovery]        [Advance Valid Offset] ---> Loop Next Frame
```

#### Compaction (Log Rewrite) Invariant
Compaction streams live keys to `<path>.tmp`, executes `fdatasync()`, and replaces the active file via atomic POSIX `rename()`. A crash at any moment leaves either the pristine original log or the fully committed new compacted log.
