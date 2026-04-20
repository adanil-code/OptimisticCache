/*
* Apache Optimistic Cache test/Sample Driver
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

// ----------------------------------------------------------------------------
// This driver validates the correctness, concurrency safety, and performance
// of the COptimisticCache under strict Windows NT executive constraints.
// It directly ports the user-mode comprehensive multi-threaded test suite
// (excluding std:: container comparisons) into a kernel-mode benchmark.
// 
// TEST RESULTS OUTPUT:
// DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...) is used for all logging and output, 
// allowing real-time monitoring
// ----------------------------------------------------------------------------

#define TEST_IS_KM 1

#include <ntifs.h>
#include <ntstrsafe.h>
#include "OptimisticCache.h"

#define DRIVER_TAG 'CpOt'

// ----------------------------------------------------------------------------
// Global Synchronization & Protection
// ----------------------------------------------------------------------------
PETHREAD       g_pMasterBenchmarkThread = NULL;
EX_RUNDOWN_REF g_TestRundown;
volatile LONG  g_lAbortTests = 0;

// ----------------------------------------------------------------------------
// Kernel-Mode Global new/delete Overloads
// ----------------------------------------------------------------------------
void* __cdecl operator new(_In_ SIZE_T     uSize,
                           _In_ POOL_FLAGS ullPoolFlags,
                           _In_ ULONG      ulTag) noexcept
{
    return ExAllocatePool2(ullPoolFlags, uSize, ulTag);
}

void* __cdecl operator new[](_In_ SIZE_T     uSize,
                             _In_ POOL_FLAGS ullPoolFlags,
                             _In_ ULONG      ulTag) noexcept
{
    return ExAllocatePool2(ullPoolFlags, uSize, ulTag);
}

void __cdecl operator delete(_In_opt_ void* pMemory)
{
    if (pMemory)
    {
        ExFreePool(pMemory);
    }
}

void __cdecl operator delete(_In_opt_ void* pMemory,
                             _In_     SIZE_T uSize)
{
    UNREFERENCED_PARAMETER(uSize);

    if (pMemory)
    {
        ExFreePool(pMemory);
    }
}

void __cdecl operator delete[](_In_opt_ void* pMemory)
{
    if (pMemory)
    {
        ExFreePool(pMemory);
    }
}

void __cdecl operator delete[](_In_opt_ void* pMemory,
                               _In_     SIZE_T uSize)
{
    UNREFERENCED_PARAMETER(uSize);

    if (pMemory)
    {
        ExFreePool(pMemory);
    }
}

// ----------------------------------------------------------------------------
// Test Logging & Validation Macros
// ----------------------------------------------------------------------------
#define LOG_INFO(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__)
#define LOG_ERR(...)  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__)

#define TEST_REQUIRE(condition, msg, retVal) \
    do \
    { \
        if (InterlockedCompareExchange(&g_lAbortTests, 0, 0)) \
        { \
            return retVal; \
        } \
        if (!(condition)) \
        { \
            LOG_ERR("[OPT_CACHE] [!] TEST FAILED: %s (Line %d)\n", msg, __LINE__); \
            return retVal; \
        } \
    } while(0)

// ----------------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------------
inline VOID SleepMs(_In_ ULONG ulMs)
{
    LARGE_INTEGER liDelay;
    liDelay.QuadPart = -(LONGLONG)(ulMs * 10000LL);
    KeDelayExecutionThread(KernelMode, FALSE, &liDelay);
}

class FastRng
{
private:
    UINT32 m_u32State;

public:
    FastRng(_In_ UINT32 u32Seed) : m_u32State(u32Seed ? u32Seed : 0xBADF00D)
    {
    }

    UINT32 Next() noexcept
    {
        m_u32State ^= m_u32State << 13;
        m_u32State ^= m_u32State >> 17;
        m_u32State ^= m_u32State << 5;
        return m_u32State;
    }
};

template<typename T, typename Compare>
void QuickSort(_Inout_updates_(uCount) T* pBase,
               _In_                    SIZE_T  uCount,
               _In_                    Compare Comp)
{
    if (pBase == nullptr || uCount <= 1)
    {
        return;
    }

    struct Range
    {
        SIZE_T uLow;
        SIZE_T uHigh;
    };

    Range Stack[64];
    SIZE_T uSp = 0;

    Stack[uSp].uLow = 0;
    Stack[uSp].uHigh = uCount - 1;
    ++uSp;

    while (uSp > 0)
    {
        --uSp;
        Range CurrentRange = Stack[uSp];

        SIZE_T uLow = CurrentRange.uLow;
        SIZE_T uHigh = CurrentRange.uHigh;

        while (uLow < uHigh)
        {
            SIZE_T uI     = uLow;
            SIZE_T uJ     = uHigh;
            SIZE_T uPivot = uLow + ((uHigh - uLow) >> 1);

            for (;;)
            {
                while (Comp(pBase[uI], pBase[uPivot]) < 0)
                {
                    ++uI;
                }

                while (Comp(pBase[uJ], pBase[uPivot]) > 0)
                {
                    if (uJ == 0)
                    {
                        break;
                    }
                    --uJ;
                }

                if (uI >= uJ)
                {
                    break;
                }

                T Tmp = pBase[uI];
                pBase[uI] = pBase[uJ];
                pBase[uJ] = Tmp;

                if (uI == uPivot)
                {
                    uPivot = uJ;
                }
                else if (uJ == uPivot)
                {
                    uPivot = uI;
                }

                ++uI;

                if (uJ == 0)
                {
                    break;
                }
                --uJ;
            }

            SIZE_T uLeftLow   = uLow;
            SIZE_T uLeftHigh  = (uJ > 0) ? (uJ - 1) : 0;
            SIZE_T uRightLow  = uJ + 1;
            SIZE_T uRightHigh = uHigh;

            SIZE_T uLeftSize  = (uLeftHigh >= uLeftLow) ? (uLeftHigh - uLeftLow + 1) : 0;
            SIZE_T uRightSize = (uRightHigh >= uRightLow) ? (uRightHigh - uRightLow + 1) : 0;

            if (uLeftSize > 1 && uRightSize > 1)
            {
                if (uLeftSize < uRightSize)
                {
                    Stack[uSp].uLow  = uRightLow;
                    Stack[uSp].uHigh = uRightHigh;

                    ++uSp;
                    uHigh = uLeftHigh;
                    uLow  = uLeftLow;
                }
                else
                {
                    Stack[uSp].uLow  = uLeftLow;
                    Stack[uSp].uHigh = uLeftHigh;

                    ++uSp;
                    uLow  = uRightLow;
                    uHigh = uRightHigh;
                }
            }
            else if (uLeftSize > 1)
            {
                uHigh = uLeftHigh;
                uLow  = uLeftLow;
            }
            else if (uRightSize > 1)
            {
                uLow  = uRightLow;
                uHigh = uRightHigh;
            }
            else
            {
                break;
            }

            if (uSp >= RTL_NUMBER_OF(Stack))
            {
                for (SIZE_T uM = uLow + 1; uM <= uHigh; ++uM)
                {
                    SIZE_T uN = uM;
                    while (uN > uLow && Comp(pBase[uN], pBase[uN - 1]) < 0)
                    {
                        T Tmp = pBase[uN];
                        pBase[uN] = pBase[uN - 1];
                        pBase[uN - 1] = Tmp;
                        --uN;
                    }
                }
                uSp = 0;
                break;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Thread Management Utilities
// ----------------------------------------------------------------------------
#define MAX_TEST_THREADS 2048 

struct TEST_WORKER_CONTEXT
{
    ULONG          ulThreadId;
    PKEVENT        pStartEvent;
    volatile LONG* plStopFlag;
    PVOID          pUserContext;
};

typedef VOID(*PTEST_WORKER_FUNC)(_Inout_ TEST_WORKER_CONTEXT* pContext);

struct TEST_THREAD_MANAGER
{
    PETHREAD            pThreads[MAX_TEST_THREADS];
    TEST_WORKER_CONTEXT Contexts[MAX_TEST_THREADS];
    ULONG               ulThreadCount;
    KEVENT              StartEvent;
    volatile LONG       lStopFlag;
};

VOID StartThreads(_Inout_  TEST_THREAD_MANAGER* pMgr,
                  _In_     ULONG                ulCount,
                  _In_     PTEST_WORKER_FUNC    pFunc,
                  _In_opt_ PVOID                pUserContext)
{
    PAGED_CODE();

    if (ulCount > MAX_TEST_THREADS)
    {
        ulCount = MAX_TEST_THREADS;
    }

    pMgr->ulThreadCount = ulCount;
    pMgr->lStopFlag     = 0;

    KeInitializeEvent(&pMgr->StartEvent, NotificationEvent, FALSE);

    for (ULONG ulIndex = 0; ulIndex < ulCount; ++ulIndex)
    {
        pMgr->Contexts[ulIndex].ulThreadId   = ulIndex;
        pMgr->Contexts[ulIndex].pStartEvent  = &pMgr->StartEvent;
        pMgr->Contexts[ulIndex].plStopFlag   = &pMgr->lStopFlag;
        pMgr->Contexts[ulIndex].pUserContext = pUserContext;

        HANDLE hThread;
        NTSTATUS ntStatus = PsCreateSystemThread(&hThread,
                                                 THREAD_ALL_ACCESS,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 (PKSTART_ROUTINE)pFunc,
                                                 &pMgr->Contexts[ulIndex]);

        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = ObReferenceObjectByHandle(hThread,
                                                 THREAD_ALL_ACCESS,
                                                 NULL,
                                                 KernelMode,
                                                 (PVOID*)&pMgr->pThreads[ulIndex],
                                                 NULL);
            if (NT_SUCCESS(ntStatus))
            {
                KeSetPriorityThread((PKTHREAD)pMgr->pThreads[ulIndex], 15);
            }
            else
            {
                LOG_ERR("[OPT_CACHE] [!] Failed to reference thread %u. Aborting.\n", ulIndex);

                pMgr->pThreads[ulIndex] = NULL;
                InterlockedExchange(&pMgr->lStopFlag, 1);
                InterlockedExchange(&g_lAbortTests, 1);
                KeSetEvent(&pMgr->StartEvent, IO_NO_INCREMENT, FALSE);

                ZwWaitForSingleObject(hThread, FALSE, NULL);
            }

            ZwClose(hThread);
        }
        else
        {
            pMgr->pThreads[ulIndex] = NULL;
        }
    }

    KeSetEvent(&pMgr->StartEvent, IO_NO_INCREMENT, FALSE);
}

VOID StopAndWaitThreads(_Inout_ TEST_THREAD_MANAGER* pMgr,
                        _In_    int                  nSleepSeconds = 0)
{
    PAGED_CODE();

    if (nSleepSeconds > 0)
    {
        LARGE_INTEGER liDelay;
        liDelay.QuadPart = -1000000ll; // 100ms
        int nIterations  = nSleepSeconds * 10;

        for (int nIndex = 0; nIndex < nIterations; ++nIndex)
        {
            if (InterlockedCompareExchange(&g_lAbortTests, 0, 0))
            {
                break;
            }

            KeDelayExecutionThread(KernelMode, FALSE, &liDelay);
        }
    }

    InterlockedExchange(&pMgr->lStopFlag, 1);

    PVOID* pValidThreads = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) PVOID[pMgr->ulThreadCount];
    if (pValidThreads != NULL)
    {
        ULONG ulValidCount = 0;

        for (ULONG ulIndex = 0; ulIndex < pMgr->ulThreadCount; ++ulIndex)
        {
            if (pMgr->pThreads[ulIndex] != NULL)
            {
                pValidThreads[ulValidCount++] = pMgr->pThreads[ulIndex];
            }
        }

        ULONG ulRemaining = ulValidCount;
        ULONG ulOffset    = 0;

        while (ulRemaining > 0)
        {
            ULONG ulWaitCount = (ulRemaining > MAXIMUM_WAIT_OBJECTS) ? MAXIMUM_WAIT_OBJECTS : ulRemaining;
            KWAIT_BLOCK WaitBlocks[MAXIMUM_WAIT_OBJECTS];

            KeWaitForMultipleObjects(ulWaitCount,
                                     &pValidThreads[ulOffset],
                                     WaitAll,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL,
                                     WaitBlocks);

            ulRemaining -= ulWaitCount;
            ulOffset    += ulWaitCount;
        }

        delete[] pValidThreads;
    }
    else
    {
        LOG_ERR("[OPT_CACHE] [!] Failed to allocate wait array. Falling back to single waits.\n");

        for (ULONG ulIndex = 0; ulIndex < pMgr->ulThreadCount; ++ulIndex)
        {
            if (pMgr->pThreads[ulIndex] != NULL)
            {
                KeWaitForSingleObject(pMgr->pThreads[ulIndex],
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      NULL);
            }
        }
    }

    for (ULONG ulIndex = 0; ulIndex < pMgr->ulThreadCount; ++ulIndex)
    {
        if (pMgr->pThreads[ulIndex] != NULL)
        {
            ObDereferenceObject(pMgr->pThreads[ulIndex]);
        }
    }
}

VOID SetWorkerThreadAffinity(_In_ ULONG ulThreadId)
{
    ULONG ulTotalProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulTotalProcessors == 0)
    {
        ulTotalProcessors = 1;
    }

    PROCESSOR_NUMBER procNum;
    if (NT_SUCCESS(KeGetProcessorNumberFromIndex(ulThreadId % ulTotalProcessors, &procNum)))
    {
        GROUP_AFFINITY affinity = { 0 };
        affinity.Group = procNum.Group;
        affinity.Mask  = (KAFFINITY)-1; // Allow execution on any core within this group
        
        // Correctly set the group affinity for the CURRENT thread
        KeSetSystemGroupAffinityThread(&affinity, NULL);
    }
}

// ----------------------------------------------------------------------------
// Context Payload Generators & Validation
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
struct TestContextTraits;

template <>
struct TestContextTraits<0>
{
    static inline Context0 Make(UINT64 value)
    {
        UNREFERENCED_PARAMETER(value);
        return {};
    }

    static inline BOOLEAN IsMatch(Context0 ctx, UINT64 expectedValue)
    {
        UNREFERENCED_PARAMETER(ctx);
        UNREFERENCED_PARAMETER(expectedValue);
        return TRUE;
    }

    static inline BOOLEAN IsTorn(Context0 ctx)
    {
        UNREFERENCED_PARAMETER(ctx);
        return FALSE;
    }
};

template <>
struct TestContextTraits<64>
{
    static inline UINT64 Make(UINT64 value)
    {
        return value;
    }

    static inline BOOLEAN IsMatch(UINT64 ctx, UINT64 expectedValue)
    {
        return ctx == expectedValue;
    }

    static inline BOOLEAN IsTorn(UINT64 ctx)
    {
        UNREFERENCED_PARAMETER(ctx);
        return FALSE;
    }
};

template <>
struct TestContextTraits<128>
{
    static inline Context128 Make(UINT64 value)
    {
        Context128 ctx;
        ctx.Low = value;
        ctx.High = ~value;
        return ctx;
    }

    static inline BOOLEAN IsMatch(const Context128& ctx, UINT64 expectedValue)
    {
        if (ctx.Low != expectedValue)
        {
            return FALSE;
        }

        if (ctx.High != ~expectedValue)
        {
            return FALSE;
        }

        return TRUE;
    }

    static inline BOOLEAN IsTorn(const Context128& ctx)
    {
        return ctx.High != ~ctx.Low;
    }
};

// ----------------------------------------------------------------------------
// Common Test Context Structs for Workers
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
struct WorkerCtx
{
    COptimisticCache<TContextSize>* pCache;
    SIZE_T                          uCacheCapacity;
    UINT64                          ullKeySpace;
    volatile LONG64                 llTotalOps;
    volatile LONG                   lDataCorruption;
    volatile LONG                   lStartFlag;

    // Tail Latency specific
    UINT64* pGlobalSamples;
    SIZE_T                          uSamplesPerThread;
    LARGE_INTEGER                   liFreq;
};

// ----------------------------------------------------------------------------
// Correctness Tests
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
BOOLEAN RunCorrectnessTests()
{
    PAGED_CODE();

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    LOG_INFO("[OPT_CACHE] [*] Running Correctness Tests...\n");

    COptimisticCache<TContextSize> cache;

    TEST_REQUIRE(cache.Initialize(1024), "Initialize failed", FALSE);

    ContextType ctx1 = TestContextTraits<TContextSize>::Make(100);

    auto failedResult = cache.CheckAndInsert(0, ctx1);

    TEST_REQUIRE(failedResult == COptimisticCache<TContextSize>::InsertResult::Failed, "Add with sentinel key 0 should return Failed", FALSE);

    auto insertResult = cache.CheckAndInsert(10, ctx1);

    TEST_REQUIRE(insertResult == COptimisticCache<TContextSize>::InsertResult::Inserted, "Initial Add should return Inserted", FALSE);

    if constexpr (TContextSize > 0)
    {
        ContextType outCtx;

        TEST_REQUIRE(cache.LookupContext(10, outCtx), "Lookup failed", FALSE);
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 100), "Context data corrupted", FALSE);
    }
    else
    {
        TEST_REQUIRE(cache.Contains(10), "Contains failed", FALSE);
    }

    LOG_INFO("[OPT_CACHE]      [-] Testing Overwrite policies...\n");

    ContextType ctx2 = TestContextTraits<TContextSize>::Make(200);
    ContextType existingCtx;

    insertResult = cache.CheckAndInsert(10,
                                        ctx2,
                                        COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                                        &existingCtx);

    TEST_REQUIRE(insertResult == COptimisticCache<TContextSize>::InsertResult::Updated, "Overwrite Add should return Updated", FALSE);

    if constexpr (TContextSize > 0)
    {
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(existingCtx, 100), "Failed to capture existing context", FALSE);

        ContextType outCtx;

        TEST_REQUIRE(cache.LookupContext(10, outCtx), "Lookup after overwrite failed", FALSE);
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 200), "Overwrite context data corrupted", FALSE);
    }

    LOG_INFO("[OPT_CACHE]      [-] Testing Delete logic and Output Capture...\n");

    ContextType deletedCtx;

    TEST_REQUIRE(cache.Delete(10, &deletedCtx), "Delete existing failed", FALSE);

    if constexpr (TContextSize > 0)
    {
        TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(deletedCtx, 200), "Deleted context mismatch", FALSE);

        ContextType outCtx;

        TEST_REQUIRE(!cache.LookupContext(10, outCtx), "Lookup succeeded after delete", FALSE);
    }
    else
    {
        TEST_REQUIRE(!cache.Contains(10), "Contains succeeded after delete", FALSE);
    }

    TEST_REQUIRE(!cache.Delete(999), "Delete non-existent should fail", FALSE);

    LOG_INFO("[OPT_CACHE]      [-] Testing Eviction output capture...\n");

    UINT64 evictedKey = 0;
    ContextType evictedCtx;
    BOOLEAN evictionOccurred = FALSE;

    for (UINT64 i = 1000; i < 15000; ++i)
    {
        cache.CheckAndInsert(i,
                             TestContextTraits<TContextSize>::Make(i),
                             COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                             nullptr,
                             &evictedKey,
                             &evictedCtx);

        if (evictedKey != 0)
        {
            evictionOccurred = TRUE;

            if constexpr (TContextSize > 0)
            {
                TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(evictedCtx, evictedKey), "Evicted context data corrupted", FALSE);
            }

            break;
        }
    }

    TEST_REQUIRE(evictionOccurred, "Failed to force an eviction for testing", FALSE);

    LOG_INFO("[OPT_CACHE] [+] Correctness Tests Passed.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// Heavy Population & Eviction Tests
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
BOOLEAN RunEvictionTest()
{
    PAGED_CODE();

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    LOG_INFO("[OPT_CACHE] [*] Running Heavily Populated (Eviction) Tests...\n");

    COptimisticCache<TContextSize> cache;

    TEST_REQUIRE(cache.Initialize(256), "Initialize failed", FALSE);

    LOG_INFO("[OPT_CACHE]      [-] Inserting 10,000 items into 256 capacity cache...\n");

    for (UINT64 i = 1; i <= 10000; ++i)
    {
        ContextType ctx = TestContextTraits<TContextSize>::Make(i);
        cache.CheckAndInsert(i, ctx);
    }

    if constexpr (TContextSize > 0)
    {
        ContextType outCtx;

        if (cache.LookupContext(10000, outCtx))
        {
            TEST_REQUIRE(TestContextTraits<TContextSize>::IsMatch(outCtx, 10000), "Eviction corrupted context", FALSE);
        }
    }
    else
    {
        cache.Contains(10000);
    }

    LOG_INFO("[OPT_CACHE] [+] Eviction Tests Passed.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// ABA Correctness Test
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
VOID AbaWorker(_Inout_ TEST_WORKER_CONTEXT* pCtx)
{
    PAGED_CODE();

    SetWorkerThreadAffinity(pCtx->ulThreadId);

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    WorkerCtx<TContextSize>* pAbaCtx = (WorkerCtx<TContextSize>*)pCtx->pUserContext;
    FastRng Rng(pCtx->ulThreadId + 1000);

    KeWaitForSingleObject(pCtx->pStartEvent, Executive, KernelMode, FALSE, NULL);

    if (ExAcquireRundownProtection(&g_TestRundown))
    {
        UINT64 targetKey = 42;
        ContextType ctxA = TestContextTraits<TContextSize>::Make(0xAAAAAAAA);
        ContextType ctxB = TestContextTraits<TContextSize>::Make(0xBBBBBBBB);

        while (InterlockedCompareExchange(&pAbaCtx->lStartFlag, 0, 0) == 0)
        {
            YieldProcessor();
        }

        while (!InterlockedCompareExchange(pCtx->plStopFlag, 0, 0))
        {
            if (pCtx->ulThreadId % 2 == 0) // Writer
            {
                pAbaCtx->pCache->Delete(targetKey);

                if (Rng.Next() % 4 == 0)
                {
                    YieldProcessor();
                }

                pAbaCtx->pCache->CheckAndInsert(targetKey,
                                                ctxB,
                                                COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                                                nullptr);
                if (Rng.Next() % 4 == 0)
                {
                    YieldProcessor();
                }

                pAbaCtx->pCache->Delete(targetKey);

                if (Rng.Next() % 4 == 0)
                {
                    YieldProcessor();
                }

                pAbaCtx->pCache->CheckAndInsert(targetKey,
                                                ctxA,
                                                COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                                                nullptr);
                if (Rng.Next() % 4 == 0)
                {
                    YieldProcessor();
                }
            }
            else // Reader
            {
                if (Rng.Next() % 10 == 0)
                {
                    YieldProcessor();
                }

                if constexpr (TContextSize > 0)
                {
                    ContextType outCtx;

                    if (pAbaCtx->pCache->LookupContext(targetKey, outCtx))
                    {
                        BOOLEAN isA = TestContextTraits<TContextSize>::IsMatch(outCtx, 0xAAAAAAAA);
                        BOOLEAN isB = TestContextTraits<TContextSize>::IsMatch(outCtx, 0xBBBBBBBB);

                        if (!isA && !isB)
                        {
                            InterlockedExchange(&pAbaCtx->lDataCorruption, 1);
                        }
                    }
                }
                else
                {
                    pAbaCtx->pCache->Contains(targetKey);
                }
            }
        }

        ExReleaseRundownProtection(&g_TestRundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

template <SIZE_T TContextSize>
BOOLEAN RunAbaCorrectnessTest(int nSecondsToRun)
{
    PAGED_CODE();

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    LOG_INFO("[OPT_CACHE] [*] Running ABA (A-B-A) Race Condition Test (%d seconds)...\n", nSecondsToRun);

    COptimisticCache<TContextSize> cache;

    TEST_REQUIRE(cache.Initialize(2048), "Initialize failed", FALSE);

    UINT64 targetKey = 42;
    ContextType ctxA = TestContextTraits<TContextSize>::Make(0xAAAAAAAA);

    cache.CheckAndInsert(targetKey, ctxA);

    ULONG ulThreadCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulThreadCount == 0)
    {
        ulThreadCount = 4;
    }

    ulThreadCount *= 4; // Heavily oversubscribe
    if (ulThreadCount > MAX_TEST_THREADS)
    {
        ulThreadCount = MAX_TEST_THREADS;
    }

    WorkerCtx<TContextSize> Ctx = { 0 };
    Ctx.pCache = &cache;

    TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
    if (!pMgr)
    {
        LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
        return FALSE;
    }

    LOG_INFO("[OPT_CACHE]      [-] Hammering single key with rapid A-B-A transitions...\n");
    LOG_INFO("[OPT_CACHE]      [-] Using %u heavily oversubscribed threads...\n", ulThreadCount);

    StartThreads(pMgr, ulThreadCount, AbaWorker<TContextSize>, &Ctx);

    InterlockedExchange(&Ctx.lStartFlag, 1);
    StopAndWaitThreads(pMgr, nSecondsToRun);

    delete pMgr;
    cache.Cleanup();

    TEST_REQUIRE(Ctx.lDataCorruption == 0, "ABA DATA CORRUPTION DETECTED!", FALSE);

    LOG_INFO("[OPT_CACHE] [+] ABA Correctness Test Passed.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// Comprehensive Multi-Threaded Tearing & Race Condition Test
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
VOID MixedOpsWorker(_Inout_ TEST_WORKER_CONTEXT* pCtx)
{
    PAGED_CODE();

    SetWorkerThreadAffinity(pCtx->ulThreadId);

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    WorkerCtx<TContextSize>* pWorkerCtx = (WorkerCtx<TContextSize>*)pCtx->pUserContext;
    FastRng Rng(pCtx->ulThreadId + 3000);

    KeWaitForSingleObject(pCtx->pStartEvent, Executive, KernelMode, FALSE, NULL);

    if (ExAcquireRundownProtection(&g_TestRundown))
    {
        while (InterlockedCompareExchange(&pWorkerCtx->lStartFlag, 0, 0) == 0)
        {
            YieldProcessor();
        }

        UINT64 localOps = 0;

        while (!InterlockedCompareExchange(pCtx->plStopFlag, 0, 0))
        {
            UINT32 op  = Rng.Next() % 100;
            UINT64 key = (Rng.Next() % pWorkerCtx->ullKeySpace) + 1;

            if (op <= 60)
            {
                if constexpr (TContextSize > 0)
                {
                    ContextType outCtx;

                    if (pWorkerCtx->pCache->LookupContext(key, outCtx))
                    {
                        if (TestContextTraits<TContextSize>::IsTorn(outCtx))
                        {
                            InterlockedExchange(&pWorkerCtx->lDataCorruption, 1);
                        }
                    }
                }
                else
                {
                    pWorkerCtx->pCache->Contains(key);
                }
            }
            else if (op <= 85)
            {
                ContextType ctx = TestContextTraits<TContextSize>::Make(key + pCtx->ulThreadId);

                pWorkerCtx->pCache->CheckAndInsert(key,
                                                   ctx,
                                                   COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                                                   nullptr);
            }
            else
            {
                pWorkerCtx->pCache->Delete(key);
            }

            localOps++;
        }

        InterlockedExchangeAdd64(&pWorkerCtx->llTotalOps, localOps);
        ExReleaseRundownProtection(&g_TestRundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

template <SIZE_T TContextSize>
BOOLEAN RunComprehensiveMultiThreadedTest(int nSecondsToRun)
{
    PAGED_CODE();

    LOG_INFO("[OPT_CACHE] [*] Running Comprehensive Multi-Threaded Correctness Test (%d seconds)...\n", nSecondsToRun);

    COptimisticCache<TContextSize> cache;

    TEST_REQUIRE(cache.Initialize(2048), "Initialize failed", FALSE);

    ULONG ulThreadCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulThreadCount == 0)
    {
        ulThreadCount = 4;
    }

    if (ulThreadCount > MAX_TEST_THREADS)
    {
        ulThreadCount = MAX_TEST_THREADS;
    }

    WorkerCtx<TContextSize> Ctx = { 0 };
    Ctx.pCache      = &cache;
    Ctx.ullKeySpace = 100;

    TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
    if (!pMgr)
    {
        LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
        return FALSE;
    }

    LOG_INFO("[OPT_CACHE]      [-] Hammering cache with %u threads (Mixed Ops)...\n", ulThreadCount);

    StartThreads(pMgr, ulThreadCount, MixedOpsWorker<TContextSize>, &Ctx);

    InterlockedExchange(&Ctx.lStartFlag, 1);
    StopAndWaitThreads(pMgr, nSecondsToRun);

    delete pMgr;
    cache.Cleanup();

    TEST_REQUIRE(Ctx.lDataCorruption == 0, "DATA TEARING DETECTED!", FALSE);

    LOG_INFO("[OPT_CACHE] [+] Multi-Threaded Correctness Test Passed.\n\n");

    return TRUE;
}

template <SIZE_T TContextSize>
BOOLEAN RunOversubscribedMixedWorkloadTest(int nSecondsToRun)
{
    PAGED_CODE();

    LOG_INFO("[OPT_CACHE] [*] Running Oversubscribed Mixed Workload Test (3x Cores, %d seconds)...\n", nSecondsToRun);

    COptimisticCache<TContextSize> cache;
    SIZE_T uCapacity = 4096;

    TEST_REQUIRE(cache.Initialize(uCapacity), "Initialize failed", FALSE);

    LOG_INFO("[OPT_CACHE]      [-] Cache Capacity : %llu items\n", uCapacity);
    LOG_INFO("[OPT_CACHE]      [-] Memory Used    : %llu KB\n", cache.GetMemoryUsage() / 1024);

    ULONG ulThreadCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulThreadCount == 0)
    {
        ulThreadCount = 4;
    }

    ulThreadCount *= 3;
    if (ulThreadCount > MAX_TEST_THREADS)
    {
        ulThreadCount = MAX_TEST_THREADS;
    }

    WorkerCtx<TContextSize> Ctx = { 0 };
    Ctx.pCache      = &cache;
    Ctx.ullKeySpace = 500;

    TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
    if (!pMgr)
    {
        LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
        return FALSE;
    }

    LOG_INFO("[OPT_CACHE]      [-] Hammering cache with %u threads (Heavy Contention)...\n", ulThreadCount);

    StartThreads(pMgr, ulThreadCount, MixedOpsWorker<TContextSize>, &Ctx);

    InterlockedExchange(&Ctx.lStartFlag, 1);
    StopAndWaitThreads(pMgr, nSecondsToRun);

    delete pMgr;
    cache.Cleanup();

    TEST_REQUIRE(Ctx.lDataCorruption == 0, "DATA TEARING DETECTED DURING OVERSUBSCRIPTION!", FALSE);

    UINT64 throughput = Ctx.llTotalOps / nSecondsToRun;

    LOG_INFO("[OPT_CACHE]      [-] Total Ops  : %llu\n", Ctx.llTotalOps);
    LOG_INFO("[OPT_CACHE]      [-] Throughput : %llu Ops/sec\n", throughput);
    LOG_INFO("[OPT_CACHE] [+] Oversubscribed Mixed Workload Test Passed.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// Constant Eviction Performance Test (Large Capacity)
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
VOID ConstantEvictionWorker(_Inout_ TEST_WORKER_CONTEXT* pCtx)
{
    PAGED_CODE();

    SetWorkerThreadAffinity(pCtx->ulThreadId);

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    WorkerCtx<TContextSize>* pWorkerCtx = (WorkerCtx<TContextSize>*)pCtx->pUserContext;
    FastRng Rng(pCtx->ulThreadId + 7000);

    KeWaitForSingleObject(pCtx->pStartEvent, Executive, KernelMode, FALSE, NULL);

    if (ExAcquireRundownProtection(&g_TestRundown))
    {
        while (InterlockedCompareExchange(&pWorkerCtx->lStartFlag, 0, 0) == 0)
        {
            YieldProcessor();
        }

        UINT64 localOps = 0;

        while (!InterlockedCompareExchange(pCtx->plStopFlag, 0, 0))
        {
            UINT32 op = Rng.Next() % 100;
            UINT64 key = (Rng.Next() % pWorkerCtx->ullKeySpace) + 1;

            if (op <= 50)
            {
                if constexpr (TContextSize > 0)
                {
                    ContextType outCtx;

                    if (pWorkerCtx->pCache->LookupContext(key, outCtx))
                    {
                        if (TestContextTraits<TContextSize>::IsTorn(outCtx))
                        {
                            InterlockedExchange(&pWorkerCtx->lDataCorruption, 1);
                        }
                    }
                }
                else
                {
                    pWorkerCtx->pCache->Contains(key);
                }
            }
            else if (op <= 85)
            {
                ContextType ctx = TestContextTraits<TContextSize>::Make(key + pCtx->ulThreadId);

                pWorkerCtx->pCache->CheckAndInsert(key,
                                                   ctx,
                                                   COptimisticCache<TContextSize>::InsertPolicy::Overwrite,
                                                   nullptr);
            }
            else
            {
                pWorkerCtx->pCache->Delete(key);
            }

            localOps++;
        }

        InterlockedExchangeAdd64(&pWorkerCtx->llTotalOps, localOps);
        ExReleaseRundownProtection(&g_TestRundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

template <SIZE_T TContextSize>
BOOLEAN RunConstantEvictionPerfTest(int nSecondsToRun)
{
    PAGED_CODE();

    LOG_INFO("[OPT_CACHE] [*] Running Constant Eviction Performance Test (1x Cores, %d seconds)...\n", nSecondsToRun);

    COptimisticCache<TContextSize> cache;
    SIZE_T uCapacity = 1000000;

    TEST_REQUIRE(cache.Initialize(uCapacity), "Initialize failed", FALSE);

    LOG_INFO("[OPT_CACHE]      [-] Cache Capacity : %llu items\n", uCapacity);
    LOG_INFO("[OPT_CACHE]      [-] Memory Used    : %llu MB\n", cache.GetMemoryUsage() / (1024 * 1024));

    ULONG ulThreadCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulThreadCount == 0)
    {
        ulThreadCount = 4;
    }

    if (ulThreadCount > MAX_TEST_THREADS)
    {
        ulThreadCount = MAX_TEST_THREADS;
    }

    WorkerCtx<TContextSize> Ctx = { 0 };
    Ctx.pCache      = &cache;
    Ctx.ullKeySpace = 10000000; // 10M key space for constant eviction storm

    TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
    if (!pMgr)
    {
        LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
        return FALSE;
    }

    LOG_INFO("[OPT_CACHE]      [-] Hammering cache with %u threads (Constant Eviction)...\n", ulThreadCount);

    StartThreads(pMgr, ulThreadCount, ConstantEvictionWorker<TContextSize>, &Ctx);

    InterlockedExchange(&Ctx.lStartFlag, 1);
    StopAndWaitThreads(pMgr, nSecondsToRun);

    delete pMgr;
    cache.Cleanup();

    TEST_REQUIRE(Ctx.lDataCorruption == 0, "DATA TEARING DETECTED DURING EVICTION STORM!", FALSE);

    UINT64 throughput = Ctx.llTotalOps / nSecondsToRun;

    LOG_INFO("[OPT_CACHE]      [-] Total Ops  : %llu\n", Ctx.llTotalOps);
    LOG_INFO("[OPT_CACHE]      [-] Throughput : %llu Ops/sec\n", throughput);
    LOG_INFO("[OPT_CACHE] [+] Constant Eviction Performance Test Passed.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// Performance & Scalability Framework
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
VOID ScalingWorker(_Inout_ TEST_WORKER_CONTEXT* pCtx)
{
    PAGED_CODE();

    SetWorkerThreadAffinity(pCtx->ulThreadId);

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    WorkerCtx<TContextSize>* pWorkerCtx = (WorkerCtx<TContextSize>*)pCtx->pUserContext;
    FastRng Rng(pCtx->ulThreadId + 6000);

    KeWaitForSingleObject(pCtx->pStartEvent, Executive, KernelMode, FALSE, NULL);

    if (ExAcquireRundownProtection(&g_TestRundown))
    {
        ContextType ctx = TestContextTraits<TContextSize>::Make(0xDEADBEEF);
        ContextType outCtx;
        UINT64 localOps = 0;

        while (InterlockedCompareExchange(&pWorkerCtx->lStartFlag, 0, 0) == 0)
        {
            YieldProcessor();
        }

        while (!InterlockedCompareExchange(pCtx->plStopFlag, 0, 0))
        {
            UINT32 op       = Rng.Next() % 100;
            UINT64 targetId = (Rng.Next() % 200000) + 1;

            // Notice we use ullKeySpace to act as "readPercentage" parameter for scaling tests
            if (op <= pWorkerCtx->ullKeySpace)
            {
                if constexpr (TContextSize > 0)
                {
                    pWorkerCtx->pCache->LookupContext(targetId, outCtx);
                }
                else
                {
                    pWorkerCtx->pCache->Contains(targetId);
                }
            }
            else
            {
                if (op % 2 == 0)
                {
                    pWorkerCtx->pCache->CheckAndInsert(targetId, ctx);
                }
                else
                {
                    pWorkerCtx->pCache->Delete(targetId);
                }
            }

            localOps++;
        }

        InterlockedExchangeAdd64(&pWorkerCtx->llTotalOps, localOps);
        ExReleaseRundownProtection(&g_TestRundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

template <SIZE_T TContextSize>
BOOLEAN RunComparativeScalingSweep(const char* szTestName,
                                   int         nReadPercentage,
                                   int         nSecondsPerStep)
{
    PAGED_CODE();

    LOG_INFO("[OPT_CACHE] [*] Comparative Scaling Sweep: %s\n", szTestName);
    LOG_INFO("[OPT_CACHE]      %-10s| %-18s| %-12s\n", "Threads", "Throughput Ops/s", "Scaling Factor");
    LOG_INFO("[OPT_CACHE]      --------------------------------------------------\n");

    ULONG ulMaxThreads = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulMaxThreads == 0)
    {
        ulMaxThreads = 4;
    }

    if (ulMaxThreads > MAX_TEST_THREADS)
    {
        ulMaxThreads = MAX_TEST_THREADS;
    }

    ULONG   ulThreadCount  = 1;
    BOOLEAN bDidMax        = FALSE;
    UINT64  ullBaselineOps = 0;

    LARGE_INTEGER liFreq, liStart, liEnd;
    KeQueryPerformanceCounter(&liFreq);

    while (!bDidMax)
    {
        if (ulThreadCount >= ulMaxThreads)
        {
            ulThreadCount = ulMaxThreads;
            bDidMax = TRUE;
        }

        COptimisticCache<TContextSize> cache;

        TEST_REQUIRE(cache.Initialize(131072), "Initialize failed", FALSE);

        for (UINT64 i = 1; i <= 65000; ++i)
        {
            cache.CheckAndInsert(i, TestContextTraits<TContextSize>::Make(i));
        }

        WorkerCtx<TContextSize> Ctx = { 0 };
        Ctx.pCache      = &cache;
        Ctx.ullKeySpace = nReadPercentage; // passing read percentage using key space variable

        TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
        if (!pMgr)
        {
            LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
            return FALSE;
        }

        StartThreads(pMgr, ulThreadCount, ScalingWorker<TContextSize>, &Ctx);

        SleepMs(500); // Wait for threads to hit barrier spin

        InterlockedExchange(&Ctx.lStartFlag, 1);
        liStart = KeQueryPerformanceCounter(NULL);

        StopAndWaitThreads(pMgr, nSecondsPerStep);
        liEnd = KeQueryPerformanceCounter(NULL);

        delete pMgr;
        cache.Cleanup();

        UINT64 ullTicks = liEnd.QuadPart - liStart.QuadPart;
        if (ullTicks == 0)
        {
            ullTicks = 1;
        }

        UINT64 ullOps = Ctx.llTotalOps;
        if (ullOps == 0)
        {
            ullOps = 1;
        }

        UINT64 ullThroughput = (ullOps * liFreq.QuadPart) / ullTicks;
        if (ulThreadCount == 1)
        {
            ullBaselineOps = ullThroughput;
        }

        UINT64 ullScaleWhole = 0;
        UINT64 ullScaleFrac = 0;

        if (ullBaselineOps > 0)
        {
            UINT64 ullScaleFactor = (ullThroughput * 100) / ullBaselineOps;
            ullScaleWhole = ullScaleFactor / 100;
            ullScaleFrac  = ullScaleFactor % 100;
        }

        LOG_INFO("[OPT_CACHE]      %-10u| %-18llu| %llu.%02llux\n",
                 ulThreadCount,
                 ullThroughput,
                 ullScaleWhole,
                 ullScaleFrac);

        if (!bDidMax)
        {
            ulThreadCount *= 2;
        }
    }

    LOG_INFO("[OPT_CACHE] [+] Scaling Sweep Complete.\n\n");

    return TRUE;
}

// ----------------------------------------------------------------------------
// Tail Latency Metric Capture
// ----------------------------------------------------------------------------
template <SIZE_T TContextSize>
VOID TailLatencyWorker(_Inout_ TEST_WORKER_CONTEXT* pCtx)
{
    PAGED_CODE();

    SetWorkerThreadAffinity(pCtx->ulThreadId);

    using ContextType = typename CacheContextTraits<TContextSize>::Type;

    WorkerCtx<TContextSize>* pLatencyCtx = (WorkerCtx<TContextSize>*)pCtx->pUserContext;
    FastRng Rng(pCtx->ulThreadId + 8000);

    KeWaitForSingleObject(pCtx->pStartEvent, Executive, KernelMode, FALSE, NULL);

    if (ExAcquireRundownProtection(&g_TestRundown))
    {
        UINT64  ullMaxKey = pLatencyCtx->uCacheCapacity + (pLatencyCtx->uCacheCapacity / 2);
        UINT32  u32OpCounter = 0;
        SIZE_T  uSampleIdx = 0;
        UINT64* pMySamples = pLatencyCtx->pGlobalSamples + (pCtx->ulThreadId * pLatencyCtx->uSamplesPerThread);

        while (InterlockedCompareExchange(&pLatencyCtx->lStartFlag, 0, 0) == 0)
        {
            UINT64 key = (Rng.Next() % ullMaxKey) + 1;

            if constexpr (TContextSize > 0)
            {
                ContextType out;
                pLatencyCtx->pCache->LookupContext(key, out);
            }
            else
            {
                pLatencyCtx->pCache->Contains(key);
            }
        }

        while (uSampleIdx < pLatencyCtx->uSamplesPerThread)
        {
            if (InterlockedCompareExchange(&g_lAbortTests, 0, 0))
            {
                break;
            }

            UINT64 key = (Rng.Next() % ullMaxKey) + 1;
            UINT32 opType = Rng.Next() % 100;

            u32OpCounter++;
            BOOLEAN bShouldSample = (u32OpCounter % 100 == 0);

            LARGE_INTEGER liT0 = { 0 };
            LARGE_INTEGER liT1 = { 0 };

            if (bShouldSample)
            {
                liT0 = KeQueryPerformanceCounter(NULL);
            }

            if (opType < 90)
            {
                if constexpr (TContextSize > 0)
                {
                    ContextType out;
                    pLatencyCtx->pCache->LookupContext(key, out);
                }
                else
                {
                    pLatencyCtx->pCache->Contains(key);
                }
            }
            else
            {
                pLatencyCtx->pCache->CheckAndInsert(key, TestContextTraits<TContextSize>::Make(key));
            }

            if (bShouldSample)
            {
                liT1 = KeQueryPerformanceCounter(NULL);
                UINT64 ullElapsedNs = ((liT1.QuadPart - liT0.QuadPart) * 1000000000ULL) / pLatencyCtx->liFreq.QuadPart;
                pMySamples[uSampleIdx++] = ullElapsedNs;
            }
        }

        ExReleaseRundownProtection(&g_TestRundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

template <SIZE_T TContextSize>
BOOLEAN RunTailLatencyTest(const char* szTestName,
                           SIZE_T      uCacheCapacity)
{
    PAGED_CODE();

    LOG_INFO("[OPT_CACHE] [*] Running Tail Latency Test: %s...\n", szTestName);

    COptimisticCache<TContextSize> cache;

    TEST_REQUIRE(cache.Initialize(uCacheCapacity), "Init failed", FALSE);

    for (SIZE_T i = 1; i <= uCacheCapacity / 2; ++i)
    {
        cache.CheckAndInsert(i, TestContextTraits<TContextSize>::Make(i));
    }

    ULONG ulThreadCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (ulThreadCount == 0)
    {
        ulThreadCount = 4;
    }

    if (ulThreadCount > MAX_TEST_THREADS)
    {
        ulThreadCount = MAX_TEST_THREADS;
    }

    const SIZE_T uSamplesPerThread = 50000;
    SIZE_T uTotalSamples = uSamplesPerThread * ulThreadCount;

    UINT64* pGlobalSamples = (UINT64*)ExAllocatePool2(POOL_FLAG_PAGED, uTotalSamples * sizeof(UINT64), DRIVER_TAG);
    if (!pGlobalSamples)
    {
        LOG_ERR("[OPT_CACHE] [!] Failed to allocate latency samples buffer\n");
        return FALSE;
    }

    WorkerCtx<TContextSize> Ctx = { 0 };
    Ctx.pCache            = &cache;
    Ctx.pGlobalSamples    = pGlobalSamples;
    Ctx.uSamplesPerThread = uSamplesPerThread;
    Ctx.uCacheCapacity    = uCacheCapacity;
    KeQueryPerformanceCounter(&Ctx.liFreq);

    TEST_THREAD_MANAGER* pMgr = new (POOL_FLAG_NON_PAGED, DRIVER_TAG) TEST_THREAD_MANAGER();
    if (!pMgr)
    {
        ExFreePoolWithTag(pGlobalSamples, DRIVER_TAG);
        LOG_ERR("[OPT_CACHE]      [!] Failed to allocate TEST_THREAD_MANAGER.\n");
        return FALSE;
    }

    StartThreads(pMgr, ulThreadCount, TailLatencyWorker<TContextSize>, &Ctx);

    LOG_INFO("[OPT_CACHE]      - Warming up for 1 second...\n");
    SleepMs(1000);

    LOG_INFO("[OPT_CACHE]      - Warm-up complete. Collecting tail latency samples...\n");
    InterlockedExchange(&Ctx.lStartFlag, 1);

    StopAndWaitThreads(pMgr, 0);

    delete pMgr;
    cache.Cleanup();

    QuickSort(pGlobalSamples, uTotalSamples, [](const UINT64& ullA,
        const UINT64& ullB) -> int
        {
            if (ullA < ullB)
            {
                return -1;
            }

            if (ullA > ullB)
            {
                return 1;
            }

            return 0;
        });

    auto getPercentile = [&](UINT32 u32Permille) -> UINT64
        {
            SIZE_T uIndex = (uTotalSamples * u32Permille) / 10000;

            if (uIndex >= uTotalSamples)
            {
                uIndex = uTotalSamples - 1;
            }

            return pGlobalSamples[uIndex];
        };

    LOG_INFO("[OPT_CACHE]      - Total Samples Collected : %llu\n", uTotalSamples);
    LOG_INFO("[OPT_CACHE]      - P50 (Median) Latency    : %llu ns\n", getPercentile(5000));
    LOG_INFO("[OPT_CACHE]      - P90 Latency             : %llu ns\n", getPercentile(9000));
    LOG_INFO("[OPT_CACHE]      - P99 Latency             : %llu ns\n", getPercentile(9900));
    LOG_INFO("[OPT_CACHE]      - P99.9 Latency           : %llu ns\n", getPercentile(9990));
    LOG_INFO("[OPT_CACHE]      - P99.99 Latency          : %llu ns\n", getPercentile(9999));
    LOG_INFO("[OPT_CACHE] [+] Tail Latency Test Complete.\n\n");

    ExFreePoolWithTag(pGlobalSamples, DRIVER_TAG);

    return TRUE;
}

// ----------------------------------------------------------------------------
// Master Test Orchestrator
// ----------------------------------------------------------------------------
VOID RunBenchmarks(_In_opt_ PVOID pContext)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pContext);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    LOG_INFO("[OPT_CACHE] \n=========================================================\n");
    LOG_INFO("[OPT_CACHE]    OptimisticCache KM Test Suite Start\n");
    LOG_INFO("[OPT_CACHE] =========================================================\n\n");

    auto RunTestsForContext = [](auto contextSizeArg) -> BOOLEAN
        {
            constexpr SIZE_T SIZE = contextSizeArg();

            LOG_INFO("[OPT_CACHE] =========================================================\n");
            LOG_INFO("[OPT_CACHE]          OPTIMISTIC CACHE <%llu-BIT CONTEXT>             \n", SIZE);
            LOG_INFO("[OPT_CACHE] =========================================================\n\n");

#define ABORT_CHECK() \
        if (InterlockedCompareExchange(&g_lAbortTests, 0, 0)) \
        { \
            LOG_INFO("[OPT_CACHE] Abort requested. Stopping test suite execution.\n"); \
            return FALSE; \
        }

            ABORT_CHECK();
            if (!RunCorrectnessTests<SIZE>())
            {
                return FALSE;
            }

            ABORT_CHECK();
            if (!RunEvictionTest<SIZE>())
            {
                return FALSE;
            }

            ABORT_CHECK();
            if (!RunAbaCorrectnessTest<SIZE>(15))
            {
                return FALSE;
            }

            ABORT_CHECK();
            if (!RunComprehensiveMultiThreadedTest<SIZE>(15))
            {
                return FALSE;
            }

            ABORT_CHECK();
            if (!RunOversubscribedMixedWorkloadTest<SIZE>(10))
            {
                return FALSE;
            }

            ABORT_CHECK();
            if (!RunConstantEvictionPerfTest<SIZE>(10))
            {
                return FALSE;
            }

            ABORT_CHECK();
            RunComparativeScalingSweep<SIZE>("10/90 Write-Heavy Workload", 10, 2);

            ABORT_CHECK();
            RunComparativeScalingSweep<SIZE>("50/50 Mixed Contention Workload", 50, 2);

            ABORT_CHECK();
            RunComparativeScalingSweep<SIZE>("80/20 Read-Heavy Workload", 80, 2);

            ABORT_CHECK();
            RunComparativeScalingSweep<SIZE>("95/5 Highly Skewed Workload", 95, 2);

            ABORT_CHECK();
            if (!RunTailLatencyTest<SIZE>("Lock-Free Cache", 131072))
            {
                return FALSE;
            }

            return TRUE;

#undef ABORT_CHECK
        };

    BOOLEAN bTestsPassed = TRUE;

    // Use lambda indirection to explicitly pass the constexpr SIZE_T to templates
    if (bTestsPassed)
    {
        bTestsPassed = RunTestsForContext([]() constexpr { return 0; });
    }

    if (bTestsPassed)
    {
        bTestsPassed = RunTestsForContext([]() constexpr { return 64; });
    }

    if (bTestsPassed)
    {
        bTestsPassed = RunTestsForContext([]() constexpr { return 128; });
    }

    if (!bTestsPassed && InterlockedCompareExchange(&g_lAbortTests, 0, 0) == 0)
    {
        LOG_ERR("[OPT_CACHE] [-] Test suite aborted due to an error.\n");
    }
    else if (!bTestsPassed && InterlockedCompareExchange(&g_lAbortTests, 0, 0) != 0)
    {
        LOG_INFO("[OPT_CACHE] [-] Test suite aborted by driver unload.\n");
    }
    else
    {
        LOG_INFO("[OPT_CACHE] \n--- All Tests Finished Successfully ---\n");
    }

    PsTerminateSystemThread(bTestsPassed ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
}

VOID DriverUnload(_In_ PDRIVER_OBJECT pDriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pDriverObject);

    LOG_INFO("\n[OPT_CACHE] ***Unload requested. Signaling abort...\n");

    InterlockedExchange(&g_lAbortTests, 1);

    ExWaitForRundownProtectionRelease(&g_TestRundown);

    if (g_pMasterBenchmarkThread != NULL)
    {
        LOG_INFO("[OPT_CACHE] Waiting for master benchmark thread to terminate...\n");

        KeWaitForSingleObject(g_pMasterBenchmarkThread,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);

        ObDereferenceObject(g_pMasterBenchmarkThread);
        g_pMasterBenchmarkThread = NULL;
    }

    LOG_INFO("[OPT_CACHE] ***Driver Unloaded.\n");
}

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  pDriverObject,
                                _In_ PUNICODE_STRING pRegistryPath);

#pragma alloc_text(INIT, DriverEntry)
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  pDriverObject,
                                _In_ PUNICODE_STRING pRegistryPath)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pRegistryPath);

    LOG_INFO("[OPT_CACHE] ***DriverEntry Called.\n");

    ExInitializeRundownProtection(&g_TestRundown);

    pDriverObject->DriverUnload = DriverUnload;

    HANDLE hThread;
    NTSTATUS ntStatus = PsCreateSystemThread(&hThread,
                                             THREAD_ALL_ACCESS,
                                             NULL,
                                             NULL,
                                             NULL,
                                             (PKSTART_ROUTINE)RunBenchmarks,
                                             NULL);

    if (NT_SUCCESS(ntStatus))
    {
        NTSTATUS refStatus = ObReferenceObjectByHandle(hThread,
                                                       THREAD_ALL_ACCESS,
                                                       NULL,
                                                       KernelMode,
                                                       (PVOID*)&g_pMasterBenchmarkThread,
                                                       NULL);

        if (!NT_SUCCESS(refStatus))
        {
            LOG_ERR("[OPT_CACHE] [!] Failed to reference master thread handle. Aborting.\n");
            
            // Fast abort the thread we just spawned
            InterlockedExchange(&g_lAbortTests, 1);
            
            // Wait for it to safely terminate to prevent the BSOD condition
            ZwWaitForSingleObject(hThread, FALSE, NULL);
            ZwClose(hThread);
            
            return refStatus;
        }

        ZwClose(hThread);
    }
    else
    {
        LOG_ERR("[OPT_CACHE] [!] Failed to create master system thread.\n");
        return ntStatus;
    }

    return STATUS_SUCCESS;
}