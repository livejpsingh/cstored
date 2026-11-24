# Developer Onboarding & Engineering Guide: `cstored`

This guide establishes the mental models, architectural invariants, and non-negotiable coding standards enforced across `cstored`. All contributors and code reviewers must adhere strictly to these principles.

---

## 1. Toolchain & Development Environment

* **C Standard:** ISO C17 (`-std=c17`, `set(CMAKE_C_STANDARD 17)`).
* **Compiler Flags Enforced:**
  ```
  -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  -Wunused -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes
  -Wmissing-declarations -Wcast-align -Wpointer-arith -fno-common
  ```
* **Required Tooling:**
  * CMake 3.20+ & Ninja build generator
  * GCC 11+ or Clang 14+
  * LLVM tools: `clang-format`, `clang-tidy`
  * Valgrind 3.18+ (for full leak checks and memory profiling)

---

## 2. Core Architectural Invariants

### 2.1 Explicit Error Propagation (`kvs_status_t`)
Never return untyped integers, `-1`, or silently return `NULL` on error paths. Every public API must return a typed `kvs_status_t`:
```c
typedef enum {
    KVS_OK               =  0,
    KVS_ERR_NOMEM        = -1,
    KVS_ERR_INVALID_ARG  = -2,
    KVS_ERR_NOT_FOUND    = -3,
    KVS_ERR_KEY_EXISTS   = -4,
    KVS_ERR_KEY_TOO_LONG = -5,
    KVS_ERR_VAL_TOO_LONG = -6,
    KVS_ERR_OVERFLOW     = -7,
    KVS_ERR_DOUBLE_FREE  = -8,
    KVS_ERR_SYS          = -9
} kvs_status_t;
```
Pointer outputs must be returned via out-parameters (`kvs_status_t func(..., void **out)`). Always mark public functions with `KVS_NODISCARD` (`__attribute__((warn_unused_result))`).

### 2.2 Memory Ownership & Deep-Copy Semantics
1. **No External References Retained:** When inserting into the hash table, `cstored` deep-copies both the key and the value into internal allocator-managed buffers. The caller retains ownership of their input buffer and may free or overwrite it immediately after `put()` returns.
2. **Concurrent Read Safety (Copy-Out):** In `kvs_concurrent_ht_get()`, `cstored` **never** returns a raw pointer into the node payload. Returning raw pointers causes use-after-free bugs if another thread updates or deletes that key. The read path deep-copies the value into a newly allocated buffer that the caller owns and must `free()`.
3. **Exception-Safe Updates:** When updating an existing key, the new value payload is allocated **before** releasing the old value buffer. If the new allocation fails with `KVS_ERR_NOMEM`, the previous state remains intact.

### 2.3 Safe Arithmetic & Pointer Operations
* **Integer Overflows:** Never write `malloc(count * sizeof(T))` or `(size + 15) & ~15` without overflow validation. Use the provided inline helpers in `include/kvs/common.h`:
  * `kvs_mul_overflow_size(a, b, &out)`
  * `kvs_add_overflow_size(a, b, &out)`
  * `kvs_align_up(size, alignment)`
* **Pointer Arithmetic:** Arithmetic on `void *` is a non-standard GCC extension and prohibited by `-Wpointer-arith`. Cast to `uint8_t *` or `uintptr_t` before computing offsets.

---

## 3. Concurrency Mental Model & Locking Hierarchy

### 3.1 Two-Tier Hierarchical Locking
To guarantee zero deadlocks between concurrent mutations and dynamic table expansion, `cstored` enforces a strict two-tier lock ordering:

```
[Tier 1: Global resize_lock (pthread_rwlock_t)]
                     |
                     v (Monotonic single direction)
[Tier 2: Lock Stripe (pthread_rwlock_t)] (Stripe 0 .. Stripe 63)
```

* **Read Operations (`get`, `contains`):**
  1. Acquire shared read: `pthread_rwlock_rdlock(&ht->resize_lock);`
  2. Hash key & determine stripe index: `s = (hash >> 32) & stripe_mask;`
  3. Acquire stripe shared read: `pthread_rwlock_rdlock(&ht->stripes[s].rwlock);`
  4. Traverse bucket chain, copy out value.
  5. Release stripe read lock.
  6. Release resize read lock.

* **Write Operations (`put`, `delete`):**
  1. Acquire shared read: `pthread_rwlock_rdlock(&ht->resize_lock);`
  2. Hash key & determine stripe index: `s = (hash >> 32) & stripe_mask;`
  3. Acquire stripe exclusive write: `pthread_rwlock_wrlock(&ht->stripes[s].rwlock);`
  4. Perform mutation. Check load factor threshold ($N \ge \text{cap} \times \frac{3}{4}$).
  5. Release stripe write lock.
  6. Release resize read lock.
  7. If resize threshold was reached, trigger `kvs_cht_resize(ht)`.

* **Resize Operation (`kvs_cht_resize`):**
  1. Acquire exclusive write on Tier 1: `pthread_rwlock_wrlock(&ht->resize_lock);` (waits for all reads & writes across all stripes to drain).
  2. Acquire all 64 stripe write locks in **strict monotonic ascending order**: $0, 1, 2, \dots, 63$.
  3. Double bucket array, re-link nodes using cached hashes without reallocation.
  4. Release all 64 stripe write locks in reverse order ($63, \dots, 0$).
  5. Release resize write lock.

**Result:** Inversion deadlocks are mathematically impossible.

### 3.2 False Sharing Elimination
`pthread_rwlock_t` instances are wrapped inside `kvs_stripe_t` padded to 64 bytes (`_Alignas(64)`). This prevents CPU cache-line bouncing (MESI invalidations) across cores accessing adjacent stripes.

### 3.3 Thread Pool Synchronization Idioms
* Never use `pthread_detach()`—always store `pthread_t` and invoke `pthread_join()` during shutdown to prevent thread descriptor leaks.
* Wait loops for condition variables must **always** check state predicates inside a `while` loop under mutex protection to guard against spurious wakeups:
  ```c
  while (!pool->shutdown && pool->queue_head == NULL) {
      pthread_cond_wait(&pool->not_empty, &pool->lock);
  }
  ```

---

## 4. Persistence & Crash Invariants (AOF)

1. **Binary Framing:** Every mutation is wrapped in a 24-byte header containing `magic` (`0x414F4631`), `op`, `key_len`, `val_len`, and Castagnoli `crc32c`.
2. **Looping Syscalls:** Always use `kvs_write_all()` and `kvs_read_exact()` to handle `EINTR` and partial socket/file deliveries.
3. **Crash Recovery Boundary:** When scanning records during `kvs_aof_recover()`, the engine tracks the last valid file offset. The moment an unexpected EOF, truncated header, or CRC mismatch is detected, `ftruncate()` is immediately called on the file descriptor to eliminate corrupt trailing bytes.
4. **Compaction Atomic Swap:** Compaction streams current table entries to `<path>.tmp`, executes `fdatasync()`, and replaces the active file using atomic POSIX `rename()`.

---

## 5. Build, Test & Sanitizer Workflow

Always compile and verify across all four build profiles prior to submitting a pull request:

```bash
# 1. Debug Build & Unit Tests
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure

# 2. ThreadSanitizer (TSan) - Verify zero races
cmake -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_TSAN=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure

# 3. AddressSanitizer & UndefinedBehaviorSanitizer (ASan / UBSan)
cmake -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKVS_ENABLE_ASAN=ON
cmake --build build/asan --parallel
ctest --test-dir build/asan --output-on-failure

# 4. Valgrind Full Leak & Origin Tracking
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         ./build/debug/bin/test_concurrent_ht

# 5. Static Analysis
clang-tidy -p build/debug src/*.c tests/*.c -- -Iinclude -Itests/unity
```
