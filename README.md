# cstored: Production-Grade Concurrent In-Memory Key-Value Store

A high-performance, concurrent, in-memory key-value store with slab-allocated memory pools, cache-conscious hash tables, fine-grained lock striping, a fixed-worker thread pool, and persistent write-ahead logging (AOF) with CRC32C crash recovery, built in C17 targeting Linux/POSIX environments.

---

## Business Problem & Solution

### The Problem

Modern real-time applications — financial tickers, high-frequency trading platforms, gaming leaderboards, ad-tech bidding exchanges, telecommunications routers, and in-process caching layers — share one hard operational requirement:

> **Serve millions of concurrent read/write operations with sub-millisecond latency, without ever corrupting data or crashing under peak load.**

Off-the-shelf solutions (such as Redis or Memcached) are widely adopted but introduce **network serialization hops** (adding 0.5ms–2.5ms per request) and **costly operational overhead** (managing cluster sidecars, sentinels, replication links, and container fleets). Teams needing embedded, zero-dependency, in-process performance are forced to choose between two bad outcomes:

1. **Build a custom in-memory store in-house** and risk subtle concurrency bugs (data races, deadlocks during hash table resizing, use-after-free, memory fragmentation) that cause **catastrophic production outages** and compliance breaches.
2. **Deploy heavy external clusters** that add **unacceptable tail latency, infrastructure bloat, and high cloud bills**.

Both outcomes harm the business: lost revenue from downtime, poor user conversion from high latency, and regulatory risk from state corruption.

---

### How `cstored` Solves It (Phases 1, 2 & 3 — Core Engine & Durability)

`cstored` provides a **provably safe, high-performance concurrent core** that eliminates the most common technical root causes of production failure.

| Business Risk | Technical Root Cause | `cstored` Solution (Implemented) | Business Outcome |
| :--- | :--- | :--- | :--- |
| **Revenue loss from downtime** | Crashes from heap corruption, double-frees, memory leaks, thread descriptor leaks | Custom slab allocator with **in-band 64-bit state cookies**; thread pool with **guaranteed join on shutdown** | Higher availability and strictly zero memory/thread leaks |
| **Poor UX from high latency** | A single global lock serializing all reads and writes across worker threads | **64-way lock striping** with cache-line padding (`_Alignas(64)`) for true parallel reads and writes | Predictable sub-millisecond tail latency ($P_{99.9}$) at scale |
| **Data corruption & compliance risk** | Data races and use-after-free under concurrent mutation | **ThreadSanitizer-verified** zero races; **deep-copy-on-read** prevents dangling pointers | Data integrity and compliance by design |
| **Deadlocks under load** | Lock-order inversion during dynamic hash table resizing | **Two-tier hierarchical lock protocol** (shared resize lock $\to$ stripe lock; resize acquires all stripes in strict ascending order) | Provably deadlock-free operations |
| **Data loss on server panic / kill -9** | In-memory writes discarded when process aborts or container is restarted | **Append-Only File (AOF)** with 24-byte binary framing, Castagnoli **CRC32C checksums**, and automated trailing partial-write truncation | Durability by default (RPO = 0 with `SYNC_ALWAYS`); automatic crash self-repair |
| **High cloud infra / maintenance cost** | Memory fragmentation from millions of small allocations + external cluster overhead | Slab allocator groups allocations into contiguous OS pages (`mmap`); zero third-party dependencies | Lower Total Cost of Ownership (TCO), reduced RAM footprint |

---

### Value Proposition for Engineering Leadership

For engineering leadership and infrastructure architects, `cstored` offers:

* **A Foundational In-Process Building Block:** Embed a trusted, high-performance KV core directly into your C/C++ backend binary with zero external dependencies, no licensing fees, and no daemon sidecars.
* **Proactive Risk Mitigation:** Rigorous sanitizer validation (ASan, UBSan, TSan) and Valgrind leak checking enforced in automated CI reduce production incident risk to near-zero.
* **Linear Performance Headroom:** 64-way fine-grained lock striping scales throughput near-linearly with CPU cores, protecting system performance as traffic scales.
* **Audit & Durability Compliance:** Checksummed write-ahead logging guarantees crash survivability, meeting enterprise disaster recovery standards.

---

### Roadmap to Full Business Value

* [x] **Phase 1 — Memory Engine & Hash Table:** Slab pool allocator, 64-bit MurmurHash3 table with $O(1)$ dynamic rehashing, strict compiler warnings, and Unity test harness.
* [x] **Phase 2 — Concurrency & Scalability:** Fixed-worker thread pool, 64-way cache-line padded lock striping, concurrent client stress tests, and ThreadSanitizer validation.
* [x] **Phase 3 — Durability & Crash Recovery:** Binary AOF persistence, Castagnoli CRC32C, atomic `.tmp` log compaction, and resilience tests surviving `kill -9`.
* [ ] **Phase 4 — Observability & Hardening:** Async-signal-safe `SIGUSR1` telemetry dump (memory, cache hit rate, log ops) and structured non-blocking logging.
* [ ] **Phase 5 — Production Sign-Off:** Operational runbook, final code audit, and benchmark review.

---

## Documentation Suite

Detailed architectural and developer guides are located in the [`docs/`](file:///c:/workspace/public/cstored/docs/) directory:

* [**Business Understanding & Executive Brief**](file:///c:/workspace/public/cstored/docs/business_understanding.md): Strategic alignment, market positioning, TCO analysis, and business impact.
* [**Developer Onboarding & Engineering Guide**](file:///c:/workspace/public/cstored/docs/developer_guide.md): Mental models, invariants, two-tier locking rules, memory ownership, and coding standards.
* [**Architecture Deep-Dive & System Specifications**](file:///c:/workspace/public/cstored/docs/architecture.md): Component diagrams, binary framing layouts, CRC32C specifications, and crash recovery proofs.

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
|  - Fixed pools    |                                 | - 24B Binary frame |
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

## Build System & Presets

The project uses modern CMake (3.20+) with strict compiler diagnostics (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -std=c17`).

### Build Profiles

* `dev-debug`: Debug build (`-O0 -g`) with full assertions and symbols.
* `dev-release`: Release build (`-O3 -DNDEBUG`) with maximum optimizations.
* `dev-asan`: AddressSanitizer and UndefinedBehaviorSanitizer profile (`-fsanitize=address,undefined`).
* `dev-tsan`: ThreadSanitizer profile (`-fsanitize=thread`).

---

## Build & Test Instructions

### 1. Configure and Build (Debug Profile)
```bash
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
```

### 2. Run Test Suite
```bash
ctest --test-dir build/debug --output-on-failure
```

### 3. Build & Run with ThreadSanitizer (TSan - Zero Race Conditions)
```bash
cmake -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_TSAN=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

### 4. Build & Run with AddressSanitizer (ASan + UBSan)
```bash
cmake -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_ASAN=ON
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

## C Pitfalls Avoided & Production Lessons Learned

| C Pitfall / Concurrency Trap | Architectural Prevention in `cstored` |
| :--- | :--- |
| **Silent Log Corruption on Crash** | Fixed-size 24-byte binary framing with Castagnoli CRC32C checksums. Broken frames or partial writes are detected and truncated to the last transaction boundary. |
| **Data Races in Hash Table Read/Write** | Lock striping with $N=64$ `pthread_rwlock_t` instances. Readers acquire shared locks; writers acquire exclusive stripe locks. Verified clean under ThreadSanitizer. |
| **False Sharing Cache Invalidation** | Stripes are padded to 64 bytes (`_Alignas(64)`) to match modern CPU cache lines, preventing cache ping-pong between distinct CPU cores accessing adjacent locks. |
| **Deadlocks during Dynamic Rehashing** | Two-tier hierarchical lock protocol: all normal operations acquire shared `resize_lock`, then target stripe lock. Resize acquires exclusive `resize_lock`, followed by all stripe locks in strict monotonic ascending order ($0, 1, ..., N-1$), preventing inversion deadlocks. |
| **Use-After-Free in Concurrent Reads** | `kvs_concurrent_ht_get()` deep-copies the value buffer out under the protection of the stripe read lock. Callers never hold dangling pointers to buffers concurrently modified or freed by other threads. |
| **Lost Wakeups & Spurious Wakeups** | Thread pool uses standard POSIX idioms: condition variables waited on within explicit `while (!condition)` loops under acquired mutex lock. |
| **Thread Leaks on Pool Shutdown** | `kvs_thread_pool_destroy()` broadcasts termination to all workers and unconditionally joins (`pthread_join`) every thread. |
| **Double-Free Corruptions** | Every slab chunk header encodes a 64-bit state cookie (`KVS_CHUNK_MAGIC_ALLOCATED` vs `KVS_CHUNK_MAGIC_FREE`). Freeing an already freed chunk returns `KVS_ERR_DOUBLE_FREE`. |
| **Integer Overflow in Size Calculations** | Protected by `kvs_mul_overflow_size()` and `kvs_add_overflow_size()` using compiler builtins (`__builtin_mul_overflow`) before allocations. |
| **Partial write() Call Failures** | Wrapped by `kvs_write_all()` which handles `EINTR` and partial byte deliveries in a loop until buffer is completely transferred. |
