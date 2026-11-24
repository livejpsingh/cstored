# cstored: Production-Grade Concurrent In-Memory Key-Value Store

A high-performance, concurrent, in-memory key-value store with slab-allocated memory pools, cache-conscious hash tables, fine-grained lock striping, a fixed-worker thread pool, persistent write-ahead logging (AOF) with CRC32C crash recovery, zero-allocation structured logging, and async-signal-safe runtime telemetry (`SIGUSR1`), built in C17 targeting Linux/POSIX environments.

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

### How `cstored` Solves It (All 5 Phases 100% Complete)

`cstored` provides a **provably safe, high-performance concurrent core** that eliminates the most common technical root causes of production failure.

| Business Risk | Technical Root Cause | `cstored` Solution (Implemented) | Business Outcome |
| :--- | :--- | :--- | :--- |
| **Revenue loss from downtime** | Crashes from heap corruption, double-frees, memory leaks, thread descriptor leaks | Custom slab allocator with **in-band 64-bit state cookies**; thread pool with **guaranteed join on shutdown** | Higher availability and strictly zero memory/thread leaks |
| **Poor UX from high latency** | A single global lock serializing all reads and writes across worker threads | **64-way lock striping** with cache-line padding (`_Alignas(64)`) for true parallel reads and writes | Predictable sub-millisecond tail latency ($P_{99.9}$) at scale |
| **Data corruption & compliance risk** | Data races and use-after-free under concurrent mutation | **ThreadSanitizer-verified** zero races; **deep-copy-on-read** prevents dangling pointers | Data integrity and compliance by design |
| **Deadlocks under load** | Lock-order inversion during dynamic hash table resizing | **Two-tier hierarchical lock protocol** (shared resize lock $\to$ stripe lock; resize acquires all stripes in strict ascending order) | Provably deadlock-free operations |
| **Data loss on server panic / kill -9** | In-memory writes discarded when process aborts or container is restarted | **Append-Only File (AOF)** with 24-byte binary framing, Castagnoli **CRC32C checksums**, and automated trailing partial-write truncation | Durability by default (RPO = 0 with `SYNC_ALWAYS`); automatic crash self-repair |
| **Production blindness & silent degradations** | Lack of real-time introspection into memory fragmentation and cache efficiency | **Async-signal-safe `SIGUSR1` telemetry dump** without `malloc` deadlocks; zero-allocation structured logging | Instant observability and live telemetry during production incidents |
| **High cloud infra / maintenance cost** | Memory fragmentation from millions of small allocations + external cluster overhead | Slab allocator groups allocations into contiguous OS pages (`mmap`); zero third-party dependencies | Lower Total Cost of Ownership (TCO), reduced RAM footprint |

---

### Value Proposition for Engineering Leadership

For engineering leadership and infrastructure architects, `cstored` offers:

* **A Foundational In-Process Building Block:** Embed a trusted, high-performance KV core directly into your C/C++ backend binary with zero external dependencies, no licensing fees, and no daemon sidecars.
* **Proactive Risk Mitigation:** Rigorous sanitizer validation (ASan, UBSan, TSan) and Valgrind leak checking enforced in automated CI reduce production incident risk to near-zero.
* **Linear Performance Headroom:** 64-way fine-grained lock striping scales throughput near-linearly with CPU cores, protecting system performance as traffic scales.
* **Audit & Durability Compliance:** Checksummed write-ahead logging guarantees crash survivability, meeting enterprise disaster recovery standards.
* **Production-Grade Observability:** Inspect memory maps, cache hit ratios, and persistence health live without stopping or perturbing worker threads.

---

### Roadmap Status: All 5 Phases Complete

* [x] **Phase 1 — Memory Engine & Hash Table:** Slab pool allocator, 64-bit MurmurHash3 table with $O(1)$ dynamic rehashing, strict compiler warnings, and Unity test harness.
* [x] **Phase 2 — Concurrency & Scalability:** Fixed-worker thread pool, 64-way cache-line padded lock striping, concurrent client stress tests, and ThreadSanitizer validation.
* [x] **Phase 3 — Durability & Crash Recovery:** Binary AOF persistence, Castagnoli CRC32C, atomic `.tmp` log compaction, and resilience tests surviving `kill -9`.
* [x] **Phase 4 — Observability & Hardening:** Async-signal-safe `SIGUSR1` telemetry dump (memory, cache hit rate, log ops), zero-allocation structured logger, and adversarial fault-injection harness.
* [x] **Phase 5 — Production Sign-Off & Verification:** Production operational runbook, 12-pitfall systems engineering catalog, and formal compliance audit against all 7 non-negotiable standards.

---

## Documentation Suite

Detailed architectural, operational, and engineering guides are located in the [`docs/`](file:///c:/workspace/public/cstored/docs/) directory:

* [**Business Understanding & Executive Brief**](file:///c:/workspace/public/cstored/docs/business_understanding.md): Strategic alignment, market positioning, TCO analysis, and business impact.
* [**Developer Onboarding & Engineering Guide**](file:///c:/workspace/public/cstored/docs/developer_guide.md): Mental models, invariants, two-tier locking rules, memory ownership, and coding standards.
* [**Architecture Deep-Dive & System Specifications**](file:///c:/workspace/public/cstored/docs/architecture.md): Component diagrams, binary framing layouts, CRC32C specifications, and crash recovery proofs.
* [**Production Operational Runbook**](file:///c:/workspace/public/cstored/docs/operational_runbook.md): Sizing formulas (RAM, lock stripes, threads), durability tuning matrix, and disaster recovery runbooks.
* [**Lessons Learned & Systems Pitfalls Avoided**](file:///c:/workspace/public/cstored/docs/lessons_learned.md): Deep-dive into 12 critical C hazards (hardware traps, unaligned reads, false sharing, signal deadlocks, torn writes).
* [**Production Standards Compliance Audit**](file:///c:/workspace/public/cstored/docs/production_audit.md): Formal compliance verification proving 100% adherence to all 7 non-negotiable production standards.

---

## Architecture Overview

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
          |                  |                  |                 |
(Node Allocations)    (Value Buffers)   (Durable Writes)   (Telemetry)
          v                  v                  v                 v
+-------------------+ +---------------+ +----------------+ +---------------+
|  Slab Allocator   | |  Heap / mmap  | |  AOF Engine    | |  Telemetry &  |
|  (kvs_slab_t)     | +---------------+ |  (kvs_aof_t)   | |  SIGUSR1 Dump |
|  - Fixed pools    |                   | - 24B Framing  | | - Lock-free   |
|  - Double-free ck |                   | - Castagnoli   | | - No malloc   |
+-------------------+                   | - Auto-repair  | | - Safe write  |
                                        +----------------+ +---------------+
                                                |                 ^
                                                v                 |
                                        +----------------+ [kill -USR1 <pid>]
                                        | Disk Storage   |
                                        | (kvs.aof)      |
                                        +----------------+
```

---

## Live Observability: Async-Signal-Safe `SIGUSR1` Telemetry

Send `SIGUSR1` to the `cstored` process at any time to dump a non-disruptive, atomic diagnostic report directly to standard error or a configured metrics descriptor:

```bash
kill -USR1 <pid>
```

Output:
```
=======================================================
           cstored RUNTIME TELEMETRY DUMP              
=======================================================
[Metrics & Cache Efficiency]
  Total Operations: 5000000
  GET Operations:   4200000
  PUT Operations:   750000
  DELETE Operations:50000
  Cache Hits:       4050000
  Cache Misses:     150000
  Hit Rate:         96 %
[Hash Table]
  Active Items:     700000
[Slab Allocator]
  Active Chunks:    700000
  Total Chunks:     1048576
  Mapped Memory:    134217728 bytes
[AOF Persistence]
  Total Writes:     800000
  Bytes Committed:  48291040
  Syncs Executed:   800000
=======================================================
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

### 2. Run Complete Test Suite (11 Test Suites)
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

### 5. Valgrind Full Leak & Error Check
```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_fault_injection

valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_signal_handler

valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_log
```
