# cstored: Production-Grade Concurrent In-Memory Key-Value Store

A high-performance, concurrent, in-memory key-value store with slab-allocated memory pools, cache-conscious hash tables, fine-grained lock striping, a fixed-worker thread pool, and persistent write-ahead logging (AOF) with CRC32C crash recovery, built in C17 targeting Linux/POSIX environments.

---

## Architecture Overview (Phases 1, 2 & 3)

```
+------------------------------------------------------------------+
|                    Client Connections / Tasks                    |
+------------------------------------------------------------------+
                                  |
                                  v
+------------------------------------------------------------------+
|                  Thread Pool (kvs_thread_pool_t)                 |
|  - Fixed worker threads with condition variable work-stealing    |
|  - Bounded or unbounded FIFO task queue                          |
|  - Graceful thread-drain and zero-leak thread joining            |
+------------------------------------------------------------------+
                                  |
                                  v
+------------------------------------------------------------------+
|       Fine-Grained Concurrent Table (kvs_concurrent_ht_t)        |
|  - Two-tier hierarchical locking (global resize + stripe locks)  |
|  - 64 Cache-line padded (_Alignas(64)) pthread_rwlock_t stripes  |
|  - Independent parallel reads & writes without false sharing     |
|  - O(1) dynamic rehashing synchronized across all stripes        |
+------------------------------------------------------------------+
          |                             |                       |
(Node Allocations)               (Value Buffers)       (Durable Mutations)
          v                             v                       v
+-------------------+         +-----------------+     +--------------------+
|  Slab Allocator   |         |   Heap / mmap   |     |    AOF Engine      |
|  (kvs_slab_t)     |         +-----------------+     |    (kvs_aof_t)     |
|  - Fixed pools    |                                 | - Binary framing   |
|  - Double-free ck |                                 | - Castagnoli CRC   |
+-------------------+                                 | - Crash recovery   |
                                                      | - Log compaction   |
                                                      +--------------------+
                                                                |
                                                                v
                                                      +--------------------+
                                                      | Disk Storage       |
                                                      | (kvs.aof)          |
                                                      +--------------------+
```

---

## AOF Framing & Crash Recovery Protocol

Each mutation is appended as a 24-byte binary framed record protected by a 32-bit CRC32C checksum:

```
+-------------------+------------------+------------------+------------------+
| Magic (4 Bytes)   | Opcode (1 Byte)  | Flags (1 Byte)   | Reserved (2 B)   |
+-------------------+------------------+------------------+------------------+
| CRC32C (4 Bytes)  | Key Length (4 B) | Value Length (8 Bytes)              |
+-------------------+------------------+------------------+------------------+
| Key Payload (Key Length Bytes)                                             |
+----------------------------------------------------------------------------+
| Value Payload (Value Length Bytes)                                         |
+----------------------------------------------------------------------------+
```

### Crash Invariant (`kill -9` Proof)
If the operating system halts or the process is killed (`kill -9`) mid-write:
1. `kvs_aof_recover()` scans records sequentially.
2. If a partial header, incomplete payload, or CRC32C mismatch is encountered at the end of the log, the recovery engine rolls back to the byte offset of the last known valid transaction.
3. It issues an `ftruncate()` to atomically discard the partial garbage bytes, restoring log integrity with zero data corruption.
4. Compaction (`kvs_aof_compact()`) snapshots live state into a `.tmp` file, calls `fdatasync()`, and atomically replaces the active log via POSIX `rename()`.

---

## Build System & Presets

The project uses CMake 3.20+ with strict compiler warning profiles (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion`).

### Build Profiles

* `dev-debug`: Debug build (`-O0 -g`) with full assertions and symbols.
* `dev-release`: Release build (`-O3 -DNDEBUG`) with maximum optimizations.
* `dev-asan`: AddressSanitizer and UndefinedBehaviorSanitizer profile (`-fsanitize=address,undefined`).
* `dev-tsan`: ThreadSanitizer profile (`-fsanitize=thread`).

---

## Build & Test Instructions

### 1. Configure and Build (Debug Profile)
```bash
cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
```

### 2. Run Complete Test Suite
```bash
ctest --test-dir build/debug --output-on-failure
```

### 3. Build & Run with ThreadSanitizer (TSan - Zero Race Conditions)
```bash
cmake -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_TSAN=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

### 4. Build & Run with AddressSanitizer (ASan + UBSan)
```bash
cmake -B build/asan -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_ASAN=ON
cmake --build build/asan --parallel
ctest --test-dir build/asan --output-on-failure
```

### 5. Valgrind Memory Leak & Error Check
```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_crash_recovery

valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_aof

valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_concurrent_ht
```

---

## C Pitfalls Avoided & Persistence Lessons Learned

| C Pitfall / Systems Hazard | Architectural Prevention in `cstored` |
| :--- | :--- |
| **Silent Log Corruption on Crash** | Fixed-size 24-byte binary framing with Castagnoli CRC32C checksums. Broken frames or partial writes are detected and truncated to the last transaction boundary. |
| **Data Races in Multi-Threaded Appends** | Writes to the AOF are guarded by `pthread_mutex_t append_lock`, ensuring strict serialization without interleaving bytes across threads. |
| **AOF Compaction File Loss / Corruption** | Compaction streams snapshot data to a separate `.tmp` file, calls `fdatasync()`, and replaces the active file via atomic `rename()`. |
| **Partial write() Call Failures** | Wrapped by `kvs_write_all()` which handles `EINTR` and partial byte deliveries in a loop until buffer is completely transferred. |
| **False Sharing Cache Invalidation** | Stripes are padded to 64 bytes (`_Alignas(64)`) to match modern CPU cache lines, preventing cache ping-pong between distinct CPU cores accessing adjacent locks. |
| **Deadlocks during Dynamic Rehashing** | Two-tier hierarchical lock protocol: all normal operations acquire shared `resize_lock`, then target stripe lock. Resize acquires exclusive `resize_lock`, followed by all stripe locks in strict monotonic ascending order ($0, 1, ..., N-1$). |
| **Integer Overflow in Size Calculations** | Protected by `kvs_mul_overflow_size()` and `kvs_add_overflow_size()` using compiler builtins (`__builtin_mul_overflow`) before allocations. |
| **Double-Free Corruptions** | Every slab chunk header encodes a 64-bit state cookie (`KVS_CHUNK_MAGIC_ALLOCATED` vs `KVS_CHUNK_MAGIC_FREE`). Freeing an already freed chunk returns `KVS_ERR_DOUBLE_FREE`. |
