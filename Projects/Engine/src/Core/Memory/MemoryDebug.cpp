// FBZZ Engine
// MemoryDebug.cpp | fbzz::core
// MemoryDebug の実装
// weak_ptr の期限切れを掃除し、まだ生きているスマートポインタ所有リソースを列挙する。
#include "Engine/Core/Memory/MemoryDebug.hpp"

namespace fbzz::core {

MemoryDebug::MemoryDebug()
{
    // WHY: ResourcePool は ResourceManager に複数個内包され、Sandbox では ResourceManager がローカル変数になる。
    //      固定長配列を値メンバにするとスタックを圧迫するため、台帳本体は vector のヒープ領域に逃がす。
    m_entries.resize(MAX_DEBUG_ALLOCATIONS);
}

void MemoryDebug::Reset()
{
    for (Entry& entry : m_entries) {
        entry = {};
    }

    m_droppedCount = 0;
    m_nextAllocationId = 1;
}

bool MemoryDebug::TrackShared(std::shared_ptr<void> ptr, AllocationInfo info)
{
    if (!ptr || info.pointer == nullptr) {
        return false;
    }

    SweepExpired();

    if (FindEntry(info.pointer) != nullptr) {
        return false;
    }

    Entry* slot = nullptr;
    for (Entry& entry : m_entries) {
        if (!entry.info.isActive) {
            slot = &entry;
            break;
        }
    }

    if (slot == nullptr) {
        ++m_droppedCount;
        return false;
    }

    info.allocationId = m_nextAllocationId++;
    info.isActive = true;
    slot->weak = ptr;
    slot->info = info;
    return true;
}

bool MemoryDebug::Untrack(const void* ptr)
{
    Entry* entry = FindEntry(ptr);
    if (entry == nullptr) {
        return false;
    }

    *entry = {};
    return true;
}

void MemoryDebug::SweepExpired()
{
    for (Entry& entry : m_entries) {
        if (entry.info.isActive && entry.weak.expired()) {
            entry = {};
        }
    }
}

std::size_t MemoryDebug::GetLiveCount() const
{
    std::size_t count = 0;
    for (const Entry& entry : m_entries) {
        if (entry.info.isActive && !entry.weak.expired()) {
            ++count;
        }
    }
    return count;
}

const AllocationInfo* MemoryDebug::GetLive(std::size_t liveIndex) const
{
    std::size_t activeIndex = 0;
    for (const Entry& entry : m_entries) {
        if (!entry.info.isActive || entry.weak.expired()) {
            continue;
        }

        if (activeIndex == liveIndex) {
            return &entry.info;
        }

        ++activeIndex;
    }

    return nullptr;
}

MemoryDebug::Entry* MemoryDebug::FindEntry(const void* ptr)
{
    if (ptr == nullptr) {
        return nullptr;
    }

    for (Entry& entry : m_entries) {
        if (entry.info.isActive && entry.info.pointer == ptr) {
            return &entry;
        }
    }

    return nullptr;
}

const MemoryDebug::Entry* MemoryDebug::FindEntry(const void* ptr) const
{
    if (ptr == nullptr) {
        return nullptr;
    }

    for (const Entry& entry : m_entries) {
        if (entry.info.isActive && entry.info.pointer == ptr) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace fbzz::core
