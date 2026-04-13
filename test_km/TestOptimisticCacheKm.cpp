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

#define KERNEL_TEST 1

#include <iostream>

// ----------------------------------------------------------------------------
// User Mode Headers
// ----------------------------------------------------------------------------
#include <windows.h>
#include <winternl.h>
#include <intrin.h>
#include <stdlib.h>

// ----------------------------------------------------------------------------
// Type Definitions & SAL Annotations
// ----------------------------------------------------------------------------
typedef LONG NTSTATUS;

#ifndef _In_
#define _In_
#endif

#ifndef _Out_
#define _Out_
#endif

#ifndef _In_opt_
#define _In_opt_
#endif

#ifndef _Out_opt_
#define _Out_opt_
#endif

// ----------------------------------------------------------------------------
// NTSTATUS Codes
// ----------------------------------------------------------------------------
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#endif

#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#endif

#ifndef PAGED_CODE
#define PAGED_CODE()
#endif

#ifndef POOL_NODE_REQUIREMENT
typedef ULONG POOL_NODE_REQUIREMENT;
#endif

#ifndef EX_POOL_PRIORITY
typedef enum _EX_POOL_PRIORITY
{
    LowPoolPriority,
    LowPoolPrioritySpecialPoolOverrun,
    LowPoolPrioritySpecialPoolUnderrun,
    NormalPoolPriority,
    NormalPoolPrioritySpecialPoolOverrun,
    NormalPoolPrioritySpecialPoolUnderrun,
    HighPoolPriority,
    HighPoolPrioritySpecialPoolOverrun,
    HighPoolPrioritySpecialPoolUnderrun
} EX_POOL_PRIORITY;
#endif

#ifndef POOL_EXTENDED_PARAMETER_TYPE
typedef enum _POOL_EXTENDED_PARAMETER_TYPE
{
    PoolExtendedParameterInvalidType = 0,
    PoolExtendedParameterPriority = 1,
    PoolExtendedParameterNumaNode = 2
} POOL_EXTENDED_PARAMETER_TYPE;
#endif

#ifndef POOL_EXTENDED_PARAMETER
typedef struct _POOL_EXTENDED_PARAMS_SECURE_POOL POOL_EXTENDED_PARAMS_SECURE_POOL;
typedef struct _POOL_EXTENDED_PARAMETER
{
    struct
    {
        ULONG64 Type : 8;
        ULONG64 Optional : 1;
        ULONG64 Reserved : 55;
    };
    union
    {
        ULONG64                           Reserved2;
        PVOID                             Reserved3;
        EX_POOL_PRIORITY                  Priority;
        POOL_EXTENDED_PARAMS_SECURE_POOL* SecurePoolParams;
        POOL_NODE_REQUIREMENT             PreferredNode;
    };
} POOL_EXTENDED_PARAMETER, * PPOOL_EXTENDED_PARAMETER;
#endif

#ifndef KernelMode
typedef char KPROCESSOR_MODE;
#define KernelMode 0
#endif

// ----------------------------------------------------------------------------
// Pool Allocation Mocking
// ----------------------------------------------------------------------------
#define POOL_FLAG_NON_PAGED 0x0000000000000040UI64
#define POOL_FLAG_PAGED     0x0000000000000100UI64

inline PVOID ExAllocatePool2(_In_ ULONG  Flags,
                             _In_ SIZE_T NumberOfBytes,
                             _In_ ULONG  Tag)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Tag);

    return ::operator new[](NumberOfBytes, std::align_val_t{ 64 }, std::nothrow);
}

inline PVOID ExAllocatePool3(_In_ ULONG                                                       Flags,
                             _In_ SIZE_T                                                      NumberOfBytes,
                             _In_ ULONG                                                       Tag,
                             _In_reads_opt_(ExtendedParametersCount) PPOOL_EXTENDED_PARAMETER ExtendedParameters,
                             _In_ ULONG                                                       ExtendedParametersCount)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(ExtendedParameters);
    UNREFERENCED_PARAMETER(ExtendedParametersCount);

    return ::operator new[](NumberOfBytes, std::align_val_t{ 64 }, std::nothrow);
}

inline void ExFreePoolWithTag(_In_opt_ PVOID P,
                              _In_     ULONG Tag
)
{
    UNREFERENCED_PARAMETER(Tag);
    if (P)
    {
        ::operator delete[](P, std::align_val_t{ 64 });
    }
}

// ----------------------------------------------------------------------------
// Processor & System Information
// ----------------------------------------------------------------------------
typedef UCHAR KIRQL, * PKIRQL;
#define PASSIVE_LEVEL  0
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2

// Simple global to allow tests to simulate different IRQL states
static thread_local KIRQL g_MockIrql = PASSIVE_LEVEL;

inline ULONG KeQueryActiveProcessorCountEx(_In_ USHORT GroupNumber)
{
    // Maps cleanly to the user-mode equivalent introduced in Windows 7.
    return GetActiveProcessorCount(GroupNumber);
}

// Emulates a hardware stall without yielding context to the OS scheduler
inline VOID KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER freq, start, current;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    LONGLONG stallTicks = (freq.QuadPart * MicroSeconds) / 1000000;

    do
    {
        YieldProcessor();
        QueryPerformanceCounter(&current);
    } while (current.QuadPart - start.QuadPart < stallTicks);
}

inline KIRQL KeGetCurrentIrql()
{
    return g_MockIrql;
}

inline void KeRaiseIrql(_In_  KIRQL  NewIrql,
                        _Out_ PKIRQL OldIrql)
{    
    if (OldIrql)
    {
        *OldIrql = g_MockIrql;
    }

    g_MockIrql = NewIrql;
}

inline void KeLowerIrql(_In_ KIRQL NewIrql)
{
    g_MockIrql = NewIrql;
}

// In KM, this is an export from ntoskrnl. In UM, we map it to SwitchToThread.
inline NTSTATUS ZwYieldExecution()
{
    // SwitchToThread returns a BOOL; we convert to NTSTATUS for signature parity.
    return SwitchToThread() ? 0x00000000L /* STATUS_SUCCESS */ : 0x00000001L;
}

// Mocking the scheduler hint
inline BOOLEAN KeShouldYieldProcessor()
{
    // Default to false for performance benchmarks; 
    // toggle in specific test cases to exercise the "Cooperative" branch.
    static thread_local bool mockShouldYield = false;
    return (BOOLEAN)mockShouldYield;
}

inline NTSTATUS NTAPI UmStub_ZwYieldExecution()
{
    // SwitchToThread returns non-zero if a switch occurred, 0 if not.
    // Map this to STATUS_SUCCESS (0) or STATUS_NO_YIELD_PERFORMED (0x40000024)
    return SwitchToThread() ? 0x00000000L : 0x40000024L;
}

inline NTSTATUS KeDelayExecutionThread(_In_ KPROCESSOR_MODE WaitMode,
                                       _In_ BOOLEAN         Alertable,
                                       _In_ PLARGE_INTEGER  Interval)
{
    UNREFERENCED_PARAMETER(WaitMode);
    UNREFERENCED_PARAMETER(Alertable);

    // The kernel Interval is in 100-nanosecond units. 
    // Negative values represent relative time.
    // User-mode Sleep() uses milliseconds.
    // Calculation: 1ms = 10,000 units of 100ns.
    if (Interval->QuadPart < 0)
    {
        LONGLONG ms = -Interval->QuadPart / 10000;

        // Ensure we sleep at least 1ms if a tiny interval was provided
        if (ms == 0 && Interval->QuadPart != 0)
        {
            ms = 1;
        }

        Sleep((DWORD)ms);
    }
    else
    {
        // Absolute time waits are rare in these cache implementations.
        // For testing purposes, we treat it as a minimal yield.
        Sleep(1);
    }

    return 0; // STATUS_SUCCESS
}

inline PVOID MmGetSystemRoutineAddress(_In_ PUNICODE_STRING SystemRoutineName)
{
    if (!SystemRoutineName || !SystemRoutineName->Buffer)
    {
        return nullptr;
    }

    // UNICODE_STRING is not guaranteed to be null-terminated, 
    // so we compare based on exact byte length.
    const wchar_t* target = L"ZwYieldExecution";
    const USHORT targetBytes = (USHORT)(wcslen(target) * sizeof(wchar_t));

    if (SystemRoutineName->Length == targetBytes &&
        _wcsnicmp(SystemRoutineName->Buffer, target, targetBytes / sizeof(wchar_t)) == 0)
    {
        return (PVOID)&UmStub_ZwYieldExecution;
    }

    // Return nullptr for anything we haven't explicitly mocked,
    // exactly as the real kernel does for missing exports.
    return nullptr;
}

// Dynamically resolve and call the real RtlInitUnicodeString from ntdll.dll
inline VOID RtlInitUnicodeString(_Out_    PUNICODE_STRING DestinationString,
                                 _In_opt_ PCWSTR          SourceString)
{
    typedef VOID(NTAPI* PRTL_INIT_UNICODE_STRING)(PUNICODE_STRING, PCWSTR);
    static PRTL_INIT_UNICODE_STRING pRtlInit = nullptr;

    if (pRtlInit == nullptr)
    {
        // ntdll.dll is always loaded, so GetModuleHandle is safe and fast
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll)
        {
            pRtlInit = (PRTL_INIT_UNICODE_STRING)GetProcAddress(hNtdll, "RtlInitUnicodeString");
        }
    }

    if (pRtlInit)
    {
        pRtlInit(DestinationString, SourceString);
    }
}

// ----------------------------------------------------------------------------
// Processor/Topology API
// ----------------------------------------------------------------------------
inline USHORT KeQueryHighestNodeNumber() noexcept
{
    ULONG ulHighestNodeNumber = 0;
    if (GetNumaHighestNodeNumber(&ulHighestNodeNumber))
    {
        return static_cast<USHORT>(ulHighestNodeNumber);
    }

    return 0;
}

inline USHORT KeGetCurrentNodeNumber() noexcept
{
    PROCESSOR_NUMBER procNumber;
    GetCurrentProcessorNumberEx(&procNumber);

    USHORT usNodeNumber = 0;
    if (GetNumaProcessorNodeEx(&procNumber, &usNodeNumber))
    {
        return usNodeNumber;
    }

    return 0;
}

// ----------------------------------------------------------------------------
// KeQueryNodeActiveAffinity stub
// Maps directly to the Win32 NUMA APIs to simulate realistic kernel topology.
// ----------------------------------------------------------------------------
VOID KeQueryNodeActiveAffinity(_In_      USHORT          NodeNumber,
                               _Out_opt_ PGROUP_AFFINITY Affinity,
                               _Out_opt_ PUSHORT         Count)
{
    GROUP_AFFINITY LocalAffinity = { 0 };

    // Query the OS for the actual user-mode processor mask for this NUMA node
    BOOL bSuccess = GetNumaNodeProcessorMaskEx(NodeNumber, &LocalAffinity);
    if (!bSuccess)
    {
        // If the node doesn't exist or the call fails, zero out the mask 
        // to mimic a node with no active processors.
        LocalAffinity.Mask = 0;
        LocalAffinity.Group = 0;
    }

    if (Affinity != NULL)
    {
        *Affinity = LocalAffinity;
    }

    if (Count != NULL)
    {
        if (LocalAffinity.Mask == 0)
        {
            *Count = 0;
        }
        else
        {
            // Use hardware popcnt to count the number of active logical processors
#if defined(_M_AMD64) || defined(_M_ARM64)
            *Count = (USHORT)__popcnt64(LocalAffinity.Mask);
#else
            // Fallback for 32-bit
            // GROUP_AFFINITY.Mask is a KAFFINITY (ULONG_PTR)
            * Count = (USHORT)__popcnt((unsigned int)LocalAffinity.Mask);
#endif
        }
    }
}

// ----------------------------------------------------------------------------
// Concurrency & Memory Barriers
// ----------------------------------------------------------------------------
inline void KeMemoryBarrier()
{
    // MemoryBarrier is a macro in user-mode <windows.h> that emits the 
    // exact same mfence/dmb hardware instruction as the kernel equivalent.
    MemoryBarrier();
}

#include "OptimisticCache.h"
#include "test_optimistic_cache_common.h"

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);

    std::cout << "\n";
    std::cout << "=========================================================\n";
    std::cout << "            KM OPTIMISTIC CACHE TEST SUITE               \n";
    std::cout << "=========================================================\n";

    return RunAllCacheTests();
}
