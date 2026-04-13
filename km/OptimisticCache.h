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

#ifdef _KERNEL_MODE
#include <intrin.h>
#include <ntddk.h>
#endif

// ----------------------------------------------------------------------------
// Cross-Platform Prefetch Helpers
// ----------------------------------------------------------------------------
#if defined(_M_AMD64) || defined(_M_IX86)
#include <xmmintrin.h>
#define CACHE_PREFETCH(ptr) _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T0)
#elif defined(_M_ARM64)
#define CACHE_PREFETCH(ptr) __prefetch(ptr)
#else
#define CACHE_PREFETCH(ptr) ((void)0)
#endif

// ----------------------------------------------------------------------------
// Memory Ordering Macros for x64/ARM64 Performance Parity
// ----------------------------------------------------------------------------
#if defined(_M_ARM64)
#define SEQUENCE_LOAD_ACQUIRE(ptr) ReadULong64Acquire(reinterpret_cast<volatile ULONG64*>(ptr))
#define SEQUENCE_HARDWARE_FENCE() KeMemoryBarrier()
#else
#define SEQUENCE_LOAD_ACQUIRE(ptr) (*(ptr))
#define SEQUENCE_HARDWARE_FENCE() _ReadWriteBarrier()
#endif

// ----------------------------------------------------------------------------
// Context128
// 128-bit Context Structure for high-density payloads.
// ----------------------------------------------------------------------------
struct alignas(16) Context128
{
    UINT64 Low;
    UINT64 High;
};

// ----------------------------------------------------------------------------
// Context0 for Context-free implementations
// ----------------------------------------------------------------------------
struct Context0
{
};

// ----------------------------------------------------------------------------
// Cache Context Traits
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
struct CacheContextTraits;

template <>
struct CacheContextTraits<0>
{
    using Type = Context0;

    static __forceinline Type Read(_In_opt_ volatile void*)
    {
        return {};
    }

    static __forceinline void Write(_Inout_opt_ volatile void*, _In_ Type)
    {
    }

    static __forceinline BOOLEAN IsEmpty(_In_ Type)
    {
        return FALSE;
    }
};

template <>
struct CacheContextTraits<64>
{
    using Type = UINT64;

    static __forceinline Type Read(_In_opt_ volatile void* pSource)
    {
        return *reinterpret_cast<volatile Type*>(pSource);
    }

    static __forceinline void Write(_Inout_opt_ volatile void* pDest, _In_ Type Value)
    {
        *reinterpret_cast<volatile Type*>(pDest) = Value;
    }

    static __forceinline BOOLEAN IsEmpty(_In_ Type Value)
    {
        return Value == 0;
    }
};

template <>
struct CacheContextTraits<128>
{
    using Type = Context128;

    static __forceinline Type Read(_In_opt_ volatile void* pSource)
    {
        return *const_cast<const Type*>(reinterpret_cast<volatile Type*>(pSource));
    }

    static __forceinline void Write(_Inout_opt_ volatile void* pDest, _In_ Type Value)
    {
        *const_cast<Type*>(reinterpret_cast<volatile Type*>(pDest)) = Value;
    }

    static __forceinline BOOLEAN IsEmpty(_In_ Type Value)
    {
        return (Value.Low | Value.High) == 0;
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
//   ulRetries. Writers lock a slot by atomically incrementing the sequence to an 
//   odd number, perform the write, and release by incrementing to an even number.
// 
// - Hot/Cold Segregation: Cache memory is split. Metadata (Keys and Sequences) 
//   are packed into "Hot" 64-byte blocks to maximize L1 search density. Payload 
//   data is isolated in "Cold" blocks. This prevents large payloads from 
//   polluting the CPU cache during hash collisions or cache misses.
// 
// - Tiered Backoff: When threads encounter a locked sequence (contention), they 
//   enter an escalating backoff loop. This transitions from lightweight hardware 
//   hints to OS scheduler yields, and finally hard processor stalls to prevent 
//   livelock and priority inversion under heavy oversubscription.
// ----------------------------------------------------------------------------

template <SIZE_T TContextSize>
class COptimisticCache
{
    static_assert(TContextSize == 0 || TContextSize == 64 || TContextSize == 128, "Context size must be 0, 64 or 128 bits.");

public:
    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    enum class InsertPolicy
    {
        KeepExisting,
        Overwrite
    };

    enum class InsertResult
    {
        Inserted,
        Updated,
        Failed
    };

    typedef void (*EnumerateCallback)(_In_ UINT64      ullKey,
                                      _In_ ContextType Context,
                                      _In_ PVOID       pvUserContext);

private:
    static const ULONG POOL_TAG = 'CLqS';

    // Internal SFINAE helper to avoid <type_traits> header dependency in pure WDK environments.
    template <bool B, class T = void>
    struct EnableIf
    {
    };

    template <class T>
    struct EnableIf<true, T>
    {
        typedef T Type;
    };

    template <bool B, class T = void>
    using EnableIf_t = typename EnableIf<B, T>::Type;

    // ------------------------------------------------------------------------
    // CacheSetHot (L1 Cache Optimized)
    // Hot search metadata. Packed sequentially to maximize L1 search density.
    // Contains only the minimum data needed to verify a match and sequence state.
    // ------------------------------------------------------------------------
    struct alignas(64) CacheSetHot
    {
        struct OptimizedSlot
        {
            volatile UINT64 Sequence;
            volatile UINT64 Key;
        } Slots[8];
    };

    // ------------------------------------------------------------------------
    // CacheSetCold
    // Cold payload data. Segregated to prevent cache pollution. Accessed only 
    // when a definitive key match occurs in the corresponding Hot set.
    // ------------------------------------------------------------------------
    struct alignas(64) CacheSetCold
    {
        volatile ContextType Contexts[8];
    };

    // ------------------------------------------------------------------------
    // Cache Shard
    // Aligned to 64 bytes to prevent false sharing across CPU cache lines.
    // Independently manages its assigned portion of the hot/cold sets.
    // ------------------------------------------------------------------------
    struct alignas(64) Shard
    {
        PVOID         pRawAllocation;  // Base pointer for accurate non-paged pool deallocation
        CacheSetHot*  pSetsHot;        // Pointer to the contiguous block of Hot metadata sets
        CacheSetCold* pSetsCold;       // Pointer to the contiguous block of Cold payload sets
        SIZE_T        uMask;           // Bitwise mask used to rapidly route hashes to specific buckets
        SIZE_T        uAllocationSize; // Track exactly how much memory was allocated
    };

    Shard* m_pShards;                  // Aligned array of cache shards used to distribute workload and minimize lock contention
    PVOID  m_pShardsBase;              // Raw pointer for proper NUMA allocator deallocation
    ULONG  m_ulShardCount;             // Total number of shards, guaranteed to be a power of two for fast bitwise hash routing

    // ------------------------------------------------------------------------
    // Hasher
    // Fast avalanche mixer (SplitMix64 variant) for high-entropy key distribution
    // ------------------------------------------------------------------------
    static __forceinline UINT64 Hasher(_In_ UINT64 ullData)
    {
        UINT64 ullMixed = ullData ^ (ullData >> 30);
        ullMixed *= 0xbf58476d1ce4e5b9ULL;
        ullMixed ^= (ullMixed >> 27);
        ullMixed *= 0x94d049bb133111ebULL;
        ullMixed ^= (ullMixed >> 31);

        return ullMixed;
    }

    // ------------------------------------------------------------------------
    // ExecuteTieredBackoff
    // Optimized Tiered Backoff hardened for oversubscription.
    // Transitions smoothly from hardware hints to scheduler yields, and finally 
    // to hard execution stalls to prevent priority inversion under extreme load.
    // ------------------------------------------------------------------------
    static __forceinline void ExecuteTieredBackoff(_In_ ULONG ululRetries)
    {
        if (ululRetries < 64)
        {
            YieldProcessor();
            return;
        }

        if (ululRetries < 256 || KeShouldYieldProcessor())
        {
            KIRQL currentIrql = KeGetCurrentIrql();

            if (currentIrql == PASSIVE_LEVEL)
            {
                typedef NTSTATUS(NTAPI* PZW_YIELD_EXECUTION)(VOID);

                // --------------------------------------------------------------------
                // Statics Thread-Safety
                // Standard C++ static initializers are not reliably thread-safe in the WDK.
                // We must use InterlockedCompareExchangePointer to prevent concurrent 
                // threads from overwriting the resolved function pointer.
                // --------------------------------------------------------------------
                static volatile PVOID pZwYieldExecutionCached = nullptr;
                if (pZwYieldExecutionCached == nullptr)
                {
                    UNICODE_STRING routineName;
                    RtlInitUnicodeString(&routineName, L"ZwYieldExecution");
                    PVOID pResolved = MmGetSystemRoutineAddress(&routineName);
                    if (pResolved)
                    {
                        InterlockedCompareExchangePointer(&pZwYieldExecutionCached, pResolved, nullptr);
                    }
                }

                if (pZwYieldExecutionCached)
                {
                    reinterpret_cast<PZW_YIELD_EXECUTION>(pZwYieldExecutionCached)();
                    return;
                }
            }
            else if (currentIrql <= APC_LEVEL)
            {
                // --------------------------------------------------------------------
                // APC_LEVEL Soft-Lock Prevention
                // At APC_LEVEL, YieldProcessor() (which issues a PAUSE instruction) 
                // does NOT yield the OS scheduler. If the thread holding the sequence 
                // lock is suspended at PASSIVE_LEVEL on this same core, the APC_LEVEL 
                // thread will spin forever, causing a priority inversion soft-lock.
                // By explicitly using KeDelayExecutionThread here, we allow the 
                // PASSIVE_LEVEL thread to resume and release the lock.
                // --------------------------------------------------------------------
                LARGE_INTEGER timeout;
                timeout.QuadPart = -10000; // 1ms relative timeout
                KeDelayExecutionThread(KernelMode, FALSE, &timeout);

                return;
            }

            // Only hit this fallback if we are at DISPATCH_LEVEL and < 256 retries
            YieldProcessor();

            return;
        }

        // Hard stall for DISPATCH_LEVEL threads that have exhausted the spin count
        KeStallExecutionProcessor(1);
    }

public:
    COptimisticCache() : m_pShards(nullptr),
                         m_pShardsBase(nullptr),
                         m_ulShardCount(0)
    {
    }

    ~COptimisticCache()
    {
        PAGED_CODE();
        Cleanup();
    }

    // ------------------------------------------------------------------------
    // Initialize
    // Allocates shard memory using NUMA-aware non-paged pool allocations. 
    // Distributes memory physically across active nodes to maximize bus bandwidth.
    // 
    // Parameters:
    //   uTotalEntries - The requested total capacity of the cache. The underlying 
    //                   implementation will round this value up to ensure it is 
    //                   evenly divisible by the calculated shard count and 8-slot
    //                   sets.
    //
    // Returns:
    //   TRUE if initialization and pool allocations succeeded.
    //   FALSE if pool allocation fails due to insufficient system resources.
    // ------------------------------------------------------------------------
    BOOLEAN Initialize(_In_ SIZE_T uTotalEntries)
    {
        PAGED_CODE();

        Cleanup();

        // Sharding logic to maximize bandwidth across memory controllers
        ULONG ulNumProcs = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        if (ulNumProcs == 0)
        {
            ulNumProcs = 4; // Fallback edge case if OS fails to report cores
        }

        ULONG ulTargetShards = ulNumProcs * 4;

        m_ulShardCount = 1;
        while (m_ulShardCount < ulTargetShards)
        {
            m_ulShardCount <<= 1;
        }

        if (m_ulShardCount > 512)
        {
            m_ulShardCount = 512; // Cap shards to prevent extreme memory fragmentation
        }

        POOL_EXTENDED_PARAMETER shardExtParams = { 0 };
        shardExtParams.Type          = PoolExtendedParameterNumaNode;
        shardExtParams.Optional      = 1;
        shardExtParams.PreferredNode = KeGetCurrentNodeNumber();

        m_pShardsBase = ExAllocatePool3(POOL_FLAG_NON_PAGED, (sizeof(Shard) * m_ulShardCount) + 63, POOL_TAG, &shardExtParams, 1);
        if (m_pShardsBase == nullptr)
        {
            return FALSE;
        }

        m_pShards = reinterpret_cast<Shard*>((reinterpret_cast<ULONG_PTR>(m_pShardsBase) + 63) & ~63ULL);

        // Ensure m_pShards array is zeroed so Cleanup() doesn't encounter garbage pointers on partial failure
        RtlZeroMemory(m_pShards, sizeof(Shard) * m_ulShardCount);

        SIZE_T uSets = 1;

        while ((uSets * 8) < (uTotalEntries / m_ulShardCount))
        {
            uSets <<= 1;
        }

        USHORT usHighestNode = KeQueryHighestNodeNumber();
        ULONG  ulMaxNodes    = usHighestNode + 1;

        USHORT* pusActiveNodes    = static_cast<USHORT*>(ExAllocatePool2(POOL_FLAG_PAGED, ulMaxNodes * sizeof(USHORT), POOL_TAG));
        ULONG   ulActiveNodeCount = 0;

        if (pusActiveNodes)
        {
            for (USHORT usNode = 0; usNode <= usHighestNode; ++usNode)
            {
                GROUP_AFFINITY Affinity = { 0 };
                KeQueryNodeActiveAffinity(usNode, &Affinity, NULL);

                if (Affinity.Mask != 0)
                {
                    pusActiveNodes[ulActiveNodeCount++] = usNode;
                }
            }
        }

        if (ulActiveNodeCount == 0)
        {
            ulActiveNodeCount = ulMaxNodes;
        }

        for (ULONG i = 0; i < m_ulShardCount; ++i)
        {
            USHORT usTargetNode;

            if (pusActiveNodes && pusActiveNodes[i % ulActiveNodeCount] <= usHighestNode)
            {
                usTargetNode = pusActiveNodes[i % ulActiveNodeCount];
            }
            else
            {
                usTargetNode = (USHORT)(i % ulActiveNodeCount);
            }

            POOL_EXTENDED_PARAMETER extendedParams = { 0 };
            extendedParams.Type          = PoolExtendedParameterNumaNode;
            extendedParams.Optional      = 1;
            extendedParams.PreferredNode = usTargetNode;

            SIZE_T cbHot = uSets * sizeof(CacheSetHot);
            SIZE_T cbCold = 0;

            if constexpr (TContextSize > 0)
            {
                cbCold = uSets * sizeof(CacheSetCold);
            }

            PVOID pRaw = ExAllocatePool3(POOL_FLAG_NON_PAGED, cbHot + cbCold + 63, POOL_TAG, &extendedParams, 1);
            if (pRaw == nullptr)
            {
                if (pusActiveNodes)
                {
                    ExFreePoolWithTag(pusActiveNodes, POOL_TAG);
                }

                // --------------------------------------------------------------------                
                // If we fail to allocate halfway through the shard loop, we MUST 
                // invoke Cleanup() to unwind and free m_pShardsBase and any previously 
                // successful raw allocations before returning.
                // --------------------------------------------------------------------
                Cleanup();

                return FALSE;
            }

            m_pShards[i].pRawAllocation  = pRaw;
            m_pShards[i].pSetsHot        = reinterpret_cast<CacheSetHot*>((reinterpret_cast<ULONG_PTR>(pRaw) + 63) & ~63ULL);
            m_pShards[i].pSetsCold       = nullptr;
            m_pShards[i].uMask           = uSets - 1;
            m_pShards[i].uAllocationSize = cbHot + cbCold + 63;

            RtlZeroMemory(m_pShards[i].pSetsHot, cbHot);

            if constexpr (TContextSize > 0)
            {
                m_pShards[i].pSetsCold = reinterpret_cast<CacheSetCold*>(reinterpret_cast<ULONG_PTR>(m_pShards[i].pSetsHot) + cbHot);
                RtlZeroMemory(m_pShards[i].pSetsCold, cbCold);
            }
        }

        if (pusActiveNodes)
        {
            ExFreePoolWithTag(pusActiveNodes, POOL_TAG);
        }

        return TRUE;
    }

    // ------------------------------------------------------------------------
    // GetMemoryUsage
    // Calculates the total bytes allocated by the cache across all NUMA nodes.
    // ------------------------------------------------------------------------
    SIZE_T GetMemoryUsage() const
    {
        SIZE_T uTotal = 0;

        if (m_pShardsBase != nullptr)
        {
            uTotal += (sizeof(Shard) * m_ulShardCount) + 63;
        }

        if (m_pShards != nullptr)
        {
            for (ULONG i = 0; i < m_ulShardCount; ++i)
            {
                uTotal += m_pShards[i].uAllocationSize;
            }
        }

        return uTotal;
    }

    // ------------------------------------------------------------------------
    // Cleanup
    // Safely releases all NUMA-allocated shard and array memory back to the 
    // system pool. Caller must ensure no active operations are occurring.
    // ------------------------------------------------------------------------
    void Cleanup()
    {
        PAGED_CODE();

        if (m_pShards != nullptr)
        {
            for (ULONG i = 0; i < m_ulShardCount; ++i)
            {
                if (m_pShards[i].pRawAllocation != nullptr)
                {
                    ExFreePoolWithTag(m_pShards[i].pRawAllocation, POOL_TAG);
                }
            }
        }

        if (m_pShardsBase != nullptr)
        {
            ExFreePoolWithTag(m_pShardsBase, POOL_TAG);
        }

        m_pShards = nullptr;
        m_pShardsBase = nullptr;
    }

    // ------------------------------------------------------------------------
    // LookupContext
    // Wait-free lookup using the canonical SeqLock read protocol. Verifies 
    // sequence parity before and after the read to detect concurrent writer 
    // modifications, retrying seamlessly on state tears.
    // 
    // Parameters:
    //   ullKey      - The unique 64-bit identifier to search for. Must not be 0.
    //   rOutContext - Reference to a variable where the payload will be safely 
    //                 copied if the key is found.
    //
    // Returns:
    //   TRUE if the key was found and a valid context was read into rOutContext.
    //   FALSE if the key does not exist or the provided key is invalid (0).
    // ------------------------------------------------------------------------
    template <SIZE_T Size = TContextSize,
              typename = EnableIf_t<Size != 0>>
    BOOLEAN LookupContext(_In_  UINT64       ullKey,
                          _Out_ ContextType& rOutContext)
    {
        if (m_pShards == nullptr || ullKey == 0)
        {
            return FALSE;
        }

        UINT64 hash   = Hasher(ullKey);
        Shard* pShard = &m_pShards[static_cast<ULONG>((hash >> 48) ^ (hash >> 56)) & (m_ulShardCount - 1)];

        CacheSetHot*  pHot  = &pShard->pSetsHot[hash & pShard->uMask];
        CacheSetCold* pCold = pShard->pSetsCold;

        for (int i = 0; i < 8; ++i)
        {
            // Optimistic pre-fetch based on a relaxed key read before the seq lock loop
            if (pHot->Slots[i].Key == ullKey)
            {
                CACHE_PREFETCH(const_cast<ContextType*>(&pCold[hash & pShard->uMask].Contexts[i]));
            }

            UINT64 seq1, seq2, verifyKey;
            ContextType ctx{};
            ULONG ulRetries = 0;

            do
            {
                seq1 = SEQUENCE_LOAD_ACQUIRE(&pHot->Slots[i].Sequence);
                while (seq1 & 1)
                {
                    ExecuteTieredBackoff(ulRetries++);
                    seq1 = pHot->Slots[i].Sequence;
                }

                SEQUENCE_HARDWARE_FENCE();

                verifyKey = pHot->Slots[i].Key;
                if (verifyKey != ullKey)
                {
                    break;
                }

                ctx = CacheContextTraits<TContextSize>::Read(&pCold[hash & pShard->uMask].Contexts[i]);

                SEQUENCE_HARDWARE_FENCE();
                seq2 = pHot->Slots[i].Sequence;

            } while (seq1 != seq2);

            if (verifyKey == ullKey && !CacheContextTraits<TContextSize>::IsEmpty(ctx))
            {
                rOutContext = ctx;

                return TRUE;
            }
        }

        return FALSE;
    }

    // ------------------------------------------------------------------------
    // Contains
    // Wait-free key verification using the SeqLock protocol. Ideal for 
    // context-free or HashSet-style usage where only key existence matters.
    // 
    // Parameters:
    //   ullKey - The unique 64-bit identifier to verify. Must not be 0.
    //
    // Returns:
    //   TRUE  if the key is currently present in the cache.
    //   FALSE if the key does not exist, the table is uninitialized, or the 
    //         key is invalid (0).
    // ------------------------------------------------------------------------
    BOOLEAN Contains(_In_ UINT64 ullKey) const
    {
        if (m_pShards == nullptr || ullKey == 0)
        {
            return FALSE;
        }

        UINT64       hash   = Hasher(ullKey);
        Shard*       pShard = &m_pShards[static_cast<ULONG>((hash >> 48) ^ (hash >> 56)) & (m_ulShardCount - 1)];
        CacheSetHot* pHot   = &pShard->pSetsHot[hash & pShard->uMask];

        for (int i = 0; i < 8; ++i)
        {
            UINT64 seq1, seq2, verifyKey;
            ULONG ulRetries = 0;

            do
            {
                seq1 = SEQUENCE_LOAD_ACQUIRE(&pHot->Slots[i].Sequence);
                while (seq1 & 1)
                {
                    ExecuteTieredBackoff(ulRetries++);
                    seq1 = pHot->Slots[i].Sequence;
                }

                SEQUENCE_HARDWARE_FENCE();

                verifyKey = pHot->Slots[i].Key;
                if (verifyKey != ullKey)
                {
                    break;
                }

                SEQUENCE_HARDWARE_FENCE();
                seq2 = pHot->Slots[i].Sequence;

            } while (seq1 != seq2);

            if (verifyKey == ullKey)
            {
                return TRUE;
            }
        }

        return FALSE;
    }

    // ------------------------------------------------------------------------
    // CheckAndInsert
    // Thread-safe insertion utilizing atomic sequence locks. Writes lock a 
    // specific slot by transitioning its sequence number to an odd state via 
    // InterlockedCompareExchange64.
    // 
    // Parameters:
    //   ullKey              - The unique 64-bit identifier for the entry. Must
    //                         not be 0.
    //   Context             - The payload data to store alongside the key.
    //   policy              - Determines behavior if the key already exists 
    //                         (KeepExisting or Overwrite).
    //   pOutExistingContext - (Optional) Captures the existing payload if the 
    //                                    key was already present.
    //   pOutEvictedKey      - (Optional) Captures the key of an item that was 
    //                                    forcefully evicted to make room.
    //   pOutEvictedContext  - (Optional) Captures the payload of the forcefully 
    //                                    evicted item.
    //
    // Returns:
    //   InsertResult::Inserted if a new key was successfully added.
    //   InsertResult::Updated if an existing key was successfully overwritten.
    //   InsertResult::Failed if the table is uninitialized or the key is invalid (0).
    // ------------------------------------------------------------------------
    InsertResult CheckAndInsert(_In_      UINT64       ullKey,
                                _In_      ContextType  Context,
                                _In_      InsertPolicy policy = InsertPolicy::Overwrite,
                                _Out_opt_ ContextType* pOutExistingContext = nullptr,
                                _Out_opt_ UINT64*      pOutEvictedKey = nullptr,
                                _Out_opt_ ContextType* pOutEvictedContext = nullptr)
    {
        if (m_pShards == nullptr || ullKey == 0)
        {
            return InsertResult::Failed;
        }

        UINT64 hash   = Hasher(ullKey);
        Shard* pShard = &m_pShards[static_cast<ULONG>((hash >> 48) ^ (hash >> 56)) & (m_ulShardCount - 1)];

        CacheSetHot*  pHot  = &pShard->pSetsHot[hash & pShard->uMask];
        CacheSetCold* pCold = pShard->pSetsCold;

        ULONG ulGlobalulRetries = 0;

        while (true)
        {
            int iTargetIdx = -1;
            BOOLEAN bExists = FALSE;

            for (int i = 0; i < 8; ++i)
            {
                if (pHot->Slots[i].Key == ullKey)
                {
                    iTargetIdx = i;
                    bExists = TRUE;

                    if constexpr (TContextSize > 0)
                    {
                        CACHE_PREFETCH(const_cast<ContextType*>(&pCold[hash & pShard->uMask].Contexts[i]));
                    }

                    break;
                }

                if (iTargetIdx == -1 && pHot->Slots[i].Key == 0)
                {
                    iTargetIdx = i;
                }
            }

            if (iTargetIdx == -1)
            {
                iTargetIdx = Hasher(ullKey + ulGlobalulRetries) & 7; // Improved victim selection
            }

            UINT64 seq = pHot->Slots[iTargetIdx].Sequence;

            if ((seq & 1) == 0)
            {
                // --------------------------------------------------------------------
                // Preemptible Writer Livelock Prevention
                // We MUST raise the IRQL to DISPATCH_LEVEL before locking the sequence.
                // If a writer holds the lock (odd sequence) at PASSIVE_LEVEL and gets 
                // preempted by a DPC on the same core, the DPC will spin forever waiting 
                // for the lock to release, causing a Bugcheck 0x133.
                // --------------------------------------------------------------------
                KIRQL oldIrql;
                KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);

                if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[iTargetIdx].Sequence),
                                                 static_cast<LONG64>(seq + 1),
                                                 static_cast<LONG64>(seq)) == static_cast<LONG64>(seq))
                {
                    if (bExists && pHot->Slots[iTargetIdx].Key != ullKey)
                    {
                        InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[iTargetIdx].Sequence),
                                                 1);
                        KeLowerIrql(oldIrql); // Release lock and restore IRQL

                        continue;
                    }

                    // --------------------------------------------------------------------
                    // Set-Level Deadlock & Duplicate Mitigation
                    // If we initially found the key didn't exist, we must re-verify.
                    // However, we CANNOT spin-wait for other slots in the set to unlock,
                    // because if two threads lock two different slots in the same set 
                    // and wait for each other at DISPATCH_LEVEL, they will DEADLOCK.
                    // Instead, we perform a lock-free check. If ANY other slot is locked
                    // or already contains the key, we immediately abort, drop our lock, 
                    // and retry.
                    // --------------------------------------------------------------------
                    if (!bExists)
                    {
                        BOOLEAN bCollisionOrLocked = FALSE;

                        for (int verifyIdx = 0; verifyIdx < 8; ++verifyIdx)
                        {
                            if (verifyIdx == iTargetIdx)
                            {
                                continue;
                            }

                            UINT64 seqVerify = SEQUENCE_LOAD_ACQUIRE(&pHot->Slots[verifyIdx].Sequence);
                            if (seqVerify & 1)
                            {
                                bCollisionOrLocked = TRUE;
                                break;
                            }

                            SEQUENCE_HARDWARE_FENCE();

                            if (pHot->Slots[verifyIdx].Key == ullKey)
                            {
                                bCollisionOrLocked = TRUE;
                                break;
                            }
                        }

                        if (bCollisionOrLocked)
                        {
                            // Another thread is touching this set. Drop lock and retry.
                            InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[iTargetIdx].Sequence),
                                                     1);
                            KeLowerIrql(oldIrql);

                            continue;
                        }
                    }

                    if constexpr (TContextSize > 0)
                    {
                        if (bExists && pOutExistingContext != nullptr)
                        {
                            *pOutExistingContext = CacheContextTraits<TContextSize>::Read(&pCold[hash & pShard->uMask].Contexts[iTargetIdx]);
                        }
                    }

                    if (!bExists)
                    {
                        // Eviction capture logic
                        UINT64 oldKey = pHot->Slots[iTargetIdx].Key;
                        if (oldKey != 0)
                        {
                            if (pOutEvictedKey != nullptr)
                            {
                                *pOutEvictedKey = oldKey;
                            }

                            if constexpr (TContextSize > 0)
                            {
                                if (pOutEvictedContext != nullptr)
                                {
                                    *pOutEvictedContext = CacheContextTraits<TContextSize>::Read(&pCold[hash & pShard->uMask].Contexts[iTargetIdx]);
                                }
                            }
                        }
                    }

                    if (!bExists || policy == InsertPolicy::Overwrite)
                    {
                        pHot->Slots[iTargetIdx].Key = ullKey;

                        if constexpr (TContextSize > 0)
                        {
                            CacheContextTraits<TContextSize>::Write(&pCold[hash & pShard->uMask].Contexts[iTargetIdx], Context);
                        }

                        SEQUENCE_HARDWARE_FENCE();
                    }

                    InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[iTargetIdx].Sequence), 1);
                    KeLowerIrql(oldIrql); // Successful exit, restore IRQL

                    return bExists ? InsertResult::Updated : InsertResult::Inserted;
                }

                // Lock acquisition failed, lower IRQL before entering the backoff spin
                KeLowerIrql(oldIrql);
            }

            ExecuteTieredBackoff(ulGlobalulRetries++);
        }
    }

    // ------------------------------------------------------------------------
    // Delete
    // Atomic removal using the sequence lock write protocol. Safely zeroes 
    // the key and payload while simultaneously incrementing the sequence to 
    // notify any optimistic readers of the change.
    // 
    // Parameters:
    //   ullKey             - The unique 64-bit identifier of the entry to remove. 
    //                        Must not be 0.
    //   pOutDeletedContext - (Optional) Pointer to a variable that will receive 
    //                        a copy of the payload data just before it is zeroed 
    //                        out/deleted.
    //
    // Returns:
    //   TRUE if the key was successfully located and removed.
    //   FALSE if the key was not found or the table is uninitialized.
    // ------------------------------------------------------------------------
    BOOLEAN Delete(_In_      UINT64       ullKey,
                   _Out_opt_ ContextType* pOutDeletedContext = nullptr)
    {
        if (m_pShards == nullptr || ullKey == 0)
        {
            return FALSE;
        }

        UINT64 hash   = Hasher(ullKey);
        Shard* pShard = &m_pShards[static_cast<ULONG>((hash >> 48) ^ (hash >> 56)) & (m_ulShardCount - 1)];

        CacheSetHot*  pHot  = &pShard->pSetsHot[hash & pShard->uMask];
        CacheSetCold* pCold = pShard->pSetsCold;

        ULONG ulRetries = 0;

        for (int i = 0; i < 8; ++i)
        {
            if (pHot->Slots[i].Key == ullKey)
            {
                if constexpr (TContextSize > 0)
                {
                    if (pOutDeletedContext != nullptr)
                    {
                        CACHE_PREFETCH(const_cast<ContextType*>(&pCold[hash & pShard->uMask].Contexts[i]));
                    }
                }

                while (true)
                {
                    UINT64 seq = pHot->Slots[i].Sequence;
                    if ((seq & 1) == 0)
                    {
                        // --------------------------------------------------------------------
                        // Preemptible Writer Livelock Prevention
                        // Raise IRQL to DISPATCH_LEVEL to prevent preemption while 
                        // holding the sequence lock for deletion.
                        // --------------------------------------------------------------------
                        KIRQL oldIrql;
                        KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);

                        if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[i].Sequence),
                                                         static_cast<LONG64>(seq + 1),
                                                         static_cast<LONG64>(seq)) == static_cast<LONG64>(seq))
                        {
                            if (pHot->Slots[i].Key == ullKey)
                            {
                                pHot->Slots[i].Key = 0;

                                if constexpr (TContextSize > 0)
                                {
                                    // Deletion capture logic
                                    if (pOutDeletedContext != nullptr)
                                    {
                                        *pOutDeletedContext = CacheContextTraits<TContextSize>::Read(&pCold[hash & pShard->uMask].Contexts[i]);
                                    }

                                    ContextType emptyCtx{};
                                    CacheContextTraits<TContextSize>::Write(&pCold[hash & pShard->uMask].Contexts[i], emptyCtx);
                                }

                                SEQUENCE_HARDWARE_FENCE();
                                InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[i].Sequence),
                                                         1);
                                KeLowerIrql(oldIrql);

                                return TRUE;
                            }

                            InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&pHot->Slots[i].Sequence),
                                                     1);
                            KeLowerIrql(oldIrql);

                            return FALSE;
                        }

                        // Failed to acquire, lower IRQL before backoff
                        KeLowerIrql(oldIrql);
                    }

                    ExecuteTieredBackoff(ulRetries++);
                }
            }
        }

        return FALSE;
    }

    // ------------------------------------------------------------------------
    // Enumerate
    // Iterates over all active, non-locked slots across all shards and invokes
    // the provided callback with the sequence-validated key and context. Uses 
    // the same SeqLock protocol as LookupContext to prevent torn reads.
    // 
    // Parameters:
    //   pfnCallback   - A function pointer invoked for each valid, non-zero key found.
    //                   The callback receives the key, its payload context, and pvUserContext.
    //   pvUserContext - (Optional) An opaque pointer passed directly to the callback. 
    //                   Useful for passing context structures or tracking state 
    //                   during the enumeration.
    // ------------------------------------------------------------------------
    void Enumerate(_In_     EnumerateCallback pfnCallback,
                   _In_opt_ PVOID             pvUserContext)
    {
        if (m_pShards == nullptr || pfnCallback == nullptr)
        {
            return;
        }

        for (ULONG i = 0; i < m_ulShardCount; ++i)
        {
            for (SIZE_T j = 0; j <= m_pShards[i].uMask; ++j)
            {
                CacheSetHot*  pHot  = &m_pShards[i].pSetsHot[j];
                CacheSetCold* pCold = m_pShards[i].pSetsCold;

                for (int k = 0; k < 8; ++k)
                {
                    // --------------------------------------------------------------------
                    // Phantom Skips in Enumerate
                    // We must use the identical do-while loop protocol used in LookupContext
                    // to guarantee we accurately capture the slot state and don't silently 
                    // skip over slots undergoing concurrent writes.
                    // --------------------------------------------------------------------
                    UINT64 seq1, seq2, key;
                    ContextType ctx{};
                    ULONG ulRetries = 0;

                    do
                    {
                        seq1 = SEQUENCE_LOAD_ACQUIRE(&pHot->Slots[k].Sequence);
                        while (seq1 & 1)
                        {
                            ExecuteTieredBackoff(ulRetries++);
                            seq1 = pHot->Slots[k].Sequence;
                        }

                        SEQUENCE_HARDWARE_FENCE();
                        key = pHot->Slots[k].Key;

                        if constexpr (TContextSize > 0)
                        {
                            ctx = CacheContextTraits<TContextSize>::Read(&pCold[j].Contexts[k]);
                        }

                        SEQUENCE_HARDWARE_FENCE();
                        seq2 = pHot->Slots[k].Sequence;

                    } while (seq1 != seq2);

                    if (key != 0)
                    {
                        pfnCallback(key, ctx, pvUserContext);
                    }
                }
            }
        }
    }
};