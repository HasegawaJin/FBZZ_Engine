/// @file    MemoryTracker.cpp
/// @brief   MemoryTracker の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// 固定配列でタグ別統計を持ち、集計そのものが追加メモリを要求しないようにする。
#include "Engine/Core/Memory/MemoryTracker.hpp"

namespace fbzz::core {
namespace {

constexpr const char* TAG_NAMES[] = {
    "Unknown",
    "Core",
    "Renderer",
    "Scene",
    "Physics",
    "Audio",
    "Asset",
    "Editor",
};

} // namespace

void MemoryTracker::Reset()
{
    for (MemoryStats& stats : m_stats) {
        stats = {};
    }

    for (AllocationInfo& info : m_allocations) {
        info = {};
    }

    m_activeAllocationCount = 0;
    m_droppedAllocationCount = 0;
    m_nextAllocationId = 1;
}

void MemoryTracker::RecordAllocation(MemoryTag tag, std::size_t size)
{
    MemoryStats& stats = m_stats[ToIndex(tag)];
    stats.used += size;
    ++stats.allocationCount;
    ++stats.activeCount;
    UpdatePeak(stats);
}

void MemoryTracker::RecordFree(MemoryTag tag, std::size_t size)
{
    MemoryStats& stats = m_stats[ToIndex(tag)];
    if (stats.used >= size) {
        stats.used -= size;
    } else {
        stats.used = 0;
    }
    ++stats.freeCount;
    if (stats.activeCount > 0) {
        --stats.activeCount;
    }
}

bool MemoryTracker::RecordAllocation(const AllocationInfo& info)
{
    if (info.pointer == nullptr || info.size == 0) {
        return false;
    }

    if (FindAllocation(info.pointer) != nullptr) {
        return false;
    }

    AllocationInfo* slot = nullptr;
    for (AllocationInfo& allocation : m_allocations) {
        if (!allocation.isActive) {
            slot = &allocation;
            break;
        }
    }

    if (slot == nullptr) {
        ++m_droppedAllocationCount;
        RecordAllocation(info.tag, info.size);
        return false;
    }

    *slot = info;
    slot->allocationId = m_nextAllocationId++;
    slot->isActive = true;

    ++m_activeAllocationCount;
    RecordAllocation(slot->tag, slot->size);
    return true;
}

bool MemoryTracker::RecordFree(void* ptr)
{
    AllocationInfo* info = FindAllocation(ptr);
    if (info == nullptr) {
        return false;
    }

    RecordFree(info->tag, info->size);
    *info = {};

    if (m_activeAllocationCount > 0) {
        --m_activeAllocationCount;
    }
    return true;
}

MemoryStats MemoryTracker::GetStats(MemoryTag tag) const
{
    return m_stats[ToIndex(tag)];
}

MemoryStats MemoryTracker::GetTotalStats() const
{
    MemoryStats total;
    for (const MemoryStats& stats : m_stats) {
        total.capacity += stats.capacity;
        total.used += stats.used;
        total.peakUsed += stats.peakUsed;
        total.allocationCount += stats.allocationCount;
        total.freeCount += stats.freeCount;
        total.activeCount += stats.activeCount;
    }
    return total;
}

const char* MemoryTracker::GetTagName(MemoryTag tag) const
{
    return TAG_NAMES[ToIndex(tag)];
}

const AllocationInfo* MemoryTracker::GetLeak(std::size_t leakIndex) const
{
    std::size_t activeIndex = 0;
    for (const AllocationInfo& info : m_allocations) {
        if (!info.isActive) {
            continue;
        }

        if (activeIndex == leakIndex) {
            return &info;
        }

        ++activeIndex;
    }

    return nullptr;
}

std::size_t MemoryTracker::ToIndex(MemoryTag tag) const
{
    const std::size_t index = static_cast<std::size_t>(tag);
    if (index >= TAG_COUNT) {
        return static_cast<std::size_t>(MemoryTag::UNKNOWN);
    }
    return index;
}

AllocationInfo* MemoryTracker::FindAllocation(void* ptr)
{
    if (ptr == nullptr) {
        return nullptr;
    }

    for (AllocationInfo& info : m_allocations) {
        if (info.isActive && info.pointer == ptr) {
            return &info;
        }
    }

    return nullptr;
}

const AllocationInfo* MemoryTracker::FindAllocation(void* ptr) const
{
    if (ptr == nullptr) {
        return nullptr;
    }

    for (const AllocationInfo& info : m_allocations) {
        if (info.isActive && info.pointer == ptr) {
            return &info;
        }
    }

    return nullptr;
}

} // namespace fbzz::core
