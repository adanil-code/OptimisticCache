/*
* Apache Optimistic Cache
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

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------
// Cross-Platform Hardware Helpers
// ----------------------------------------------------------------------------
#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>
#else
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif
#endif

// ----------------------------------------------------------------------------
// Cross-Platform Prefetch Helpers
// ----------------------------------------------------------------------------
#if defined(_MSC_VER)
#if defined(_M_ARM64) || defined(_M_ARM)
#include <intrin.h>
#define CACHE_PREFETCH(ptr) __prefetch(ptr)
#else
#include <xmmintrin.h>
#define CACHE_PREFETCH(ptr) _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T0)
#endif
#elif defined(__GNUC__) || defined(__clang__)
// Universal builtin for Linux (GCC) and macOS (Apple Clang)
#define CACHE_PREFETCH(ptr) __builtin_prefetch(ptr, 0, 3)
#else
#define CACHE_PREFETCH(ptr) ((void)0)
#endif

// ----------------------------------------------------------------------------
// Cross-Platform NUMA Helpers
// ----------------------------------------------------------------------------
#if defined(__linux__)
#include <numa.h>
#include <numaif.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__unix__)
#include <unistd.h>
#else
#include <stdlib.h>
#endif

// ----------------------------------------------------------------------------
// Cache Line Constants
//
// We explicitly avoid std::hardware_destructive_interference_size due to ABI 
// instability warnings ([-Winterference-size]) across different compiler flags.
// 
// Apply 128-byte alignment strictly for Apple Silicon to prevent false sharing,
// while preserving optimal 64-byte density for x86_64 and standard Linux ARM64.
// ----------------------------------------------------------------------------
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr size_t CACHE_LINE_SIZE = 128;
#else        
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

constexpr size_t CACHE_LINE_MASK = CACHE_LINE_SIZE - 1;

// ----------------------------------------------------------------------------
// YieldProcessorThread
// Issues a hardware-level hint to the CPU pipeline that the current thread
// is in a spin-loop. This prevents the pipeline from speculatively executing
// instructions, saving power and reducing memory-bus contention for the 
// thread actually doing the work.
// ----------------------------------------------------------------------------
inline void YieldProcessorThread()
{
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// ----------------------------------------------------------------------------
// FastThreadLocalPRNG
// Generates a fast, thread-local pseudo-random number.
// Used primarily for selecting eviction victims in fully populated cache sets.
// Replaces OS/hardware specific instructions (like `mrs cntvct_el0`) to avoid
// ARM kernel traps and thread-migration drift issues.
// ----------------------------------------------------------------------------
inline uint32_t FastThreadLocalPRNG()
{
    thread_local uint32_t state = static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ 0x9E3779B9);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return state;
}

// ----------------------------------------------------------------------------
// Default NUMA Allocator Policy
// Abstracts OS-specific memory allocation to ensure hash table shards are
// physically pinned to the requested NUMA nodes, maximizing memory bandwidth.
// ----------------------------------------------------------------------------
struct DefaultNumaAllocator
{
    static const std::vector<uint32_t>& GetValidNodes() noexcept
    {
        static const std::vector<uint32_t> validNodes = []()
        {
            try
            {
                std::vector<uint32_t> nodes;

#if defined(_WIN32)
                ULONG highestNode = 0;

                if (GetNumaHighestNodeNumber(&highestNode)) [[likely]]
                {
                    const USHORT maxNode = static_cast<USHORT>(highestNode);

                    for (USHORT i = 0; i <= maxNode; ++i)
                    {
                        ULONGLONG availableMemory = 0;
                        if (GetNumaAvailableMemoryNodeEx(i, &availableMemory))
                        {
                            nodes.push_back(i);
                        }
                    }
                }
#elif defined(__linux__)
                if (numa_available() >= 0) [[likely]]
                {
                    int highestNode = numa_max_node();

                    for (int i = 0; i <= highestNode; ++i)
                    {
                        if (numa_bitmask_isbitset(numa_all_nodes_ptr, i))
                        {
                            nodes.push_back(i);
                        }
                    }
                }
#endif
                if (nodes.empty()) [[unlikely]]
                {
                    nodes.push_back(0);
                }

                nodes.shrink_to_fit();
                return nodes;
            }
            catch (...)
            {
                // Returns a static fallback to strictly prevent dangling reference UB
                static const std::vector<uint32_t> fallback{ 0 };
                return fallback;
            }
        }();

        return validNodes;
    }

    static inline void* Allocate(size_t   size,
                                 uint32_t node) noexcept
    {
#if defined(_WIN32)
        void* ptr = VirtualAllocExNuma(GetCurrentProcess(),
                                       NULL,
                                       size,
                                       MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE,
                                       node);

        if (!ptr) [[unlikely]]
        {
            ptr = VirtualAlloc(NULL,
                               size,
                               MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
        }

        return ptr;
#elif defined(__linux__)
        if (numa_available() >= 0) [[likely]]
        {
            void* ptr = numa_alloc_onnode(size, node);
            if (!ptr) [[unlikely]]
            {
                ptr = numa_alloc_local(size);
            }

            return ptr;
        }

        return ::operator new[](size, std::align_val_t{ CACHE_LINE_SIZE }, std::nothrow);
#else
        return ::operator new[](size, std::align_val_t{ CACHE_LINE_SIZE }, std::nothrow);
#endif
    }

    static inline void Free(void* ptr,
                            size_t size) noexcept
    {
        if (!ptr) [[unlikely]]
        {
            return;
        }

#if defined(_WIN32)
        VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(__linux__)
        if (numa_available() >= 0) [[likely]]
        {
            numa_free(ptr, size);
        }
        else
        {
            ::operator delete[](ptr, std::align_val_t{ CACHE_LINE_SIZE });
        }
#else
        ::operator delete[](ptr, std::align_val_t{ CACHE_LINE_SIZE });
#endif
    }
};

// ----------------------------------------------------------------------------
// 128-bit Context Structure for high-density payloads.
// ----------------------------------------------------------------------------
struct alignas(16) Context128
{
    uint64_t Low;
    uint64_t High;
};

// ----------------------------------------------------------------------------
// Context0 for Context-free implementations
// ----------------------------------------------------------------------------
struct Context0
{};

// ----------------------------------------------------------------------------
// C++20 Concepts
// ----------------------------------------------------------------------------
template <size_t TContextSize>
concept ValidContextSize = (TContextSize == 0 || TContextSize == 64 || TContextSize == 128);

// ----------------------------------------------------------------------------
// Cache Context Traits
// Abstracts atomic read/write operations for payload data based on size.
// ----------------------------------------------------------------------------
template <size_t TContextSize>
    requires ValidContextSize<TContextSize>
struct CacheContextTraits;

template <>
struct CacheContextTraits<0>
{
    using Type = Context0;

    static inline Type Read(const void* source)
    {
        return {};
    }

    static inline void Write(void* dest,
                             Type  value)
    {}
};

template <>
struct CacheContextTraits<64>
{
    using Type = uint64_t;

    static inline Type Read(const void* source)
    {
        // A relaxed load is perfectly safe here because the overarching Sequence Lock 
        // controls visibility. The SeqLock's acquire/release semantics bracket this read.
        return std::atomic_ref<const Type>(*reinterpret_cast<const Type*>(source)).load(std::memory_order_relaxed);
    }

    static inline void Write(void* dest,
                             Type  value)
    {
        std::atomic_ref<Type>(*reinterpret_cast<Type*>(dest)).store(value, std::memory_order_relaxed);
    }
};

template <>
struct CacheContextTraits<128>
{
    using Type = Context128;

    static inline Type Read(const void* source)
    {
        Type val;
        const Type* typedSource = reinterpret_cast<const Type*>(source);

        // Torn reads across the Low/High halves are theoretically possible here. 
        // However, using a 128-bit CAS to prevent tearing would dirty the reader's cache
        // line and destroy read scalability. We intentionally accept torn reads here 
        // because the overarching SeqLock loop will detect the tear (via sequence mismatch) 
        // and force a clean retry.
        val.Low  = std::atomic_ref<const uint64_t>(typedSource->Low).load(std::memory_order_relaxed);
        val.High = std::atomic_ref<const uint64_t>(typedSource->High).load(std::memory_order_relaxed);

        return val;
    }

    static inline void Write(void* dest,
                             Type  value)
    {
        Type* typedDest = reinterpret_cast<Type*>(dest);
        std::atomic_ref<uint64_t>(typedDest->Low).store(value.Low, std::memory_order_relaxed);
        std::atomic_ref<uint64_t>(typedDest->High).store(value.High, std::memory_order_relaxed);
    }
};

// ----------------------------------------------------------------------------
// High-Performance Optimistic Concurrency Cache
//
// Architecture:
// - Sharded Partitioning: The cache is divided into multiple independent shards 
//   based on active logical processor count to minimize global lock contention 
//   and maximize memory controller bandwidth across NUMA nodes.
// 
// - Sequence Lock (SeqLock) Protocol: Readers perform optimistic, wait-free 
//   lookups. They validate a monotonic sequence number before and after reading 
//   the data. If the sequence is odd (indicating a writer is actively modifying 
//   the slot) or if the sequence changes during the read (a tear), the reader 
//   retries. Writers lock a slot by atomically incrementing the sequence to an 
//   odd number, perform the write, and release by incrementing to an even number.
// 
// - Hot/Cold Segregation: Cache memory is split. Metadata (Keys and Sequences) 
//   are packed into "Hot" blocks to maximize L1 search density. Payload 
//   data is isolated in "Cold" blocks. This prevents large payloads from 
//   polluting the CPU cache during hash collisions or cache misses.
// 
// - Tiered Backoff: When threads encounter a locked sequence (contention), they 
//   enter an escalating backoff loop. This transitions from lightweight hardware 
//   hints to OS scheduler yields, and finally hard processor stalls to prevent 
//   livelock and priority inversion under heavy oversubscription.
// ----------------------------------------------------------------------------
template <size_t   TContextSize,
          typename TAllocator = DefaultNumaAllocator>
    requires ValidContextSize<TContextSize>
class OptimisticCache
{
public:
    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    enum class InsertPolicy
    {
        KeepExisting,
        Overwrite
    };

    using EnumerateCallback = void (*)(uint64_t    key,
                                       ContextType context,
                                       void* userData);

    // ------------------------------------------------------------------------
    // CacheSetHot (L1 Cache Optimized)
    // Hot search metadata. Packed sequentially to maximize L1 search density.
    // Contains only the minimum data needed to verify a match and sequence state.
    // Forced to 128-byte alignment to prevent 3-cache-line straddling on 64-byte 
    // architectures while sitting perfectly on a single 128-byte Apple Silicon line.
    // ------------------------------------------------------------------------
    struct alignas(128) CacheSetHot
    {
        std::array<std::atomic<uint64_t>, 8> Keys;
        std::array<std::atomic<uint64_t>, 8> Seqs;
    };

    // ------------------------------------------------------------------------
    // CacheSetCold
    // Cold payload data. Segregated to prevent cache pollution. Accessed only 
    // when a definitive key match occurs in the corresponding Hot set.
    // ------------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) CacheSetCold
    {
        std::array<ContextType, 8> Contexts;
    };

    // Strict alignment guarantees to ensure atomic_ref behavior remains defined
    static_assert(TContextSize == 0 || alignof(ContextType) >= (TContextSize == 128 ? 16 : 8), "ContextType alignment violates hardware atomicity requirements.");
    static_assert(TContextSize == 0 || alignof(CacheSetCold) >= alignof(ContextType), "CacheSetCold alignment is weaker than ContextType.");

    // ------------------------------------------------------------------------
    // Cache Shard
    // Aligned to dynamic hardware cache line size to prevent false sharing.
    // Independently manages its assigned portion of the hot/cold sets.
    // ------------------------------------------------------------------------
    struct alignas(CACHE_LINE_SIZE) Shard
    {
        void* RawMemoryBlock;  // Base pointer for accurate allocator deallocation
        size_t        AllocationSize;  // Exact size allocated on the NUMA node
        CacheSetHot* HotSets;         // Pointer to the contiguous block of Hot metadata sets
        CacheSetCold* ColdSets;        // Pointer to the contiguous block of Cold payload sets
        size_t        Mask;            // Bitwise mask used to rapidly route hashes to specific buckets
    };

private:
    Shard* m_shards;                 // Aligned array of cache shards used to distribute workload and minimize lock contention
    void* m_shardsBase;             // Raw pointer for proper NUMA allocator deallocation
    size_t   m_shardsBaseSize;         // Total byte size of the raw memory allocation required to safely free the block
    uint32_t m_shardCount;             // Total number of shards, guaranteed to be a power of two for fast bitwise hash routing

    // ------------------------------------------------------------------------
    // Hasher
    // Fast avalanche mixer (SplitMix64 variant) for high-entropy key distribution
    // ------------------------------------------------------------------------
    static inline uint64_t Hasher(uint64_t z)
    {
        uint64_t mixed = z ^ (z >> 30);
        mixed *= 0xbf58476d1ce4e5b9ULL;
        mixed ^= (mixed >> 27);
        mixed *= 0x94d049bb133111ebULL;
        mixed ^= (mixed >> 31);

        return mixed;
    }

    // ------------------------------------------------------------------------
    // FindHitIndex
    // Performs a relaxed linear scan of a bucket. Ordering is handled by SeqLock.
    // ------------------------------------------------------------------------
    static inline int FindHitIndex(const CacheSetHot* hotSet,
                                   uint64_t           targetKey)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (hotSet->Keys[i].load(std::memory_order_relaxed) == targetKey) [[unlikely]]
            {
                return i;
            }
        }

        return -1;
    }

    // ------------------------------------------------------------------------
    // FindEmptySlotIndex
    // Identifies a zeroed key, representing an unused slot
    // ------------------------------------------------------------------------
    static inline int FindEmptySlotIndex(const CacheSetHot* hotSet)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (hotSet->Keys[i].load(std::memory_order_relaxed) == 0) [[unlikely]]
            {
                return i;
            }
        }

        return -1;
    }

    // ------------------------------------------------------------------------
    // ExecuteTieredBackoff
    // Optimized Tiered Backoff hardened for oversubscription.
    // Transitions smoothly from hardware hints to scheduler yields, and finally 
    // to hard execution stalls to prevent priority inversion under extreme load.
    // ------------------------------------------------------------------------
    static inline void ExecuteTieredBackoff(uint32_t retries)
    {
        if (retries < 64) [[likely]]
        {
            YieldProcessorThread(); // Hardware pause (spin)
        }
        else if (retries < 256)
        {
            std::this_thread::yield(); // OS scheduler yield
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(1)); // Hard sleep
        }
    }

public:
    enum class InsertResult
    {
        Inserted,
        Updated,
        Failed
    };

    OptimisticCache() : m_shards(nullptr),
                        m_shardsBase(nullptr),
                        m_shardsBaseSize(0),
                        m_shardCount(0)
    {}

    ~OptimisticCache()
    {
        Cleanup();
    }

    // ------------------------------------------------------------------------
    // Initialize
    // Allocates shard memory using NUMA-aware allocations. Distributes memory 
    // physically across active nodes to maximize memory controller bus bandwidth.
    // 
    // Parameters:
    //   totalEntries - The requested total capacity of the cache. The underlying 
    //                  implementation will round this value up to ensure it is 
    //                  evenly divisible by the calculated shard count and 8-slot sets.
    //
    // Returns:
    //   true if initialization and all NUMA allocations succeeded.
    //   false if allocation fails due to insufficient system memory.
    // ------------------------------------------------------------------------
    bool Initialize(size_t totalEntries)
    {
        Cleanup();

        uint32_t numProcs = std::thread::hardware_concurrency();
        if (numProcs == 0) [[unlikely]]
        {
            numProcs = 4; // Fallback edge case if OS fails to report cores
        }

        uint32_t targetShards = numProcs * 4;

        m_shardCount = std::bit_ceil(targetShards);
        if (m_shardCount > 512) [[unlikely]]
        {
            m_shardCount = 512; // Cap shards to prevent extreme memory fragmentation
        }

        const std::vector<uint32_t>& validNumaNodes = TAllocator::GetValidNodes();
        uint32_t primaryNode = validNumaNodes.empty() ? 0 : validNumaNodes[0];

        try
        {
            m_shardsBaseSize = (sizeof(Shard) * m_shardCount) + CACHE_LINE_MASK;
            m_shardsBase     = TAllocator::Allocate(m_shardsBaseSize, primaryNode);

            if (!m_shardsBase) [[unlikely]]
            {
                return false;
            }

            uintptr_t shardPtr = reinterpret_cast<uintptr_t>(m_shardsBase);
            shardPtr = (shardPtr + CACHE_LINE_MASK) & ~(uintptr_t)CACHE_LINE_MASK;
            m_shards = reinterpret_cast<Shard*>(shardPtr);

            // Zero-initialize the shards array. This is critical for preventing 
            // wild pointer frees if allocation fails halfway through the loop below.
            for (uint32_t i = 0; i < m_shardCount; ++i)
            {
                new (&m_shards[i]) Shard();
                m_shards[i].RawMemoryBlock = nullptr;
            }

            size_t entriesPerShard = totalEntries / m_shardCount;
            if (entriesPerShard < 256) [[unlikely]]
            {
                entriesPerShard = 256;
            }

            size_t numSets = std::bit_ceil(entriesPerShard / 8);
            if (numSets == 0) [[unlikely]]
            {
                numSets = 1;
            }

            uint32_t numValidNodes = static_cast<uint32_t>(validNumaNodes.size());

            size_t hotSize  = numSets * sizeof(CacheSetHot);
            size_t coldSize = 0;

            if constexpr (TContextSize > 0)
            {
                coldSize = numSets * sizeof(CacheSetCold);
            }

            size_t allocationSizePerShard = hotSize + coldSize + CACHE_LINE_MASK;

            for (uint32_t i = 0; i < m_shardCount; ++i)
            {
                uint32_t targetPhysicalNode = validNumaNodes[i % numValidNodes];
                void* rawBlock = TAllocator::Allocate(allocationSizePerShard, targetPhysicalNode);

                if (!rawBlock) [[unlikely]]
                {
                    // Unwind previously successful allocations to prevent leaks
                    Cleanup();
                    return false;
                }

                m_shards[i].RawMemoryBlock = rawBlock;
                m_shards[i].AllocationSize = allocationSizePerShard;

                uintptr_t uPtr = reinterpret_cast<uintptr_t>(rawBlock);
                uPtr = (uPtr + CACHE_LINE_MASK) & ~(uintptr_t)CACHE_LINE_MASK;

                m_shards[i].HotSets  = reinterpret_cast<CacheSetHot*>(uPtr);
                m_shards[i].ColdSets = nullptr;
                m_shards[i].Mask     = numSets - 1;

                if constexpr (TContextSize > 0)
                {
                    m_shards[i].ColdSets = reinterpret_cast<CacheSetCold*>(uPtr + hotSize);
                }

                for (size_t setIdx = 0; setIdx < numSets; ++setIdx)
                {
                    new (&m_shards[i].HotSets[setIdx]) CacheSetHot();

                    if constexpr (TContextSize > 0)
                    {
                        new (&m_shards[i].ColdSets[setIdx]) CacheSetCold();
                    }

                    for (int slot = 0; slot < 8; ++slot)
                    {
                        m_shards[i].HotSets[setIdx].Keys[slot].store(0, std::memory_order_relaxed);
                        m_shards[i].HotSets[setIdx].Seqs[slot].store(0, std::memory_order_relaxed);

                        if constexpr (TContextSize > 0)
                        {
                            ContextType emptyCtx{};
                            CacheContextTraits<TContextSize>::Write(&m_shards[i].ColdSets[setIdx].Contexts[slot], emptyCtx);
                        }
                    }
                }
            }
        }
        catch (...)
        {
            Cleanup();
            return false;
        }

        return true;
    }

    // ------------------------------------------------------------------------
    // GetMemoryUsage
    // Calculates the total bytes allocated by the cache across all NUMA nodes.
    // ------------------------------------------------------------------------
    size_t GetMemoryUsage() const
    {
        size_t total = m_shardsBaseSize;

        if (m_shards != nullptr)
        {
            for (uint32_t i = 0; i < m_shardCount; ++i)
            {
                total += m_shards[i].AllocationSize;
            }
        }

        return total;
    }

    // ------------------------------------------------------------------------
    // Cleanup
    // Safely releases all NUMA-allocated shard and array memory back to the 
    // allocator. Caller must ensure no active operations are occurring.
    // ------------------------------------------------------------------------
    void Cleanup()
    {
        if (m_shards) [[likely]]
        {
            for (uint32_t i = 0; i < m_shardCount; ++i)
            {
                if (m_shards[i].RawMemoryBlock) [[likely]]
                {
                    TAllocator::Free(m_shards[i].RawMemoryBlock, m_shards[i].AllocationSize);
                }

                m_shards[i].~Shard();
            }
        }

        if (m_shardsBase) [[likely]]
        {
            TAllocator::Free(m_shardsBase, m_shardsBaseSize);
            m_shardsBase = nullptr;
            m_shards     = nullptr;
        }

        m_shardCount     = 0;
        m_shardsBaseSize = 0;
    }

    // ------------------------------------------------------------------------
    // CheckAndInsert
    // Thread-safe insertion utilizing atomic sequence locks. Writes lock a 
    // specific slot by transitioning its sequence number to an odd state via 
    // atomic compare-and-swap.
    // 
    // Parameters:
    //   key                - The unique 64-bit identifier for the entry. Must not be 0.
    //   context            - The payload data to store alongside the key.
    //   policy             - Determines behavior if the key already exists (KeepExisting or Overwrite).
    //   outExistingContext - (Optional) Captures the existing payload if the key was already present.
    //   outEvictedKey      - (Optional) Captures the key of an item that was forcefully evicted to make room.
    //   outEvictedContext  - (Optional) Captures the payload of the forcefully evicted item.
    //
    // Returns:
    //   InsertResult::Inserted if a new key was successfully added.
    //   InsertResult::Updated if an existing key was successfully overwritten.
    //   InsertResult::Failed if the table is uninitialized or the key is invalid (0).
    // ------------------------------------------------------------------------
    InsertResult CheckAndInsert(uint64_t key)
    {
        ContextType emptyContext{};
        return CheckAndInsert(key, emptyContext, InsertPolicy::KeepExisting, nullptr, nullptr, nullptr);
    }

    InsertResult CheckAndInsert(uint64_t    key,
        ContextType context)
    {
        return CheckAndInsert(key, context, InsertPolicy::Overwrite, nullptr, nullptr, nullptr);
    }

    InsertResult CheckAndInsert(uint64_t     key,
                                ContextType  context,
                                InsertPolicy policy,
                                ContextType* outExistingContext = nullptr,
                                uint64_t* outEvictedKey      = nullptr,
                                ContextType* outEvictedContext  = nullptr)
    {
        // Explicitly report an error state to the caller for invalid states
        if (!m_shards || key == 0) [[unlikely]]
        {
            return InsertResult::Failed;
        }

        uint64_t hash = Hasher(key);
        uint32_t shardIndex = static_cast<uint32_t>((hash >> 48) ^ (hash >> 56)) & (m_shardCount - 1);
        Shard* shard      = &m_shards[shardIndex];

        size_t setIndex = (hash ^ (hash >> 32)) & shard->Mask;
        CacheSetHot* hotSet  = &shard->HotSets[setIndex];
        CacheSetCold* coldSet = shard->ColdSets;

        uint32_t globalRetries = 0;

        while (true)
        {
            int hitIndex = FindHitIndex(hotSet, key);

            if (hitIndex != -1) [[unlikely]] // Update existing entry path
            {
                if constexpr (TContextSize > 0)
                {
                    // Pre-fetch cold data early while the sequence lock loops
                    CACHE_PREFETCH(const_cast<ContextType*>(&coldSet[setIndex].Contexts[hitIndex]));
                }

                uint64_t seq = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);

                while (true)
                {
                    if ((seq & 1) == 0) [[likely]] // Lock is likely unheld
                    {
                        if (hotSet->Seqs[hitIndex].compare_exchange_weak(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed)) [[likely]]
                        {
                            // Post-lock validation: confirm key wasn't deleted or changed by a concurrent writer
                            if (hotSet->Keys[hitIndex].load(std::memory_order_relaxed) == key) [[likely]]
                            {
                                if constexpr (TContextSize > 0)
                                {
                                    if (outExistingContext != nullptr)
                                    {
                                        *outExistingContext = CacheContextTraits<TContextSize>::Read(&coldSet[setIndex].Contexts[hitIndex]);
                                    }

                                    if (policy == InsertPolicy::Overwrite)
                                    {
                                        CacheContextTraits<TContextSize>::Write(&coldSet[setIndex].Contexts[hitIndex], context);
                                    }
                                }

                                // True publish point. Implicitly releases the prior relaxed payload writes.
                                hotSet->Seqs[hitIndex].store(seq + 2, std::memory_order_release);

                                return InsertResult::Updated;
                            }

                            hotSet->Seqs[hitIndex].store(seq + 2, std::memory_order_release);
                            break;
                        }
                    }
                    else [[unlikely]] // Lock contention
                    {
                        ExecuteTieredBackoff(globalRetries++);
                        seq = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);
                    }
                }

                continue;
            }

            int victimIndex = FindEmptySlotIndex(hotSet);

            if (victimIndex == -1) [[unlikely]]
            {
                // Use higher bits of PRNG state to avoid poor entropy bias in lower bits.
                // This prevents hash collision storms from deterministically thrashing the same slot.
                victimIndex = (FastThreadLocalPRNG() >> 29) & 7;
            }

            uint64_t seq = hotSet->Seqs[victimIndex].load(std::memory_order_relaxed);
            if ((seq & 1) == 0) [[likely]]
            {
                if (hotSet->Seqs[victimIndex].compare_exchange_weak(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed)) [[likely]]
                {
                    // --------------------------------------------------------------------
                    // Lock-Free Set-Level Abort (Duplicate Race Mitigation)
                    // If we initially scanned and found the key didn't exist, we must re-verify.
                    // If ANY other slot is currently locked (odd sequence) or already contains 
                    // the key, another thread might be actively inserting the exact same key. 
                    // We must mathematically guarantee mutual exclusion to prevent duplicate 
                    // keys from polluting the set. We drop our lock and abort.
                    // --------------------------------------------------------------------
                    bool collisionOrLocked = false;

                    for (int verifyIdx = 0; verifyIdx < 8; ++verifyIdx)
                    {
                        if (verifyIdx == victimIndex)
                        {
                            continue;
                        }

                        uint64_t seqVerify = hotSet->Seqs[verifyIdx].load(std::memory_order_acquire);
                        if ((seqVerify & 1) != 0)
                        {
                            collisionOrLocked = true;
                            break;
                        }

                        // Use an acquire fence to ensure the sequence state was verified before reading the key
                        std::atomic_thread_fence(std::memory_order_acquire);

                        if (hotSet->Keys[verifyIdx].load(std::memory_order_relaxed) == key)
                        {
                            collisionOrLocked = true;
                            break;
                        }
                    }

                    if (collisionOrLocked) [[unlikely]]
                    {
                        // Another thread is interacting with this set. Drop lock and restart state machine.
                        hotSet->Seqs[victimIndex].store(seq + 2, std::memory_order_release);
                        continue;
                    }

                    // Eviction capture logic
                    uint64_t oldKey = hotSet->Keys[victimIndex].load(std::memory_order_relaxed);
                    if (oldKey != 0)
                    {
                        if (outEvictedKey)
                        {
                            *outEvictedKey = oldKey;
                        }

                        if constexpr (TContextSize > 0)
                        {
                            if (outEvictedContext)
                            {
                                *outEvictedContext = CacheContextTraits<TContextSize>::Read(&coldSet[setIndex].Contexts[victimIndex]);
                            }
                        }
                    }

                    if constexpr (TContextSize > 0)
                    {
                        CacheContextTraits<TContextSize>::Write(&coldSet[setIndex].Contexts[victimIndex], context);
                    }

                    // Ensure context writes are fully globally visible before the key is published
                    std::atomic_thread_fence(std::memory_order_release);

                    hotSet->Keys[victimIndex].store(key, std::memory_order_relaxed);

                    // Release the slot lock. This formally publishes the key and context.
                    hotSet->Seqs[victimIndex].store(seq + 2, std::memory_order_release);

                    return InsertResult::Inserted;
                }
            }

            ExecuteTieredBackoff(globalRetries++);
        }
    }

    // ------------------------------------------------------------------------
    // LookupContext
    // Wait-free lookup using the canonical SeqLock read protocol. Verifies 
    // sequence parity before and after the read to detect concurrent writer 
    // modifications, retrying seamlessly on state tears.
    // 
    // Parameters:
    //   key        - The unique 64-bit identifier to search for. Must not be 0.
    //   outContext - Reference to a variable where the payload will be safely copied if the key is found.
    //
    // Returns:
    //   true if the key was found and a valid context was read into outContext.
    //   false if the key does not exist or the provided key is invalid (0).
    // ------------------------------------------------------------------------
    bool LookupContext(uint64_t     key,
                       ContextType& outContext) requires (TContextSize > 0)
    {
        ContextType emptyContext{};
        outContext = emptyContext;

        if (!m_shards || key == 0) [[unlikely]]
        {
            return false;
        }

        uint64_t hash = Hasher(key);
        uint32_t shardIndex = static_cast<uint32_t>((hash >> 48) ^ (hash >> 56)) & (m_shardCount - 1);
        Shard* shard      = &m_shards[shardIndex];

        size_t setIndex = (hash ^ (hash >> 32)) & shard->Mask;
        CacheSetHot* hotSet  = &shard->HotSets[setIndex];
        CacheSetCold* coldSet = shard->ColdSets;

        int hitIndex = FindHitIndex(hotSet, key);

        if (hitIndex != -1) [[likely]]
        {
            // Pre-fetch cold data early while the CPU executes the sequence lock instructions
            CACHE_PREFETCH(const_cast<ContextType*>(&coldSet[setIndex].Contexts[hitIndex]));

            uint64_t    seq1;
            uint64_t    seq2;
            ContextType tempContext;
            uint64_t    verifyKey;

            do
            {
                seq1 = hotSet->Seqs[hitIndex].load(std::memory_order_acquire);

                if ((seq1 & 1) != 0) [[unlikely]]
                {
                    YieldProcessorThread();
                    continue;
                }

                // --------------------------------------------------------------------                
                // Data reads are marked 'relaxed' to maximize throughput because the 
                // sequence load implicitly acquires. We then follow up with an explicit 
                // acquire fence BEFORE seq2 to ensure seq2 is never evaluated early on 
                // Weak-MMU architectures like ARM64.
                // --------------------------------------------------------------------
                tempContext = CacheContextTraits<TContextSize>::Read(&coldSet[setIndex].Contexts[hitIndex]);
                verifyKey = hotSet->Keys[hitIndex].load(std::memory_order_relaxed);

                std::atomic_thread_fence(std::memory_order_acquire);

                seq2 = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);

            } while (seq1 != seq2 || (seq1 & 1) != 0);

            if (verifyKey == key) [[likely]]
            {
                outContext = tempContext;

                return true;
            }
        }

        return false;
    }

    // ------------------------------------------------------------------------
    // Contains
    // Wait-free key verification using the SeqLock protocol. Ideal for 
    // context-free or HashSet-style usage where only key existence matters.
    // 
    // Parameters:
    //   key - The unique 64-bit identifier to verify. Must not be 0.
    //
    // Returns:
    //   true if the key is currently present in the cache.
    //   false if the key does not exist, the table is uninitialized, or the key is invalid (0).
    // ------------------------------------------------------------------------
    bool Contains(uint64_t key) const
    {
        if (!m_shards || key == 0) [[unlikely]]
        {
            return false;
        }

        uint64_t hash = Hasher(key);
        uint32_t shardIndex = static_cast<uint32_t>((hash >> 48) ^ (hash >> 56)) & (m_shardCount - 1);
        Shard* shard      = &m_shards[shardIndex];

        size_t setIndex = (hash ^ (hash >> 32)) & shard->Mask;
        CacheSetHot* hotSet = &shard->HotSets[setIndex];

        int hitIndex = FindHitIndex(hotSet, key);
        if (hitIndex != -1) [[likely]]
        {
            uint64_t seq1;
            uint64_t seq2;
            uint64_t verifyKey;

            do
            {
                seq1 = hotSet->Seqs[hitIndex].load(std::memory_order_acquire);
                if ((seq1 & 1) != 0) [[unlikely]]
                {
                    YieldProcessorThread();
                    continue;
                }

                verifyKey = hotSet->Keys[hitIndex].load(std::memory_order_relaxed);

                std::atomic_thread_fence(std::memory_order_acquire);

                seq2 = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);

            } while (seq1 != seq2 || (seq1 & 1) != 0);

            if (verifyKey == key) [[likely]]
            {
                return true;
            }
        }

        return false;
    }

    // ------------------------------------------------------------------------
    // Delete
    // Atomic removal using the sequence lock write protocol. Safely zeroes 
    // the key and payload while simultaneously incrementing the sequence to 
    // notify any optimistic readers of the change.
    // 
    // Parameters:
    //   key               - The unique 64-bit identifier of the entry to remove. Must not be 0.
    //   outDeletedContext - (Optional) Pointer to a variable that will receive a copy of the 
    //                       payload data just before it is zeroed out/deleted.
    //
    // Returns:
    //   true if the key was successfully located and removed.
    //   false if the key was not found or the table is uninitialized.
    // ------------------------------------------------------------------------
    bool Delete(uint64_t     key,
                ContextType* outDeletedContext = nullptr)
    {
        if (!m_shards || key == 0) [[unlikely]]
        {
            return false;
        }

        uint64_t hash = Hasher(key);
        uint32_t shardIndex = static_cast<uint32_t>((hash >> 48) ^ (hash >> 56)) & (m_shardCount - 1);
        Shard* shard      = &m_shards[shardIndex];

        size_t setIndex = (hash ^ (hash >> 32)) & shard->Mask;
        CacheSetHot* hotSet  = &shard->HotSets[setIndex];
        CacheSetCold* coldSet = shard->ColdSets;

        int hitIndex = FindHitIndex(hotSet, key);
        if (hitIndex != -1) [[likely]]
        {
            if constexpr (TContextSize > 0)
            {
                if (outDeletedContext != nullptr)
                {
                    CACHE_PREFETCH(const_cast<ContextType*>(&coldSet[setIndex].Contexts[hitIndex]));
                }
            }

            uint64_t seq     = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);
            uint32_t retries = 0;

            while (true)
            {
                if ((seq & 1) == 0) [[likely]]
                {
                    if (hotSet->Seqs[hitIndex].compare_exchange_weak(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed)) [[likely]]
                    {
                        if (hotSet->Keys[hitIndex].load(std::memory_order_relaxed) == key) [[likely]]
                        {
                            if constexpr (TContextSize > 0)
                            {
                                if (outDeletedContext != nullptr)
                                {
                                    *outDeletedContext = CacheContextTraits<TContextSize>::Read(&coldSet[setIndex].Contexts[hitIndex]);
                                }

                                ContextType emptyContext{};
                                CacheContextTraits<TContextSize>::Write(&coldSet[setIndex].Contexts[hitIndex], emptyContext);
                            }

                            hotSet->Keys[hitIndex].store(0, std::memory_order_relaxed);

                            // Publish the zeroed key to invalidate concurrent readers
                            hotSet->Seqs[hitIndex].store(seq + 2, std::memory_order_release);

                            return true;
                        }

                        // Key mismatch (it was deleted/overwritten while we spun). Unlock and return false.
                        hotSet->Seqs[hitIndex].store(seq + 2, std::memory_order_release);

                        return false;
                    }
                }
                else [[unlikely]]
                {
                    ExecuteTieredBackoff(retries++);
                    seq = hotSet->Seqs[hitIndex].load(std::memory_order_relaxed);
                }
            }
        }

        return false;
    }

    // ------------------------------------------------------------------------
    // Enumerate
    // Iterates over all active slots across all shards and invokes the provided 
    // callback. Uses the exact same SeqLock retry protocol as LookupContext 
    // to guarantee an accurate, tear-free snapshot of each slot.
    // 
    // Parameters:
    //   callback - A function pointer invoked for each valid, non-zero key found.
    //              The callback receives the key, its payload context, and the userData.
    //   userData - An opaque pointer passed directly to the callback. Useful for 
    //              passing capturing objects (like std::vector or counters) into 
    //              the static callback scope.
    // ------------------------------------------------------------------------
    void Enumerate(EnumerateCallback callback,
                   void* userData)
    {
        if (!m_shards || !callback) [[unlikely]]
        {
            return;
        }

        for (uint32_t i = 0; i < m_shardCount; ++i)
        {
            size_t numSets = m_shards[i].Mask + 1;

            for (size_t setIdx = 0; setIdx < numSets; ++setIdx)
            {
                CacheSetHot* hotSet  = &m_shards[i].HotSets[setIdx];
                CacheSetCold* coldSet = m_shards[i].ColdSets;

                for (int slot = 0; slot < 8; ++slot)
                {
                    uint64_t    seq1;
                    uint64_t    seq2;
                    ContextType context{};
                    uint64_t    key;

                    do
                    {
                        seq1 = hotSet->Seqs[slot].load(std::memory_order_acquire);
                        if ((seq1 & 1) != 0) [[unlikely]]
                        {
                            YieldProcessorThread();
                            continue;
                        }

                        if constexpr (TContextSize > 0)
                        {
                            context = CacheContextTraits<TContextSize>::Read(&coldSet[setIdx].Contexts[slot]);
                        }

                        key = hotSet->Keys[slot].load(std::memory_order_relaxed);

                        std::atomic_thread_fence(std::memory_order_acquire);

                        seq2 = hotSet->Seqs[slot].load(std::memory_order_relaxed);

                    } while (seq1 != seq2 || (seq1 & 1) != 0);

                    if (key != 0) [[likely]]
                    {
                        callback(key, context, userData);
                    }
                }
            }
        }
    }
};