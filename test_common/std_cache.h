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

#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include <concepts>

// ----------------------------------------------------------------------------
// STL-Based Cache Baseline
// Designed for direct performance comparison against the lock-free COptimisticCache.
// Uses a reader-writer lock (std::shared_mutex) to allow concurrent lookups.
// ----------------------------------------------------------------------------
template <size_t TContextSize>
    requires (TContextSize == 0 || TContextSize == 64 || TContextSize == 128)
class StdCache
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
                                       void*       userData);

private:
    std::unordered_map<uint64_t, ContextType> m_map;
    mutable std::shared_mutex                 m_mutex;
    size_t                                    m_maxCapacity;

public:
    StdCache() : m_maxCapacity(0)
    {}

    ~StdCache()
    {
        Cleanup();
    }

    bool Initialize(size_t totalEntries)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        m_map.clear();
        m_maxCapacity = totalEntries;

        try
        {
            // Pre-allocate bucket count to minimize re-hashing during the benchmark
            m_map.reserve(totalEntries);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }

        return true;
    }

    void Cleanup()
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        m_map.clear();
        m_maxCapacity = 0;
    }

    bool CheckAndInsert(uint64_t key)
    {
        ContextType emptyContext{};

        return CheckAndInsert(key,
                              emptyContext,
                              InsertPolicy::KeepExisting,
                              nullptr,
                              nullptr,
                              nullptr);
    }

    bool CheckAndInsert(uint64_t    key,
        ContextType context)
    {
        return CheckAndInsert(key,
                              context,
                              InsertPolicy::Overwrite,
                              nullptr,
                              nullptr,
                              nullptr);
    }

    bool CheckAndInsert(uint64_t     key,
                        ContextType  context,
                        InsertPolicy policy,
                        ContextType* outExistingContext = nullptr,
                        uint64_t*    outEvictedKey      = nullptr,
                        ContextType* outEvictedContext  = nullptr)
    {
        if (key == 0 || m_maxCapacity == 0) [[unlikely]]
        {
            return false;
        }

        // Exclusive lock required for insertions/modifications
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            if constexpr (TContextSize > 0)
            {
                if (outExistingContext != nullptr)
                {
                    *outExistingContext = it->second;
                }

                if (policy == InsertPolicy::Overwrite)
                {
                    it->second = context;
                }
            }

            return true; // Indicates it was already present (Updated/Kept)
        }

        // Bounded capacity eviction
        if (m_map.size() >= m_maxCapacity)
        {
            // Arbitrary O(1) eviction to simulate the random victim selection 
            // without incurring the heavy read-lock penalty of a strict LRU list.
            auto evictIt = m_map.begin();

            if (evictIt != m_map.end())
            {
                // eviction capture logic
                if (outEvictedKey != nullptr)
                {
                    *outEvictedKey = evictIt->first;
                }

                if constexpr (TContextSize > 0)
                {
                    if (outEvictedContext != nullptr)
                    {
                        *outEvictedContext = evictIt->second;
                    }
                }

                m_map.erase(evictIt);
            }
        }

        if constexpr (TContextSize > 0)
        {
            m_map.emplace(key, context);
        }
        else
        {
            ContextType emptyContext{};
            m_map.emplace(key, emptyContext);
        }

        return false; // Indicates fresh insertion
    }


    // ------------------------------------------------------------------------
    // LookupContext (SFINAE restricted to Context-backed configurations)
    // ------------------------------------------------------------------------
    bool LookupContext(uint64_t     key,
                       ContextType& outContext) requires (TContextSize > 0)
    {
        ContextType emptyContext{};
        outContext = emptyContext;

        if (key == 0 || m_maxCapacity == 0) [[unlikely]]
        {
            return false;
        }

        // Shared lock allows concurrent readers
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_map.find(key);
        if (it != m_map.end()) [[likely]]
        {
            outContext = it->second;
            return true;
        }

        return false;
    }

    // ------------------------------------------------------------------------
    // Contains (for Context-free / HashSet usage)
    // ------------------------------------------------------------------------
    bool Contains(uint64_t key) const
    {
        if (key == 0 || m_maxCapacity == 0) [[unlikely]]
        {
            return false;
        }

        // Shared lock allows concurrent readers
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        return m_map.find(key) != m_map.end();
    }

    bool Delete(uint64_t key)
    {
        if (key == 0 || m_maxCapacity == 0) [[unlikely]]
        {
            return false;
        }

        // Exclusive lock required for erasure
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        size_t erasedCount = m_map.erase(key);
        if (erasedCount > 0) [[likely]]
        {
            return true;
        }

        return false;
    }

    void Enumerate(EnumerateCallback callback,
                   void*             userData)
    {
        if (!callback || m_maxCapacity == 0) [[unlikely]]
        {
            return;
        }

        // Shared lock allows concurrent readers during enumeration
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        for (const auto& pair : m_map)
        {
            if constexpr (TContextSize > 0)
            {
                callback(pair.first, pair.second, userData);
            }
            else
            {
                ContextType emptyContext{};
                callback(pair.first, emptyContext, userData);
            }
        }
    }
};
