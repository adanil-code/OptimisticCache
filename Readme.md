# High-Performance Optimistic Concurrency Cache

![Platform: Windows | Linux | macOS](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)
![Language: C++17/20](https://img.shields.io/badge/Language-C%2B%2B17%2F20-orange)
![Environment: User & Kernel Mode](https://img.shields.io/badge/Environment-User%20%7C%20Kernel%20Mode-success)

Designed as a high-performance alternative to reader-writer lock–based concurrent maps under heavy contention.

* **Optimistic reads with wait-free fast path** via SeqLock protocol
* **L1-optimized** metadata layout
* **NUMA-aware** sharded design
* **Dual Environment:** User Mode + Windows Kernel Mode support

> ⚠️ This is NOT a general-purpose concurrent hash map.
> It is a fixed-size, set-associative cache with eviction under pressure.

## Table of Contents
1. [The Problem: Lock Contention & Cache Pollution](#1-the-problem-lock-contention--cache-pollution)
2. [Key Architectural Highlights](#2-key-architectural-highlights)
3. [Quick Start](#3-quick-start)
4. [Configuration Modes & Memory Overhead](#4-configuration-modes--memory-overhead)
5. [Core Invariants & Protocol](#5-core-invariants--protocol)
6. [Design Tradeoffs](#6-design-tradeoffs)
7. [When NOT to Use This Cache](#7-when-not-to-use-this-cache)
8. [Known Limitations & Degradation Paths](#8-known-limitations--degradation-paths)
9. [User Mode vs Kernel Mode](#9-user-mode-vs-kernel-mode)
10. [Benchmarks & Scaling Performance](#10-benchmarks--scaling-performance)
11. [Project Structure](#11-project-structure)
12. [Building test code](#12-building-test-code)
13. [Conclusion & Future Hardware Extrapolation](#13-conclusion--future-hardware-extrapolation) 
14. [License](#14-license)

---

## 1. The Problem: Lock Contention & Cache Pollution

Traditional concurrent cache designs degrade under high read concurrency because they:
* Rely on reader-writer locks.
* Generate heavy cache-coherence traffic (cache line bouncing) just to track active readers.

Additionally, colocating large payloads alongside search metadata causes **cache pollution**. When scanning deep collision chains, failed lookups waste vital memory bandwidth fetching payloads that are ultimately discarded.

---

## 2. Key Architectural Highlights

### Not a Generic Map: Set-Associative Cache Behavior
**Important:** This behaves as a fixed-size, set-associative hash cache rather than a dynamically resizing map. It utilizes a strictly fixed bucket size (8 slots per set), performs no collision chaining, and does not dynamically resize. Under capacity or localized collision pressure, it acts as a true cache by forcefully evicting and overwriting existing entries to make room for new ones.

### Sequence Lock (SeqLock) Protocol
Readers execute lookups that are **Lock-free reads with a wait-free fast path when uncontended**. There are:
* **No locks**
* **No atomic Read-Modify-Write (RMW) operations**
* **No shared-state writes**

Validation is handled entirely via a monotonic sequence number: `read sequence` → `read data` → `re-read sequence`.
A read is successful only if the sequence is **even** and **unchanged**. This completely avoids reader tracking, cache line bouncing, and inter-core coherence storms. 
**The monotonically increasing sequence counter prevents ABA-style inconsistencies within the SeqLock validation model without requiring tagged pointers or hazard tracking.**

### Memory Layout Geometry
The cache utilizes a "Mega-Block" flat-array design. Each shard allocates a single, contiguous block of NUMA-pinned memory, physically separating the highly-contended L1 search metadata from the bulky payload data.

    ===============================================================================
                          GLOBAL OPTIMISTIC CACHE
    ===============================================================================
       [ NUMA Node 0 ]             [ NUMA Node 1 ]                 [ NUMA Node N ]
       +-------------+             +-------------+                 +-------------+
       |   Shard 0   |             |   Shard 1   |      ...        |   Shard N   |
       +-------------+             +-------------+                 +-------------+
              |
              |     ===============================================================
              +--->                    SHARD MEMORY LAYOUT (Mega-Block)
                    ===============================================================
                    
                      Hot Array (L1 Search)           Cold Array (Payloads)
                    +-----------------------+       +-----------------------+
      Set 0  -----> | Keys[8] (64B)         | ----> | Contexts[8]           |
      (Hash % Mask) | Sequences[8] (64B)    |       | (512B or 1024B)       |
                    +-----------------------+       +-----------------------+
      Set 1  -----> | Keys[8]               | ----> | Contexts[8]           |
                    | Sequences[8]          |       |                       |
                    +-----------------------+       +-----------------------+
                    | ...                   |       | ...                   |

### Hot / Cold Memory Segregation
As illustrated in the geometry above, metadata and payloads are physically separated to maximize cache efficiency.

**Why it matters:** The search metadata for 8 slots is packed into exactly 128 bytes (two contiguous 64-byte cache lines on x86_64 and ARM64, or a single 128-byte cache line on Apple silicon). Hardware spatial prefetchers typically load these adjacent lines together, allowing the CPU to scan an entire collision set with extreme efficiency. Failed lookups never touch payload memory, strictly preventing L1 cache pollution.

### Sharded Partitioning
The cache is split into power-of-two shards, scaling automatically with the logical processor count. Each shard operates independently, eliminating global lock contention and reducing cross-core interference.

### NUMA-Aware Allocation
Shards are explicitly bound to specific NUMA nodes. By physically distributing memory across available controllers, the architecture improves overall bandwidth utilization and reduces memory contention under high concurrency.

---

## 3. Quick Start

    #include "optimistic_cache.h"

    // 1. Initialize cache (64-bit payload context)
    OptimisticCache<64> cache;

    if (!cache.Initialize(1'000'000))
    {
        // handle allocation failure
    }

    uint64_t key   = 0xDEADBEEF;
    uint64_t value = 42;

    // 2. Insert
    cache.CheckAndInsert(key,
                         value);

    // 3. Optimistic lookup (wait-free fast path when uncontended)
    uint64_t out = 0;

    if (cache.LookupContext(key,
                            out))
    {
        // out == 42
    }

    // 4. Delete
    cache.Delete(key);

---

## 4. Configuration Modes & Memory Overhead

Because the cache does not dynamically resize and utilizes flat pre-allocated arrays, the memory overhead is highly predictable and fixed upfront.

* **`OptimisticCache<0>`** → Hash-set behavior (validates key existence, no payload).
* **`OptimisticCache<64>`** → 64-bit context values (standard).
* **`OptimisticCache<128>`** → 128-bit high-density payloads.

**Memory Overhead Calculation:**
* **Metadata Overhead:** Exactly 16 bytes per slot (128 bytes per 8-slot set).
* **Payload Overhead:** Dependent on the template configuration (`0`, `8`, or `16` bytes per slot).
* Every shard has a fixed hardware-specific padding overhead (64 bytes on x86_64 and ARM64, and 128 bytes on Apple silicon) to prevent false sharing.

---

## 5. Core Invariants & Protocol

### Core Invariants
* A slot is stable **if and only if** its sequence is even and remains unchanged during a read.
* **Writers:** Transition `even → odd → even`. The payload must be published *before* the final sequence store.
* **Readers:** Seamlessly retry on an odd sequence or a sequence mismatch.

### SeqLock Protocol (Formal)

**Reader (Lock-free; wait-free fast path when uncontended)**
1. `seq1 = load(sequence, acquire)`
2. `if odd → retry`
3. `read payload (relaxed)`
4. `seq2 = load(sequence, acquire)`
5. `if mismatch → retry`

**Writer**
1. `CAS even → odd`
2. `write payload (relaxed)`
3. `store odd → even (release)`

### Duplicate Insertion Race Prevention
To guarantee correctness under concurrent insert races and ensure no duplicate keys exist within a set, the following mitigation is used:
* After locking a victim slot, the entire 8-slot set is rescanned.
* If **any** slot currently contains the target key, or if **any** other slot is actively locked (odd sequence), the operation immediately aborts, drops its lock, and retries.

---

## 6. Design Tradeoffs

To achieve extreme read scalability, the following tradeoffs were made:
* **Set-Associative Eviction:** Not a generic map. Hardcapped at 8 slots per set. Once a set is full, it forcibly evicts/overwrites items on pressure. No collision chaining or array resizing occurs. 
Randomized eviction avoids shared replacement metadata (e.g., LRU counters), preventing additional write contention and cache line traffic.
* **Read Retries:** Readers may retry under localized write contention (see SeqLock protocol).
* **Spin-Based Writes:** Writes are optimistic and spin-based, not strictly lock-free.
* **Torn 128-bit Payloads:** 128-bit payloads may tear during concurrent reads, any torn read is detected via sequence mismatch and retried.
* **Workload Bias:** The architecture is heavily optimized for read-dominant workloads.

---

## 7. When NOT to Use This Cache

This architecture enforces strict hardware alignment and spin-based synchronization, making it suboptimal or unsuitable for certain profiles:

* **Write-Heavy Workloads with Sustained Contention:** Since writers utilize spin-based sequence locking, constant writes to the same bucket will generate considerable cache line invalidation traffic and degrade throughput. 
* **Workloads Requiring Strict Fairness or Bounded Retries:** There is no queueing mechanism for waiting threads. 
* **Applications Needing Stable Iteration Order or Full-Map Traversal:** The sharded, set-associative design means iteration order is non-deterministic and highly dependent on active NUMA nodes and hash distribution.
* **Large or Non-Trivially Copyable Payloads:** Payloads are strictly capped at 128-bits. Larger structs cannot be atomically verified against tears via the SeqLock without risking L1 cache pollution.
* **Scenarios Requiring Precise Eviction Policies (LRU/LFU):** Victim selection upon a collision is restricted to the 8-slot bucket and relies on probabilistic/deterministic pseudo-random replacement, not global table-wide metrics.

---

## 8. Known Limitations & Degradation Paths

While designed for high-concurrency environments, this architecture possesses specific degradation paths under adverse conditions. Understanding these limits—and how the architecture empirically mitigates them—is critical for stable deployment:

* **Eviction Thrashing (Statistical Clustering):** Despite the high-entropy SplitMix64 hasher, the strict 8-way set-associative design lacks collision chaining. In rare cases where a burst of keys maps to the exact same 8-slot bucket, the cache will repeatedly overwrite entries, temporarily degrading the hit rate for those specific keys.

* **Targeted Write Contention (MESI Bus Flooding):** The optimistic SeqLock shifts contention cost to readers, which may starve under heavy writes. If a workload heavily mutates the *exact same key* or bucket simultaneously, the atomic compare-and-swap operations will generate massive cache line invalidation traffic. Writers will spin, and readers will be forced into continuous retry loops, risking temporary starvation. 
    * *Empirical Note (Global Write Resilience):* While *targeted* writes degrade performance, benchmarks demonstrate that *global* write-heavy workloads (e.g., a 10% Read / 90% Write split uniformly distributed across the key space) do not trigger this collapse. The combination of `SplitMix64` avalanche mixing and high-capacity sharding successfully isolates contention to individual hardware-aligned buckets. This allows the cache to maintain linear scaling and nanosecond tail latencies, outperforming standard global mutexes even well outside its ideal read-heavy domain.

* **Preemption & Starvation Dynamics (Environment Dependent):** SeqLock relies on spin-based polling, making thread scheduling critical:
    * *User Mode:* If a writer is preempted while holding a slot in the “odd” (write-in-progress) state, forward progress on that slot stalls. All readers will fail validation and enter retry loops, creating a temporary read blackout for that entry. Under contention, this can amplify into elevated CPU usage and increased cache-coherence traffic. To mitigate livelock and excessive spinning, the implementation employs ExecuteTieredBackoff, progressively degrading from hardware pause instructions to scheduler yields and short sleeps.
    * *Kernel Mode:* Prevents preemption by raising the IRQL to `DISPATCH_LEVEL` during writes. However, if a thread *already* at `DISPATCH_LEVEL` encounters extreme targeted write contention, it risks hard spinning and triggering a DPC watchdog timeout (Bugcheck `0x133`). Threads calling from `< DISPATCH_LEVEL` are safe as they properly lower their IRQL before yielding.

* **128-bit Payload Tearing Retries:** `OptimisticCache<128>` uses two separate 64-bit relaxed atomic loads. While the overarching SeqLock prevents corrupted data from "torn reads", a high volume of concurrent writes to a 128-bit slot will force readers to loop and retry more frequently than in the 64-bit configuration.

* **No Backpressure or Fairness Mechanism** The architecture intentionally avoids locks, queues, and coordination structures. As a result:
    * There is no fairness between threads
    * No prioritization of older operations
    * No mechanism to throttle or shed load under pressure

    Under overload or hotspot contention, the system does not degrade gracefully—it continues to spin and retry, potentially increasing CPU utilization without making proportional forward progress.

---

## 9. User Mode vs Kernel Mode

Both implementations share identical geometry and architecture, but adapt to their respective execution constraints.

| Feature                          | User Mode (`optimistic_cache.h`)      | Kernel Mode (`OptimisticCache.h`)           |
| :------------------------------- | :------------------------------------ | :------------------------------------------ |
| **Primitives** | C++20 `<atomic>`                      | Explicit hardware memory barriers           |
| **Backoff** | Scheduler-based yielding              | IRQL-aware scheduling yields                |
| **Memory** | Pageable virtual memory               | Non-paged pool memory                       |
| **Write Integrity** | Standard thread preemption            | `DISPATCH_LEVEL` write guarantees           |
| **Eviction / Victim Selection** | Probabilistic (`FastThreadLocalPRNG`) | Deterministic (`Hasher(Key + Retries) & 7`) |

**Eviction Logic Note:** Generating PRNG state without floating-point registers in kernel mode is complex. To rotate victims securely under pressure without relying on thread-local state, the KM table relies on a deterministic fallback hash retry, whereas the UM table utilizes a rapid thread-local PRNG to probabilistically select victims and mitigate collision storms.

---

## 10. Benchmarks & Scaling Performance

To validate the architecture, the Optimistic Cache was benchmarked against a standard implementation (`std::unordered_map` protected by `std::shared_mutex`) across three distinct hardware topologies:

* **Intel Core i7-1165G7** (4 Cores / 8 Threads, Low Power Mobile)
* **Intel Core i7-8086K** (6 Cores / 12 Threads, High Clock Desktop)
* **Intel Core i7-12700H** (14 Cores / 20 Threads, Big.LITTLE Hybrid)

### Test Methodology
* **Key Distribution:** Uniform random via `std::mt19937_64` feeding into the internal `SplitMix64` avalanche mixer.
* **Load Factor:** ~50% (Table pre-populated with 65,000 items in a 131,072 capacity cache).
* **Access Patterns:** Tested across 80/20 (Read-Heavy), 50/50 (Mixed Contention), and 95/5 (Highly Skewed) read/write profiles. 

*Note: All representative metrics below are based on the `<64-bit>` payload context configuration.*

### Multi-Threaded Scaling (The "Lock Convoy" Collapse)
Under an 80/20 Read-Heavy workload, the Optimistic Cache scales efficiently with physical hardware cores. Conversely, the standard library implementation exhibits negative scaling under high contention; as thread counts increase, global lock contention causes total throughput to plummet below single-threaded baseline speeds.

| Implementation       | i7-1165G7 (Mobile, 8-Thread) | i7-8086K (Desktop, 12-Thread) | i7-12700H (Hybrid, 20-Thread) |
| :------------------- | :--------------------------- | :---------------------------- | :---------------------------- |
| **Std: Map+Mutex** | 0.32x (Negative Scaling)     | 0.44x (Negative Scaling)      | 0.28x (Negative Scaling)      |
| **Lock-Free Cache** | **3.04x** (at 8 threads)     | **6.94x** (at 12 threads)     | **8.79x** (at 20 threads)     |

### Predictable Tail Latency (P99.9, P99.99)
At extreme percentiles, the true cost of OS-mediated locks becomes apparent. At the 99.9th percentile (approaches the algorithmic minimum latency of the lookup path), the wait-free SeqLock protocol maintains nanosecond-level latency, avoiding the severe context-switch penalties of standard locking mechanisms. 

**Metric: P99.9 Latency**
| Hardware Topology       | Std: Map+Mutex | Lock-Free Cache | Stability Advantage    |
| :---------------------- | :------------- | :-------------- | :--------------------- |
| **i7-1165G7 (Mobile)** | 207,514 ns     | **179 ns** | **1,159x More Stable** |
| **i7-8086K (Desktop)** | 236,869 ns     | **107 ns** | **2,213x More Stable** |
| **i7-12700H (Hybrid)** |  591,390 ns     | **231 ns** | **2,560x More Stable** |

**Metric: P99.99 Latency (OS Wait Starvation)**
At the 99.99th percentile, standard locks frequently stall for over a millisecond as threads are descheduled. The Optimistic Cache's tiered hardware backoff and wait-free reads significantly reduces tail latency variance.

| Hardware Topology       | Std: Map+Mutex | Lock-Free Cache | Stability Advantage    |
| :---------------------- | :------------- | :-------------- | :--------------------- |
| **i7-1165G7 (Mobile)** | 656,625 ns     | **3,278 ns** | **200x More Stable** |
| **i7-8086K (Desktop)** | 553,988 ns     | **246 ns** | **2,251x More Stable** |
| **i7-12700H (Hybrid)** | 1,476,190 ns   | **301 ns** | **4,904x More Stable** |

### Total Throughput Speedup (Mixed Contention)
Under heavy 50/50 mixed workloads (simultaneous reads, inserts, and aggressive evictions), the architectural differences create a significant performance gap. As core counts scale up, the standard table collapses under its own synchronization weight, while the Optimistic Cache leverages its sharded layout and wait-free reads to achieve high throughput.

| System Profile                  | Lock-Free Ops/Sec | Std Ops/Sec       | Total Speedup     |
| :------------------------------ | :---------------- | :---------------- | :---------------- |
| **Mobile (4-Core/8-Thread)** | 89.2 Million      | 6.4 Million       | **~13.7x Faster** |
| **Desktop (6-Core/12-Thread)** | 207.0 Million     | 8.0 Million       | **~25.7x Faster** |
| **Hybrid (14-Core/20-Thread)** | 253.9 Million     | 5.9 Million       | **~42.6x Faster** |

### Reproducing Benchmarks
All benchmarks were conducted on Windows 10/11 using the `test_um` harness included in this repository, compiled with Microsoft Visual Studio 2026. The corresponding MSVC project files are provided for full reproducibility.

---

## 11. Project Structure

The repository is organized into distinct layers to separate the core cache logic from the environment-specific test suites:

* **`km/`**: Contains the **Kernel-Mode** Optimistic Cache implementation.
    * `OptimisticCache.h`: The native WDK C++17 header designed strictly for Windows Kernel development.

* **`um/`**: Contains the **User-Mode** Optimistic Cache implementation.
    * `optimistic_cache.h`: The cross-platform C++20 header for Linux, macOS, and Windows applications.

* **`test_km/`**: Test code for verifying kernel-mode logic within a user-mode environment.
    * `TestOptimisticCacheKm/cpp`: Driver logic test harnesses utilizing simulated IRQL conditions.

* **`test_um/`**: Cross-platform user-mode performance test suite.
    * `test_optimistic_lock.cpp`: The primary validation and benchmarking suite.

* **`test_common/`**: Shared test logic used by both kernel and user-mode performance tests.
   * `std_cache.h`: A wrapper for standard library comparisons.
   * `test_optimistic_cache_common.h`: Shared performance tests and validation logic.

* **`test_drv/`**: Actual Windows Kernel driver performance test code for deployment on target systems.
    * `TestOptCacheDrv.cpp`: Windows WDM test driver code.

---

## 12. Building test code

### Linux / macOS
Requires a C++20 compliant compiler (GCC or Clang).

*Linux Dependencies:*
The Linux user-mode implementation utilizes libnuma to bind shard allocations to physical CPU sockets, mimicking the memory controller routing of the Windows kernel implementation. You must install the NUMA development headers before building:

```bash
# Ubuntu / Debian
sudo apt-get update
sudo apt-get install libnuma-dev numactl

# RHEL / Fedora / CentOS
sudo dnf install numactl-devel
```

*Using the Build Script (Recommended)*

./build.sh [--clean | -c] [--type Release | Debug] [--compiler g++ | clang++]

```text
Defaults:
 - Build type: Release
 - Compiler: g++
 - Reuses existing build/

Options:
-c, --clean → Remove build/ before building
-t, --type  → Set build type (Release or Debug)
--compiler  → Choose compiler (g++ or clang++)
-h, --help  → Show help and exit
```

```bash
#Set permissions (once):
chmod +x ./build.sh

# Standard Release build using default C++ compiler
./build.sh

# Clean Release build using clang++ (Optimized for macOS or specific Linux tests)
./build.sh --clean --type Release --compiler clang++

# Debug build for troubleshooting
./build.sh --clean --type Debug
```

*Using CMake*
The included CMakeLists.txt automatically detects your platform, handles libnuma linking on Linux, and configures optimized build flags.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j $(nproc)
```

*Manual Compilation*
If you prefer building without CMake, ensure you include the -pthread and -lnuma (Linux only) flags for proper linking.

```bash
# GCC (Linux)
g++ -std=c++20 -O3 -march=native -flto=auto -funroll-loops -fomit-frame-pointer -fno-rtti -fexceptions -pthread \
    -I./um -I./test_um -I./test_common test_um/test_optimistic_lock.cpp -o bin/test_optimistic_lock -lnuma
	
# Clang (Linux/macOS - omit -lnuma on Mac)
clang++ -std=c++20 -O3 -march=native -flto -funroll-loops -fomit-frame-pointer -fno-rtti -fexceptions -pthread \
    -I./um -I./test_um -I./test_common test_um/test_optimistic_lock.cpp -o bin/test_optimistic_lock -lnuma
```

**Recommended Step:** Running the Benchmark on Linux
To ensure the test suite can accurately benchmark tail latencies, apply NUMA node affinity, and manage thread priorities without requiring full root privileges, it is highly recommended to grant the `test_optimistic_lock` executable the CAP_SYS_NICE capability before execution.

```bash
# Ubuntu / Debian
sudo apt install libcap2-bin 

# RHEL / Fedora / CentOS
sudo dnf install libcap          

sudo setcap cap_sys_nice+ep ./build/bin/test_optimistic_lock
```

### Windows (User-Mode & Kernel-Mode)
Native Visual Studio Solution (.slnx/.sln) and Project (.vcxproj) files are included in the repository.

**User-Mode** Build using Visual Studio 2022 or later.
* *Note:* If building with Visual Studio 2022, you must manually change the Platform Toolset to `v143` in the project properties.
* Requires C++20.
* NUMA support is handled natively via VirtualAllocExNuma.

**Kernel-Mode:** Build using Visual Studio 2022 and the Windows Driver Kit (WDK 11).
* Requires C++17.
* The implementation uses explicit hardware memory barriers and operates safely at `DISPATCH_LEVEL` for writers.

---

## 13. Conclusion & Future Hardware Extrapolation

Per Amdahl's Law, traditional reader-writer locks are fundamentally bottlenecked by their need to track active readers, creating a sequential synchronization point that generates massive cache-invalidation traffic (MESI bus floods) across physical cores. By utilizing a wait-free Sequence Lock protocol, this Optimistic Cache eliminates read-side memory writes entirely. Readers act as pure observers, allowing throughput to scale linearly with parallel read loads.

Based on this architecture's mechanical sympathy—specifically its Hot/Cold memory segregation and zero-write read paths—the performance advantages will compound on emerging and future hardware topologies:

* **Massive L3 Caches (AMD 3D V-Cache / Server CPUs):** The L1-optimized Hot Sets pack 8 slots of search metadata into exactly 128 bytes (two adjacent cache lines). With the proliferation of massive L3 SRAM on modern server chips, entire metadata shards can remain permanently resident in cache. Failed lookups or hash collision scans will complete at L1/L3 speeds without ever touching main memory or fetching bulky payloads.

* **High-Density NUMA & Many-Core (EPYC / Xeon / Graviton):** Because optimistic readers never execute atomic RMW (Read-Modify-Write) instructions, they generate **zero** cross-die cache coherency traffic. On high-core-count NUMA systems where inter-core communication latency is the primary bottleneck, this allows read-heavy workloads to scale near-perfectly as physical cores are added.

* **ARM64 & Weak Memory Models (Apple Silicon / Cloud Native ARM):** The architecture relies on precise C++20 `std::memory_order_acquire`/`release` fences rather than implicit x86 Total Store Order (TSO) guarantees. This ensures that the wait-free fast path will scale efficiently and safely on high-core ARM server processors without risking torn reads or pipeline stalls. 

* **Asymmetric / Hybrid Architectures (Intel Big.LITTLE):** The tiered backoff system (transitioning from hardware pauses to scheduler yields and hard sleeps) is specifically designed to handle the thread-scheduling volatility of P-Core/E-Core topologies. If a writer is preempted on a slower E-Core, contending threads gracefully degrade their spin loops rather than burning power and monopolizing P-Core resources in a livelock.

---

## 14. License
This project is licensed under the Apache License, Version 2.0. 

You may not use this file except in compliance with the License. You may obtain a copy of the License at:
[http://www.apache.org/licenses/LICENSE-2.0](http://www.apache.org/licenses/LICENSE-2.0)

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, 
either express or implied. See the `LICENSE` file for the specific language governing permissions and limitations.