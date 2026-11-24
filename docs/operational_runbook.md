# Production Operational Runbook: `cstored`

This runbook provides infrastructure engineers, Site Reliability Engineers (SREs), and systems architects with the operational guidelines, sizing formulas, durability tuning strategies, and disaster recovery procedures required to run `cstored` in mission-critical production environments.

---

## 1. Capacity Planning & Sizing Formulas

### 1.1 Memory (RAM) Calculation Formula
`cstored` is an in-memory key-value store where data resides primarily in RAM. Memory allocation is split across three distinct segments:

$$\text{Total Memory} = \text{Bucket Array RAM} + \text{Slab Pool RAM} + \text{Payload Buffer RAM}$$

1. **Bucket Array Overhead:**
   * Each bucket pointer consumes 8 bytes (on 64-bit architectures).
   * Capacity $C$ is always a power of two ($C = 2^k$).
   * Table dynamically doubles when load factor reaches $75\%$ ($N \ge 0.75 \times C$).
   $$\text{RAM}_{\text{buckets}} = C \times 8 \text{ bytes}$$
   * *Example:* For 10,000,000 keys, capacity will expand to $16,777,216$ ($2^{24}$) buckets:
     $$16,777,216 \times 8 \text{ bytes} \approx 128 \text{ MB}$$

2. **Slab Memory Pool (Node Overhead):**
   * Each key-value pair is linked via a `kvs_cnode_t` structure.
   * `kvs_cnode_t` size is 48 bytes (cached hash, key pointer, key length, value pointer, value length, next pointer).
   * Slab chunk allocation aligned to 16 bytes with 32-byte tracking header:
     $$\text{Chunk Stride} = 32 + 48 = 80 \text{ bytes}$$
   * For 10,000,000 active nodes:
     $$10,000,000 \times 80 \text{ bytes} \approx 762 \text{ MB}$$

3. **Payload Buffers (Keys + Values):**
   * Average key length $\bar{K}$ bytes, average value length $\bar{V}$ bytes.
   $$\text{RAM}_{\text{payload}} = N \times (\bar{K} + 1 + \bar{V})$$
   * *Example:* For 10,000,000 keys with $\bar{K} = 32$ bytes and $\bar{V} = 128$ bytes:
     $$10,000,000 \times (33 + 128) \text{ bytes} \approx 1.5 \text{ GB}$$

4. **Total Recommended RAM Allocation:**
   $$\text{Total RAM} \approx 128 \text{ MB} + 762 \text{ MB} + 1,536 \text{ MB} \approx 2.43 \text{ GB}$$
   *SRE Sizing Rule:* Provision at least **$1.35\times$** the calculated total memory to accommodate OS page mapping alignment and dynamic resizing headroom.

---

### 1.2 Lock Stripe Sizing Formula
`cstored` employs fine-grained lock striping to eliminate lock contention across worker threads.
* **Default Stripes:** $S = 64$ (`KVS_DEFAULT_STRIPES`).
* **Sizing Guideline:**
  $$S = \max\left(64, 2^{\lceil \log_2(4 \times N_{\text{cores}}) \rceil}\right)$$
* For a 32-core server: $S = 2^{\lceil \log_2(128) \rceil} = 128$ stripes.
* Memory footprint per stripe: 64 bytes (`_Alignas(64)`). 128 stripes consume a negligible 8 KB of RAM while guaranteeing zero lock contention.

---

### 1.3 Thread Pool Sizing Formula
Worker threads handle task dispatching and asynchronous batch mutations.
* **In-Memory Cache Workloads (Read-Heavy / Low Durability):**
  $$N_{\text{threads}} = N_{\text{cores}}$$
  Matches available hardware execution threads to prevent kernel context switching overhead.
* **Mixed Workloads (AOF Persistence / Write-Heavy):**
  $$N_{\text{threads}} = 2 \times N_{\text{cores}}$$
  Permits worker threads to yield during I/O write operations while peer threads continue processing memory reads.

---

## 2. Durability Policy Tuning Matrix

`cstored` provides three distinct durability modes via `kvs_aof_open(filepath, sync_mode, &aof)`:

| Durability Mode | Recovery Point Objective (RPO) | Recovery Time Objective (RTO) | Throughput (Ops/sec) | Recommended Use Case |
| :--- | :--- | :--- | :--- | :--- |
| `KVS_AOF_SYNC_ALWAYS` | **0 (Zero data loss)** | Sub-second (log replay) | $1,000 - 15,000$ (bounded by disk sync IOPS) | Financial ledger transactions, billing state, compliance audit logs. |
| `KVS_AOF_SYNC_EVERYSEC` | **$\le 1.0$ second** | Sub-second (log replay) | $100,000 - 500,000$ | Gaming state, user sessions, shopping carts, telecommunications metrics. |
| `KVS_AOF_SYNC_NO` | Bounded by OS dirty page flushes (typically 30s) | Sub-second (log replay) | $1,000,000+$ (memory speed) | Ephemeral caching, analytics counters, real-time leaderboards. |

---

## 3. Production Telemetry & Incident Response

### 3.1 Live Telemetry Dump (`SIGUSR1`)
In production environments, never attach a debugger (such as GDB) to a running real-time process, as stopping threads can trigger upstream timeouts. `cstored` provides an **async-signal-safe** telemetry dump triggered via standard POSIX signals:

```bash
# Obtain process ID of cstored instance
PID=$(pgrep cstored)

# Send SIGUSR1 without disrupting live traffic
kill -USR1 $PID
```

The process immediately outputs an atomic diagnostic banner to standard error (or its designated metrics descriptor):

```
=======================================================
           cstored RUNTIME TELEMETRY DUMP              
=======================================================
[Metrics & Cache Efficiency]
  Total Operations: 15420300
  GET Operations:   13120000
  PUT Operations:   2100000
  DELETE Operations:200300
  Cache Hits:       12857600
  Cache Misses:     262400
  Hit Rate:         98 %
[Hash Table]
  Active Items:     1899700
[Slab Allocator]
  Active Chunks:    1899700
  Total Chunks:     2097152
  Mapped Memory:    167772160 bytes (160 MB)
[AOF Persistence]
  Total Writes:     2300300
  Bytes Committed:  142890120 bytes (136 MB)
  Syncs Executed:   2300300
=======================================================
```

---

## 4. Disaster Recovery & Maintenance Runbook

### 4.1 Recovering from an Incomplete / Corrupted AOF
If the host server experiences a kernel panic or sudden power loss during an active append, `cstored` automatically self-repairs on restart:

```
[Server Power Restored]
       |
       v
[Application Starts -> kvs_aof_open() -> kvs_aof_recover()]
       |
       +---> Scans binary 24-byte frames sequentially
       +---> Verifies Castagnoli CRC32C for each transaction
       +---> Detects torn trailing write from crash boundary
       +---> Calls ftruncate() to roll back to last committed transaction
       |
       v
[State 100% Restored -> Subsystem Transitions to Live Serving Mode]
```

*Manual Verification Step:*
If an SRE wishes to manually inspect a recovered log file:
```bash
# Check file size before and after automated recovery
ls -la /var/lib/cstored/kvs.aof

# Run standalone crash recovery suite on the target file
./build/debug/bin/test_crash_recovery
```

---

### 4.2 Scheduling Log Compaction (AOF Rewrite)
As `cstored` processes millions of updates and deletes, the historical AOF log expands. Log compaction reduces file size by writing a fresh snapshot containing only current live keys:

```c
/* Execute compaction during low-traffic maintenance window */
kvs_status_t status = kvs_aof_compact(aof, ht);
if (status != KVS_OK) {
    KVS_LOG_E("AOF Compaction failed: %s", kvs_status_str(status));
}
```

*Compaction Guarantees:*
1. Live client traffic is never blocked—snapshotting streams live keys under shared read locks.
2. Compaction writes to `<path>.tmp` first and calls `fdatasync()`.
3. The temporary file replaces the active log using an atomic POSIX `rename()`. A failure or crash during compaction never corrupts the existing log.
