/*
* Apache Optimistic Cache Test/Sample
* Copyright 2026 Alexander Danileiko
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at:
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* This software is provided on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS
* OF ANY KIND, either express or implied.
*/

#pragma once

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <functional>
#include <fstream>

// --- Platform & Architecture Specific ---
#if defined(_WIN32)
    // Windows (MSVC, Clang on Windows, MinGW)
#include <windows.h>
#include <intrin.h>
#else
    // POSIX (Linux, macOS)
#include <sys/resource.h>

// x86/x64 specific intrinsics for GCC/Clang
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#include <cpuid.h>
#endif

// macOS specific
#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#endif

#include "std_cache.h"

// ----------------------------------------------------------------------------
// Test Configuration
// ----------------------------------------------------------------------------
#ifdef KERNEL_TEST
template <size_t TContextSize>
using CacheUnderTest = COptimisticCache<TContextSize>;
#else
template <size_t TContextSize>
using CacheUnderTest = OptimisticCache<TContextSize>;
#endif

// ----------------------------------------------------------------------------
// Utility & Macros
// ----------------------------------------------------------------------------
#define TEST_REQUIRE(condition, msg, retVal)                                                     \
    do                                                                                           \
    {                                                                                            \
        if (!(condition))                                                                        \
        {                                                                                        \
            std::cerr << "    [!] TEST FAILED: " << (msg) << " (Line " << __LINE__ << ")\n";     \
            return retVal;                                                                       \
        }                                                                                        \
    } while (0)


// ----------------------------------------------------------------------------
// Utility
// ----------------------------------------------------------------------------
inline void SetHighPriority()
{
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, -10);
#endif
}

// ----------------------------------------------------------------------------
// Returns CPU namestring for informational purposes
// ----------------------------------------------------------------------------
inline std::string cpu_name()
{
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    int cpuInfo[4] = { 0 };
    char brand[0x40];
    std::memset(brand, 0, sizeof(brand));

#if defined(_MSC_VER)
    __cpuid(cpuInfo, 0x80000000);
    unsigned int nExIds = cpuInfo[0];

    if (nExIds >= 0x80000004)
    {
        __cpuid(cpuInfo, 0x80000002);
        std::memcpy(brand, cpuInfo, sizeof(cpuInfo));

        __cpuid(cpuInfo, 0x80000003);
        std::memcpy(brand + 16, cpuInfo, sizeof(cpuInfo));

        __cpuid(cpuInfo, 0x80000004);
        std::memcpy(brand + 32, cpuInfo, sizeof(cpuInfo));
    }
#else
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;

    __asm__ __volatile__("cpuid" : "=a"(eax) : "a"(0x80000000) : "ebx", "ecx", "edx");

    if (eax >= 0x80000004)
    {
        unsigned int data[12];

        for (unsigned int i = 0; i < 3; ++i)
        {
            __asm__ __volatile__("cpuid"
                : "=a"(data[i * 4 + 0]),
                "=b"(data[i * 4 + 1]),
                "=c"(data[i * 4 + 2]),
                "=d"(data[i * 4 + 3])
                : "a"(0x80000002 + i));
        }

        std::memcpy(brand, data, sizeof(data));
    }
#endif
    return std::string(brand);

#elif defined(__APPLE__)
    char buf[256];
    size_t size = sizeof(buf);

    if (sysctlbyname("machdep.cpu.brand_string", &buf, &size, NULL, 0) == 0)
    {
        return std::string(buf);
    }

    size = sizeof(buf);

    if (sysctlbyname("hw.model", &buf, &size, NULL, 0) == 0)
    {
        return std::string(buf);
    }

    return "Unknown CPU";

#elif defined(__linux__)
    std::ifstream f("/proc/cpuinfo");
    std::string line;

    while (std::getline(f, line))
    {
        if (line.find("model name") != std::string::npos ||
            line.find("Hardware") != std::string::npos)
        {
            std::size_t pos = line.find(':');

            if (pos != std::string::npos)
            {
                return line.substr(pos + 2);
            }
        }
    }

    return "Unknown CPU";
#else
    return "Unknown CPU";
#endif
}

// ----------------------------------------------------------------------------
// Cross-Platform Cycle Counter
// ----------------------------------------------------------------------------
inline uint64_t GetTestHardwareTickCount()
{
#if defined(_WIN32) || defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#else
    return 0; // Fallback for platforms without direct counter access
#endif
}

// ----------------------------------------------------------------------------
// Initialization Abstraction
// Safely bridges the differing return types of COptimisticCache (BOOLEAN),
// OptimisticCache (Status enum), and StdCache (bool).
// ----------------------------------------------------------------------------
template <typename TCache>
bool InitializeCache(TCache& cache,
    size_t  capacity)
{
#ifdef KERNEL_TEST
    if constexpr (std::is_same_v<TCache, COptimisticCache<0>> || std::is_same_v<TCache, COptimisticCache<64>> || std::is_same_v<TCache, COptimisticCache<128>>)
    {
        return cache.Initialize(capacity) != 0;
    }
    else
#endif
    {
        return static_cast<bool>(cache.Initialize(capacity));
    }
}

// ----------------------------------------------------------------------------
// Metric Structures
// ----------------------------------------------------------------------------
struct LatencyMetrics
{
    uint64_t P50;
    uint64_t P90;
    uint64_t P99;
    uint64_t P99_9;
    uint64_t P99_99;
};

// ----------------------------------------------------------------------------
// Pseudo random number generator
// ----------------------------------------------------------------------------
class FastRng
{
private:
    uint32_t m_state;

public:
    FastRng(uint32_t seed) : m_state(seed ? seed : 0xBADF00D)
    {}

    uint32_t Next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;

        return m_state;
    }
};

// ----------------------------------------------------------------------------
// Context Payload Generators & Validation
// ----------------------------------------------------------------------------
template <size_t TContextSize>
struct TestContextTraits;

template <>
struct TestContextTraits<0>
{
    static inline Context0 Make(uint64_t value)
    {
        return {};
    }

    static inline bool IsMatch(Context0 ctx, uint64_t expectedValue)
    {
        return true;
    }

    static inline bool IsTorn(Context0 ctx)
    {
        return false;
    }
};

template <>
struct TestContextTraits<64>
{
    static inline uint64_t Make(uint64_t value)
    {
        return value;
    }

    static inline bool IsMatch(uint64_t ctx, uint64_t expectedValue)
    {
        return ctx == expectedValue;
    }

    static inline bool IsTorn(uint64_t ctx)
    {
        (void)ctx;
        return false;
    }
};

template <>
struct TestContextTraits<128>
{
    static inline Context128 Make(uint64_t value)
    {
        Context128 ctx;
        ctx.Low  = value;
        ctx.High = ~value;
        return ctx;
    }

    static inline bool IsMatch(const Context128& ctx, uint64_t expectedValue)
    {
        if (ctx.Low != expectedValue)
        {
            return false;
        }

        if (ctx.High != ~expectedValue)
        {
            return false;
        }

        return true;
    }

    static inline bool IsTorn(const Context128& ctx)
    {
        return ctx.High != ~ctx.Low;
    }
};

inline double EstimateRdtscFrequency()
{
    uint64_t startTicks = GetTestHardwareTickCount();
    auto     timeStart = std::chrono::high_resolution_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    uint64_t endTicks = GetTestHardwareTickCount();
    auto     timeEnd = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::nano> elapsedNs = timeEnd - timeStart;

    return static_cast<double>(endTicks - startTicks) / elapsedNs.count();
}

// ----------------------------------------------------------------------------
// 1. Correctness Tests
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunCorrectnessTests()
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running Correctness Tests...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;

    TEST_REQUIRE(InitializeCache(cache, 1024), "Initialize failed", false);

    ContextType ctx1 = TestContextTraits<TContextSize>::Make(100);

    // Test Sentinel Key Failure Rejection
    auto failedResult = cache.CheckAndInsert(0, ctx1);
    TEST_REQUIRE(failedResult == CacheUnderTest<TContextSize>::InsertResult::Failed, "Add with sentinel key 0 should return Failed", false);

    // Test Fresh Insertion
    auto insertResult = cache.CheckAndInsert(10, ctx1);
    TEST_REQUIRE(insertResult == CacheUnderTest<TContextSize>::InsertResult::Inserted, "Initial Add should return Inserted", false);

    if constexpr (TContextSize > 0)
    {
        ContextType outCtx;
        TEST_REQUIRE(static_cast<bool>(cache.LookupContext(10, outCtx)), "Lookup failed", false);
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 100), "Context data corrupted", false);
    }
    else
    {
        TEST_REQUIRE(cache.Contains(10), "Contains failed", false);
    }

    std::cout << "    [-] Testing Overwrite policies...\n";
    std::cout << std::flush;

    ContextType ctx2 = TestContextTraits<TContextSize>::Make(200);
    ContextType existingCtx;

    // Test Overwrite Logic
    insertResult = cache.CheckAndInsert(10,
                                        ctx2,
                                        CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                        &existingCtx);

    TEST_REQUIRE(insertResult == CacheUnderTest<TContextSize>::InsertResult::Updated, "Overwrite Add should return Updated", false);

    if constexpr (TContextSize > 0)
    {
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(existingCtx, 100), "Failed to capture existing context", false);

        ContextType outCtx;
        TEST_REQUIRE(static_cast<bool>(cache.LookupContext(10, outCtx)), "Lookup after overwrite failed", false);
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 200), "Overwrite context data corrupted", false);
    }

    std::cout << "    [-] Testing Delete logic and Output Capture...\n";
    std::cout << std::flush;

    ContextType deletedCtx;
    TEST_REQUIRE(static_cast<bool>(cache.Delete(10, &deletedCtx)), "Delete existing failed", false);

    if constexpr (TContextSize > 0)
    {
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(deletedCtx, 200), "Deleted context mismatch", false);

        ContextType outCtx;
        TEST_REQUIRE(!cache.LookupContext(10, outCtx), "Lookup succeeded after delete", false);
    }
    else
    {
        TEST_REQUIRE(!cache.Contains(10), "Contains succeeded after delete", false);
    }

    TEST_REQUIRE(!cache.Delete(999), "Delete non-existent should fail", false);

    std::cout << "    [-] Testing Eviction output capture...\n";
    std::cout << std::flush;

    uint64_t evictedKey = 0;
    ContextType evictedCtx;
    bool evictionOccurred = false;

    // Force an eviction by over-populating the cache
    for (uint64_t i = 1000; i < 15000; ++i)
    {
        auto res = cache.CheckAndInsert(i,
                                        TestContextTraits<TContextSize>::Make(i),
                                        CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                        nullptr,
                                        &evictedKey,
                                        &evictedCtx);
        if (evictedKey != 0)
        {
            evictionOccurred = true;

            if constexpr (TContextSize > 0)
            {
                TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(evictedCtx, evictedKey), "Evicted context data corrupted", false);
            }

            break;
        }
    }

    TEST_REQUIRE(evictionOccurred, "Failed to force an eviction for testing", false);

    std::cout << "[+] Correctness Tests Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 2. Heavy Population & Eviction Tests
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunEvictionTest()
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running Heavily Populated (Eviction) Tests...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;
    TEST_REQUIRE(InitializeCache(cache, 256), "Initialize failed", false);

    std::cout << "    [-] Inserting 10,000 items into 256 capacity cache...\n";
    std::cout << std::flush;

    for (uint64_t i = 1; i <= 10000; ++i)
    {
        ContextType ctx = TestContextTraits<TContextSize>::Make(i);
        cache.CheckAndInsert(i, ctx);
    }

    if constexpr (TContextSize > 0)
    {
        ContextType outCtx;
        bool foundLast = static_cast<bool>(cache.LookupContext(10000, outCtx));

        if (foundLast)
        {
            TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 10000), "Eviction corrupted context", false);
        }
    }
    else
    {
        cache.Contains(10000); // Verify it doesn't crash on hit/miss
    }

    std::cout << "[+] Eviction Tests Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 3. ABA Correctness Test (A-B-A Race Condition)
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunAbaCorrectnessTest(int secondsToRun)
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running ABA (A-B-A) Race Condition Test (" << secondsToRun << " seconds)...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;
    TEST_REQUIRE(InitializeCache(cache, 2048), "Initialize failed", false);

    uint64_t targetKey = 42;
    ContextType ctxA = TestContextTraits<TContextSize>::Make(0xAAAAAAAA);
    ContextType ctxB = TestContextTraits<TContextSize>::Make(0xBBBBBBBB);

    cache.CheckAndInsert(targetKey, ctxA);

    std::atomic<bool> startFlag{ false };
    std::atomic<bool> stopFlag{ false };
    std::atomic<bool> abaFailureDetected{ false };

    uint32_t threadCount = std::thread::hardware_concurrency();

    if (threadCount == 0)
    {
        threadCount = 4;
    }

    // Oversubscribe specifically for ABA to guarantee heavy preemption
    threadCount *= 4;

    std::vector<std::thread> threads;

    auto writerWorker = [&](int threadId)
        {
            FastRng rng(threadId + 1000);

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                // Rapidly cycle A -> Deleted -> B -> Deleted -> A to induce ABA patterns
                cache.Delete(targetKey);

                if (rng.Next() % 4 == 0)
                {
                    std::this_thread::yield();
                }

                cache.CheckAndInsert(targetKey,
                                     ctxB,
                                     CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                     nullptr);

                if (rng.Next() % 4 == 0)
                {
                    std::this_thread::yield();
                }

                cache.Delete(targetKey);

                if (rng.Next() % 4 == 0)
                {
                    std::this_thread::yield();
                }

                cache.CheckAndInsert(targetKey,
                                     ctxA,
                                     CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                     nullptr);

                if (rng.Next() % 4 == 0)
                {
                    std::this_thread::yield();
                }
            }
        };

    auto readerWorker = [&](int threadId)
        {
            FastRng rng(threadId + 2000);

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                if (rng.Next() % 10 == 0)
                {
                    std::this_thread::yield();
                }

                if constexpr (TContextSize > 0)
                {
                    ContextType outCtx;

                    if (cache.LookupContext(targetKey, outCtx))
                    {
                        bool isA = TestContextTraits<TContextSize>::IsMatch(outCtx, 0xAAAAAAAA);
                        bool isB = TestContextTraits<TContextSize>::IsMatch(outCtx, 0xBBBBBBBB);

                        if (!isA && !isB)
                        {
                            abaFailureDetected.store(true, std::memory_order_relaxed);
                        }
                    }
                }
                else
                {
                    if (cache.Contains(targetKey))
                    {
                        // Payload tearing is impossible on a Key-Only cache
                    }
                }
            }
        };

    std::cout << "    [-] Hammering single key with rapid A-B-A transitions and random yields...\n";
    std::cout << "    [-] Using " << threadCount << " heavily oversubscribed threads...\n";
    std::cout << std::flush;

    for (uint32_t i = 0; i < threadCount / 2 + 1; ++i)
    {
        threads.emplace_back(writerWorker, i);
    }

    for (uint32_t i = 0; i < threadCount / 2; ++i)
    {
        threads.emplace_back(readerWorker, i + (threadCount / 2 + 1));
    }

    startFlag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(secondsToRun));
    stopFlag.store(true, std::memory_order_release);

    for (auto& t : threads)
    {
        t.join();
    }

    TEST_REQUIRE(abaFailureDetected.load() == false, "ABA DATA CORRUPTION DETECTED!", false);

    std::cout << "[+] ABA Correctness Test Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 4. Comprehensive Multi-Threaded Tearing & Race Condition Test
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunComprehensiveMultiThreadedTest(int secondsToRun)
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running Comprehensive Multi-Threaded Correctness Test (" << secondsToRun << " seconds)...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;

    TEST_REQUIRE(InitializeCache(cache, 2048), "Initialize failed", false);

    uint32_t threadCount = std::thread::hardware_concurrency();

    if (threadCount == 0)
    {
        threadCount = 4;
    }

    std::vector<std::thread> threads;
    std::atomic<bool>        startFlag{ false };
    std::atomic<bool>        stopFlag{ false };
    std::atomic<bool>        tearingDetected{ false };

    auto worker = [&](int threadId)
        {
            std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) + threadId);
            std::uniform_int_distribution<int> opDist(1, 100);
            std::uniform_int_distribution<uint64_t> idDist(1, 100);

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                int op = opDist(rng);
                uint64_t key = idDist(rng);

                if (op <= 60)
                {
                    if constexpr (TContextSize > 0)
                    {
                        ContextType outCtx;
                        if (cache.LookupContext(key, outCtx))
                        {
                            if (TestContextTraits<TContextSize>::IsTorn(outCtx))
                            {
                                tearingDetected.store(true, std::memory_order_relaxed);
                            }
                        }
                    }
                    else
                    {
                        cache.Contains(key);
                    }
                }
                else if (op <= 85)
                {
                    ContextType ctx = TestContextTraits<TContextSize>::Make(key + threadId);
                    cache.CheckAndInsert(key,
                                         ctx,
                                         CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                         nullptr);
                }
                else
                {
                    cache.Delete(key);
                }
            }
        };

    std::cout << "    [-] Hammering cache with " << threadCount << " threads (Mixed Ops)...\n";
    std::cout << std::flush;

    for (uint32_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }

    startFlag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(secondsToRun));
    stopFlag.store(true, std::memory_order_release);

    for (auto& t : threads)
    {
        t.join();
    }

    TEST_REQUIRE(tearingDetected.load() == false, "DATA TEARING DETECTED!", false);

    std::cout << "[+] Multi-Threaded Correctness Test Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 5. Oversubscribed Mixed Workload Test (3x Cores)
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunOversubscribedMixedWorkloadTest(int secondsToRun)
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running Oversubscribed Mixed Workload Test (3x Cores, " << secondsToRun << " seconds)...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;
    size_t capacity = 4096;
    TEST_REQUIRE(InitializeCache(cache, capacity), "Initialize failed", false);

    std::cout << "    [-] Cache Capacity : " << capacity << " items\n";
    std::cout << "    [-] Memory Used    : " << (cache.GetMemoryUsage() / 1024) << " KB\n";

    uint32_t hardwareCores = std::thread::hardware_concurrency();

    if (hardwareCores == 0)
    {
        hardwareCores = 4;
    }

    uint32_t threadCount = hardwareCores * 3;

    std::vector<std::thread> threads;
    std::atomic<bool>        startFlag{ false };
    std::atomic<bool>        stopFlag{ false };
    std::atomic<bool>        tearingDetected{ false };
    std::atomic<uint64_t>    totalOps{ 0 };

    auto worker = [&](int threadId)
        {
            std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) + threadId);
            std::uniform_int_distribution<int> opDist(1, 100);
            std::uniform_int_distribution<uint64_t> idDist(1, 500);

            uint64_t localOps = 0;

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                int op = opDist(rng);
                uint64_t key = idDist(rng);

                if (op <= 60)
                {
                    if constexpr (TContextSize > 0)
                    {
                        ContextType outCtx;

                        if (cache.LookupContext(key, outCtx))
                        {
                            if (TestContextTraits<TContextSize>::IsTorn(outCtx))
                            {
                                tearingDetected.store(true, std::memory_order_relaxed);
                            }
                        }
                    }
                    else
                    {
                        cache.Contains(key);
                    }
                }
                else if (op <= 85)
                {
                    ContextType ctx = TestContextTraits<TContextSize>::Make(key + threadId);

                    cache.CheckAndInsert(key,
                                         ctx,
                                         CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                         nullptr);
                }
                else
                {
                    cache.Delete(key);
                }

                ++localOps;
            }

            totalOps.fetch_add(localOps, std::memory_order_relaxed);
        };

    std::cout << "    [-] Hammering cache with " << threadCount << " threads (Heavy Contention)...\n";
    std::cout << std::flush;

    for (uint32_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }

    startFlag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(secondsToRun));
    stopFlag.store(true, std::memory_order_release);

    for (auto& t : threads)
    {
        t.join();
    }

    TEST_REQUIRE(tearingDetected.load() == false, "DATA TEARING DETECTED DURING OVERSUBSCRIPTION!", false);

    uint64_t throughput = static_cast<uint64_t>(totalOps.load() / secondsToRun);

    std::cout << "    [-] Total Ops  : " << totalOps.load() << "\n";
    std::cout << "    [-] Throughput : " << throughput << " Ops/sec\n";
    std::cout << "[+] Oversubscribed Mixed Workload Test Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 6. Constant Eviction Performance Test (Large Capacity)
// ----------------------------------------------------------------------------
template <size_t TContextSize>
bool RunConstantEvictionPerfTest(int secondsToRun)
{
    using ContextType = typename CacheUnderTest<TContextSize>::ContextType;

    std::cout << "[*] Running Constant Eviction Performance Test (" << secondsToRun << " seconds)...\n";
    std::cout << std::flush;

    CacheUnderTest<TContextSize> cache;
    size_t capacity = 1000000;
    TEST_REQUIRE(InitializeCache(cache, capacity), "Initialize failed", false);

    std::cout << "    [-] Cache Capacity : " << capacity << " items\n";
    std::cout << "    [-] Memory Used    : " << (cache.GetMemoryUsage() / (1024 * 1024)) << " MB\n";

    uint32_t threadCount = std::thread::hardware_concurrency();

    if (threadCount == 0)
    {
        threadCount = 4;
    }

    std::vector<std::thread> threads;
    std::atomic<bool>        startFlag{ false };
    std::atomic<bool>        stopFlag{ false };
    std::atomic<uint64_t>    totalOps{ 0 };

    auto worker = [&](int threadId)
        {
            std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) + threadId);
            std::uniform_int_distribution<int> opDist(1, 100);
            std::uniform_int_distribution<uint64_t> idDist(1, 10000000); // 10M key space ensuring constant eviction storm

            uint64_t localOps = 0;

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                int op = opDist(rng);
                uint64_t key = idDist(rng);

                if (op <= 50)
                {
                    if constexpr (TContextSize > 0)
                    {
                        ContextType outCtx;
                        cache.LookupContext(key, outCtx);
                    }
                    else
                    {
                        cache.Contains(key);
                    }
                }
                else if (op <= 85)
                {
                    ContextType ctx = TestContextTraits<TContextSize>::Make(key + threadId);
                    cache.CheckAndInsert(key,
                                         ctx,
                                         CacheUnderTest<TContextSize>::InsertPolicy::Overwrite,
                                         nullptr);
                }
                else
                {
                    cache.Delete(key);
                }

                ++localOps;
            }

            totalOps.fetch_add(localOps, std::memory_order_relaxed);
        };

    std::cout << "    [-] Hammering cache with " << threadCount << " threads (Constant Eviction)...\n";
    std::cout << std::flush;

    for (uint32_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }

    startFlag.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(secondsToRun));
    stopFlag.store(true, std::memory_order_release);

    for (auto& t : threads)
    {
        t.join();
    }

    uint64_t throughput = static_cast<uint64_t>(totalOps.load() / secondsToRun);

    std::cout << "    [-] Total Ops  : " << totalOps.load() << "\n";
    std::cout << "    [-] Throughput : " << throughput << " Ops/sec\n";
    std::cout << "[+] Constant Eviction Performance Test Passed.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// 7. Performance & Scalability Framework
// ----------------------------------------------------------------------------
template <typename TCache, 
          size_t   TContextSize>
uint64_t RunPerformanceWorker(TCache* pCache,
                              int     numThreads,
                              int     readPercentage,
                              int     secondsToRun)
{
    using ContextType = typename TCache::ContextType;

    std::vector<std::thread> threads;
    std::atomic<bool>        startFlag{ false };
    std::atomic<bool>        stopFlag{ false };
    std::atomic<uint64_t>    totalOps{ 0 };

    auto worker = [&](int threadId)
        {
            std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) + threadId);
            std::uniform_int_distribution<int> opDist(1, 100);
            std::uniform_int_distribution<uint64_t> idDist(1, 200000);

            uint64_t localOps = 0;
            ContextType ctx = TestContextTraits<TContextSize>::Make(0xDEADBEEF);
            ContextType outCtx;

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            while (!stopFlag.load(std::memory_order_relaxed))
            {
                int op = opDist(rng);
                uint64_t targetId = idDist(rng);

                if (op <= readPercentage)
                {
                    if constexpr (TContextSize > 0)
                    {
                        pCache->LookupContext(targetId, outCtx);
                    }
                    else
                    {
                        pCache->Contains(targetId);
                    }
                }
                else
                {
                    if (op % 2 == 0)
                    {
                        pCache->CheckAndInsert(targetId, ctx);
                    }
                    else
                    {
                        pCache->Delete(targetId);
                    }
                }

                ++localOps;
            }

            totalOps.fetch_add(localOps, std::memory_order_relaxed);
        };

    for (int i = 0; i < numThreads; ++i)
    {
        threads.emplace_back(worker, i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto timeStart = std::chrono::high_resolution_clock::now();
    startFlag.store(true, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::seconds(secondsToRun));

    stopFlag.store(true, std::memory_order_release);
    auto timeEnd = std::chrono::high_resolution_clock::now();

    for (auto& t : threads)
    {
        t.join();
    }

    std::chrono::duration<double> elapsedSeconds = timeEnd - timeStart;

    return static_cast<uint64_t>(totalOps.load() / elapsedSeconds.count());
}

template <size_t TContextSize>
uint64_t RunComparativeScalingSweep(const char* testName,
    int         readPercentage,
    int         secondsPerStep)
{
    std::cout << "[*] Comparative Scaling Sweep: " << testName << "\n";
    std::cout << "    " << std::left << std::setw(10) << "Threads"
              << "| " << std::setw(18) << "Lock-Free (Ops/s)"
              << "| " << std::setw(12) << "LF Scale"
              << "| " << std::setw(18) << "STL Mutex (Ops/s)"
              << "| " << std::setw(12) << "STL Scale"
              << "| " << "Advantage\n";
    std::cout << "    ---------------------------------------------------------------------------------------------\n";
    std::cout << std::flush;

    uint32_t maxThreads = std::thread::hardware_concurrency();
    if (maxThreads == 0)
    {
        maxThreads = 4;
    }

    uint32_t threadCount = 1;
    bool     didMax = false;
    uint64_t finalLfOps = 0;

    uint64_t lfBaseline = 0;
    uint64_t stlBaseline = 0;

    while (!didMax)
    {
        if (threadCount >= maxThreads)
        {
            threadCount = maxThreads;
            didMax = true;
        }

        // Setup & Run Lock-Free
        CacheUnderTest<TContextSize> lfCache;
        TEST_REQUIRE(InitializeCache(lfCache, 131072), "LF Initialize failed", 0);

        for (uint64_t i = 1; i <= 65000; ++i)
        {
            lfCache.CheckAndInsert(i, TestContextTraits<TContextSize>::Make(i));
        }

        uint64_t lfThroughput = RunPerformanceWorker<CacheUnderTest<TContextSize>, TContextSize>(&lfCache,
                                                                                                 threadCount,
                                                                                                 readPercentage,
                                                                                                 secondsPerStep);

        // Setup & Run STL
        StdCache<TContextSize> stlCache;
        TEST_REQUIRE(InitializeCache(stlCache, 131072), "STL Initialize failed", 0);

        for (uint64_t i = 1; i <= 65000; ++i)
        {
            stlCache.CheckAndInsert(i, TestContextTraits<TContextSize>::Make(i));
        }

        uint64_t stlThroughput = RunPerformanceWorker<StdCache<TContextSize>, TContextSize>(&stlCache,
                                                                                            threadCount,
                                                                                            readPercentage,
                                                                                            secondsPerStep);

        // Capture 1-Thread Baseline for internal scaling calculations
        if (threadCount == 1)
        {
            lfBaseline  = lfThroughput;
            stlBaseline = stlThroughput;
        }

        double lfScale = 0.0;
        if (lfBaseline > 0)
        {
            lfScale = static_cast<double>(lfThroughput) / static_cast<double>(lfBaseline);
        }

        double stlScale = 0.0;
        if (stlBaseline > 0)
        {
            stlScale = static_cast<double>(stlThroughput) / static_cast<double>(stlBaseline);
        }

        double speedup = 0.0;
        if (stlThroughput > 0)
        {
            speedup = static_cast<double>(lfThroughput) / static_cast<double>(stlThroughput);
        }

        std::stringstream ssLfScale;
        ssLfScale << std::fixed << std::setprecision(2) << lfScale << "x";

        std::stringstream ssStlScale;
        ssStlScale << std::fixed << std::setprecision(2) << stlScale << "x";

        std::cout << "    " << std::left << std::setw(10) << threadCount
                  << "| " << std::setw(18) << lfThroughput
                  << "| " << std::setw(12) << ssLfScale.str()
                  << "| " << std::setw(18) << stlThroughput
                  << "| " << std::setw(12) << ssStlScale.str()
                  << "| " << std::fixed << std::setprecision(2) << speedup << "x Faster\n";

        std::cout << std::flush;

        if (didMax)
        {
            finalLfOps = lfThroughput;
        }

        if (!didMax)
        {
            threadCount *= 2;
        }
    }

    std::cout << "[+] Comparative Sweep Complete.\n\n";
    std::cout << std::flush;

    return finalLfOps;
}

// ----------------------------------------------------------------------------
// 8. Tail Latency Metric Capture (Cycle-Accurate)
// ----------------------------------------------------------------------------
template <typename TCache, 
          size_t   TContextSize>
bool RunTailLatencyTest(const char*     testName,
                        size_t          cacheCapacity,
                        LatencyMetrics* outMetrics)
{
    std::cout << "[*] Running Tail Latency Test: " << testName << "...\n";
    std::cout << std::flush;

    TCache table;

    TEST_REQUIRE(InitializeCache(table, cacheCapacity), "Init failed", false);

    for (size_t i = 1; i <= cacheCapacity / 2; ++i)
    {
        table.CheckAndInsert(i, TestContextTraits<TContextSize>::Make(i));
    }

    uint32_t threadCount = std::thread::hardware_concurrency();

    if (threadCount == 0)
    {
        threadCount = 4;
    }

    const size_t   SAMPLES_PER_THREAD = 50000;
    const uint32_t SAMPLE_RATE        = 100;

    std::vector<std::vector<double>> allThreadSamples(threadCount);
    std::vector<std::thread>         threads;
    std::atomic<bool>                warmUpFlag{ true };
    std::atomic<bool>                startFlag{ false };

    // Calibrate CPU Cycle Counter
    double cyclesPerNs = EstimateRdtscFrequency();

    auto worker = [&](int threadId)
        {
            FastRng rng(threadId + 8000);
            uint64_t maxKey = cacheCapacity + (cacheCapacity / 2);

            std::vector<double>& localSamples = allThreadSamples[threadId];
            localSamples.reserve(SAMPLES_PER_THREAD);

            uint32_t opCounter = 0;

            while (!startFlag.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            // Warm-up phase
            while (warmUpFlag.load(std::memory_order_relaxed))
            {
                uint64_t key = (rng.Next() % maxKey) + 1;

                if constexpr (TContextSize > 0)
                {
                    typename TCache::ContextType out;
                    if (static_cast<bool>(table.LookupContext(key, out)))
                    {
                        // Active warm-up
                    }
                }
                else
                {
                    if (table.Contains(key))
                    {
                        // Active warm-up
                    }
                }
            }

            // Sampling phase
            while (localSamples.size() < SAMPLES_PER_THREAD)
            {
                uint64_t key    = (rng.Next() % maxKey) + 1;
                uint32_t opType = rng.Next() % 100;

                opCounter++;

                bool shouldSample = (opCounter % SAMPLE_RATE == 0);

                uint64_t startTick = 0;

                if (shouldSample)
                {
                    startTick = GetTestHardwareTickCount();
                }

                if (opType < 90)
                {
                    if constexpr (TContextSize > 0)
                    {
                        typename TCache::ContextType out;
                        table.LookupContext(key, out);
                    }
                    else
                    {
                        table.Contains(key);
                    }
                }
                else
                {
                    table.CheckAndInsert(key, TestContextTraits<TContextSize>::Make(key));
                }

                if (shouldSample)
                {
                    uint64_t endTick = GetTestHardwareTickCount();

                    // Out-of-order execution bounds safety
                    if (endTick > startTick)
                    {
                        double elapsedNs = static_cast<double>(endTick - startTick) / cyclesPerNs;
                        localSamples.push_back(elapsedNs);
                    }
                }
            }
        };

    for (uint32_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }

    startFlag.store(true, std::memory_order_release);

    std::cout << "    - Warming up for 1 second...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "    - Warm-up complete. Collecting tail latency samples...\n";
    std::cout << std::flush;

    warmUpFlag.store(false, std::memory_order_relaxed);

    for (auto& t : threads)
    {
        t.join();
    }

    std::vector<double> globalSamples;
    globalSamples.reserve(SAMPLES_PER_THREAD * threadCount);

    for (const auto& local : allThreadSamples)
    {
        globalSamples.insert(globalSamples.end(), local.begin(), local.end());
    }

    std::sort(globalSamples.begin(), globalSamples.end());

    auto getPercentile = [&](double p) -> double
        {
            size_t index = static_cast<size_t>(p * globalSamples.size());

            if (index >= globalSamples.size())
            {
                index = globalSamples.size() - 1;
            }

            return globalSamples[index];
        };

    if (outMetrics)
    {
        outMetrics->P50    = static_cast<uint64_t>(getPercentile(0.50));
        outMetrics->P90    = static_cast<uint64_t>(getPercentile(0.90));
        outMetrics->P99    = static_cast<uint64_t>(getPercentile(0.99));
        outMetrics->P99_9  = static_cast<uint64_t>(getPercentile(0.999));
        outMetrics->P99_99 = static_cast<uint64_t>(getPercentile(0.9999));
    }

    std::cout << "    - Total Samples Collected : " << globalSamples.size() << "\n";
    std::cout << "    - P50 (Median) Latency    : " << std::fixed << std::setprecision(2) << getPercentile(0.50) << " ns\n";
    std::cout << "    - P90 Latency             : " << getPercentile(0.90) << " ns\n";
    std::cout << "    - P99 Latency             : " << getPercentile(0.99) << " ns\n";
    std::cout << "    - P99.9 Latency           : " << getPercentile(0.999) << " ns\n";
    std::cout << "    - P99.99 Latency          : " << getPercentile(0.9999) << " ns\n";
    std::cout << "[+] Tail Latency Test Complete.\n\n";
    std::cout << std::flush;

    return true;
}

// ----------------------------------------------------------------------------
// Formatter for Final Summary Comparison
// ----------------------------------------------------------------------------
void PrintLatencyComparison(const char*           testName,
                            const LatencyMetrics& lfMetrics,
                            const LatencyMetrics& stlMetrics)
{
    std::cout << "\n---------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(32) << testName
              << " | " << std::setw(15) << "Lock-Free Cache"
              << " | " << std::setw(16) << "STL Mutex Map"
              << " | Stability Advantage\n";
    std::cout << "---------------------------------------------------------------------------------------------------\n";

    auto printMetricRow = [](const char* metric, uint64_t customLatNs, uint64_t stdLatNs)
        {
            std::cout << std::left << std::setw(32) << metric
                      << " | " << std::setw(15) << customLatNs
                      << " | " << std::setw(16) << stdLatNs
                      << " | ";

            if (customLatNs == 0)
            {
                if (stdLatNs == 0)
                {
                    std::cout << "1.00x (Both Sub-ns)\n";
                }
                else
                {
                    std::cout << "> " << stdLatNs << ".00x (Sub-ns vs " << stdLatNs << "ns)\n";
                }
            }
            else
            {
                double advantage = static_cast<double>(stdLatNs) / static_cast<double>(customLatNs);
                std::cout << std::fixed << std::setprecision(2) << advantage << "x More Stable\n";
            }
        };

    printMetricRow("P50  (Median)", lfMetrics.P50, stlMetrics.P50);
    printMetricRow("P90  (Typical Load)", lfMetrics.P90, stlMetrics.P90);
    printMetricRow("P99  (High Load)", lfMetrics.P99, stlMetrics.P99);
    printMetricRow("P99.9  (Algorithmic Limit)", lfMetrics.P99_9, stlMetrics.P99_9);
    printMetricRow("P99.99 (OS Wait Starvation)", lfMetrics.P99_99, stlMetrics.P99_99);
}

// ----------------------------------------------------------------------------
// Master Test Orchestrator
// ----------------------------------------------------------------------------
inline int RunAllCacheTests()
{
    std::cout << "\n";
    std::cout << "=========================================================\n";
    std::cout << "    CPU: " << cpu_name() << "\n";
    std::cout << "=========================================================\n\n";
    std::cout << std::flush;

    SetHighPriority();

    LatencyMetrics lf0Lat = { 0 };
    LatencyMetrics stl0Lat = { 0 };
    LatencyMetrics lf64Lat = { 0 };
    LatencyMetrics stl64Lat = { 0 };
    LatencyMetrics lf128Lat = { 0 };
    LatencyMetrics stl128Lat = { 0 };

    auto RunTests = [&]() -> bool
        {
            std::cout << "=========================================================\n";
            std::cout << "          OPTIMISTIC CACHE <0-BIT CONTEXT>               \n";
            std::cout << "=========================================================\n\n";

            if (!RunCorrectnessTests<0>())
            {
                return false;
            }

            if (!RunEvictionTest<0>())
            {
                return false;
            }

            if (!RunAbaCorrectnessTest<0>(15))
            {
                return false;
            }

            if (!RunComprehensiveMultiThreadedTest<0>(15))
            {
                return false;
            }

            if (!RunOversubscribedMixedWorkloadTest<0>(10))
            {
                return false;
            }

            if (!RunConstantEvictionPerfTest<0>(10))
            {
                return false;
            }

            RunComparativeScalingSweep<0>("10/90 Write-Heavy Workload", 10, 2);
            RunComparativeScalingSweep<0>("50/50 Mixed Contention Workload", 50, 2);
            RunComparativeScalingSweep<0>("80/20 Read-Heavy Workload", 80, 2);
            RunComparativeScalingSweep<0>("95/5 Highly Skewed Workload", 95, 2);

            if (!RunTailLatencyTest<CacheUnderTest<0>, 0>("Lock-Free Cache <0-bit>", 131072, &lf0Lat))
            {
                return false;
            }

            if (!RunTailLatencyTest<StdCache<0>, 0>("STL Mutex Baseline <0-bit>", 131072, &stl0Lat))
            {
                return false;
            }

            std::cout << "=========================================================\n";
            std::cout << "          OPTIMISTIC CACHE <64-BIT CONTEXT>              \n";
            std::cout << "=========================================================\n\n";

            if (!RunCorrectnessTests<64>())
            {
                return false;
            }

            if (!RunEvictionTest<64>())
            {
                return false;
            }

            if (!RunAbaCorrectnessTest<64>(15))
            {
                return false;
            }

            if (!RunComprehensiveMultiThreadedTest<64>(15))
            {
                return false;
            }

            if (!RunOversubscribedMixedWorkloadTest<64>(10))
            {
                return false;
            }

            if (!RunConstantEvictionPerfTest<64>(10))
            {
                return false;
            }

            RunComparativeScalingSweep<64>("10/90 Write-Heavy Workload", 10, 2);
            RunComparativeScalingSweep<64>("50/50 Mixed Contention Workload", 50, 2);
            RunComparativeScalingSweep<64>("80/20 Read-Heavy Workload", 80, 2);
            RunComparativeScalingSweep<64>("95/5 Highly Skewed Workload", 95, 2);

            if (!RunTailLatencyTest<CacheUnderTest<64>, 64>("Lock-Free Cache <64-bit>", 131072, &lf64Lat))
            {
                return false;
            }

            if (!RunTailLatencyTest<StdCache<64>, 64>("STL Mutex Baseline <64-bit>", 131072, &stl64Lat))
            {
                return false;
            }

            std::cout << "=========================================================\n";
            std::cout << "          OPTIMISTIC CACHE <128-BIT CONTEXT>             \n";
            std::cout << "=========================================================\n\n";

            if (!RunCorrectnessTests<128>())
            {
                return false;
            }

            if (!RunEvictionTest<128>())
            {
                return false;
            }

            if (!RunAbaCorrectnessTest<128>(15))
            {
                return false;
            }

            if (!RunComprehensiveMultiThreadedTest<128>(15))
            {
                return false;
            }

            if (!RunOversubscribedMixedWorkloadTest<128>(10))
            {
                return false;
            }

            if (!RunConstantEvictionPerfTest<128>(10))
            {
                return false;
            }

            RunComparativeScalingSweep<128>("10/90 Write-Heavy Workload", 10, 2);
            RunComparativeScalingSweep<128>("50/50 Mixed Contention Workload", 50, 2);
            RunComparativeScalingSweep<128>("80/20 Read-Heavy Workload", 80, 2);
            RunComparativeScalingSweep<128>("95/5 Highly Skewed Workload", 95, 2);

            if (!RunTailLatencyTest<CacheUnderTest<128>, 128>("Lock-Free Cache <128-bit>", 131072, &lf128Lat))
            {
                return false;
            }

            if (!RunTailLatencyTest<StdCache<128>, 128>("STL Mutex Baseline <128-bit>", 131072, &stl128Lat))
            {
                return false;
            }

            return true;
        };

    bool testsPassed = RunTests();

    if (!testsPassed)
    {
        std::cerr << "\n[-] Test suite aborted due to an error.\n";
        return -1;
    }

    PrintLatencyComparison("Tail Latency (ns) <0-bit>", lf0Lat, stl0Lat);
    PrintLatencyComparison("Tail Latency (ns) <64-bit>", lf64Lat, stl64Lat);
    PrintLatencyComparison("Tail Latency (ns) <128-bit>", lf128Lat, stl128Lat);

    std::cout << "--- All Tests Finished Successfully ---\n";

    return 0;
}