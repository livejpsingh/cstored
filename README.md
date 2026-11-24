# cstored: Production-Grade Concurrent In-Memory Key-Value Store

A high-performance, concurrent, in-memory key-value store with slab-allocated memory pools, cache-conscious hash tables, fine-grained lock striping, a fixed-worker thread pool, and persistent write-ahead logging, built in C17 targeting Linux/POSIX environments.

---

## Architecture Overview (Phases 1 & 2)

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
                 |                                  |
       (Node Allocations)                   (Value Buffers)
                 v                                  v
+----------------------------------+   +---------------------------+
|    Slab Allocator (kvs_slab_t)   |   |   Heap / mmap Backing     |
|  - Fixed-size chunk pools        |   +---------------------------+
|  - Anonymous OS page mapping     |
|  - O(1) alloc/free free-lists    |
|  - In-band double-free detection |
+----------------------------------+
```

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

### 2. Run Test Suite
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
         ./build/debug/bin/test_concurrent_ht

valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_thread_pool
```

---

## C Pitfalls Avoided & Concurrency Lessons Learned

| C Pitfall / Concurrency Trap | Architectural Prevention in `cstored` |
| :--- | :--- |
| **Data Races in Hash Table Read/Write** | Lock striping with $N=64$ `pthread_rwlock_t` instances. Readers acquire shared locks; writers acquire exclusive stripe locks. Verified clean under ThreadSanitizer. |
| **False Sharing Cache Invalidation** | Stripes are padded to 64 bytes (`_Alignas(64)`) to match modern CPU cache lines, preventing cache ping-pong between distinct CPU cores accessing adjacent locks. |
| **Deadlocks during Dynamic Rehashing** | Two-tier hierarchical lock protocol: all normal operations acquire shared `resize_lock`, then target stripe lock. Resize acquires exclusive `resize_lock`, followed by all stripe locks in strict monotonic ascending order ($0, 1, ..., N-1$), preventing inversion deadlocks. |
| **Use-After-Free in Concurrent Reads** | `kvs_concurrent_ht_get()` deep-copies the value buffer out under the protection of the stripe read lock. Callers never hold dangling pointers to buffers concurrently modified or freed by other threads. |
| **Lost Wakeups & Spurious Wakeups** | Thread pool uses standard POSIX idioms: condition variables waited on within explicit `while (!condition)` loops under acquired mutex lock. |
| **Thread Leaks on Pool Shutdown** | `kvs_thread_pool_destroy()` broadcasts termination to all workers and unconditionally joins (`pthread_join`) every thread. |
| **Integer Overflow in Size Calculations** | Protected by `kvs_mul_overflow_size()` and `kvs_add_overflow_size()` using compiler builtins (`__builtin_mul_overflow`) before allocations. |
| **Double-Free Corruptions** | Every slab chunk header encodes a 64-bit state cookie (`KVS_CHUNK_MAGIC_ALLOCATED` vs `KVS_CHUNK_MAGIC_FREE`). Freeing an already freed chunk returns `KVS_ERR_DOUBLE_FREE`. |
