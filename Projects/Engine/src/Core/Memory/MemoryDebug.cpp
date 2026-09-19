/// @file    MemoryDebug.cpp
/// @brief   MemoryDebug の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#include "Engine/Core/Memory/MemoryDebug.hpp"

namespace fbzz::core {

MemoryDebug::MemoryDebug()
{
    /// @note ResourcePool は ResourceManager に 7 個内包され Sandbox ではローカル変数になるため、
    ///       固定長配列を値メンバにするとスタックを圧迫する。vector にして回避する。
    m_entries.resize(MAX_DEBUG_ALLOCATIONS);
}

void MemoryDebug::Reset()
{
    for (AllocationInfo& entry : m_entries) {
        entry = {};
    }
    m_liveCount = 0;
    m_liveBytes = 0;
    m_droppedCount = 0;
    m_nextAllocationId = 1;
}

bool MemoryDebug::Track(const AllocationInfo& info)
{
    if (info.pointer == nullptr) {
        return false;
    }
    if (FindEntry(info.pointer) != nullptr) {
        return false;
    }

    for (AllocationInfo& entry : m_entries) {
        if (entry.isActive) {
            continue;
        }
        entry = info;
        entry.allocationId = m_nextAllocationId++;
        entry.isActive = true;
        ++m_liveCount;
        m_liveBytes += entry.size;
        return true;
    }

    ++m_droppedCount;
    return false;
}

bool MemoryDebug::Untrack(const void* ptr)
{
    AllocationInfo* entry = FindEntry(ptr);
    if (entry == nullptr) {
        return false;
    }

    /// @note 消す前に引く。空にしてから読むと 0 を引くことになる。
    m_liveBytes -= entry->size;
    --m_liveCount;
    *entry = {};
    return true;
}

const AllocationInfo* MemoryDebug::GetLive(std::size_t liveIndex) const
{
    std::size_t activeIndex = 0;
    for (const AllocationInfo& entry : m_entries) {
        if (!entry.isActive) {
            continue;
        }
        if (activeIndex == liveIndex) {
            return &entry;
        }
        ++activeIndex;
    }

    return nullptr;
}

void MemoryDebug::CollectLive(std::vector<AllocationInfo>& out) const
{
    for (const AllocationInfo& entry : m_entries) {
        if (entry.isActive) {
            out.push_back(entry);
        }
    }
}

AllocationInfo* MemoryDebug::FindEntry(const void* ptr)
{
    if (ptr == nullptr) {
        return nullptr;
    }

    for (AllocationInfo& entry : m_entries) {
        if (entry.isActive && entry.pointer == ptr) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace fbzz::core
