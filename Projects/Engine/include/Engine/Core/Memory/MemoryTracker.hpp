// FBZZ Engine
// MemoryTracker.hpp | fbzz::core
// メモリ使用量を用途タグ別に集計するトラッカー
// アロケータの実装詳細と独立して、サブシステム単位の使用傾向を確認できるようにする。
#pragma once

#include "Engine/Core/Memory/Allocator.hpp"

#include <cstddef>
#include <utility>

namespace fbzz::core {

// タグごとの累計確保量・現在使用量・ピークを記録する。
// WHAT: 実メモリの確保は行わず、RecordAllocation/RecordFree で統計と未解放リストを更新する。
class MemoryTracker {
public:
    static constexpr std::size_t MAX_TRACKED_ALLOCATIONS = 4096;

    MemoryTracker() = default;

    void Reset();
    void RecordAllocation(MemoryTag tag, std::size_t size);
    void RecordFree(MemoryTag tag, std::size_t size);
    [[nodiscard]] bool RecordAllocation(const AllocationInfo& info);
    [[nodiscard]] bool RecordFree(void* ptr);

    [[nodiscard]] MemoryStats GetStats(MemoryTag tag) const;
    [[nodiscard]] MemoryStats GetTotalStats() const;
    [[nodiscard]] const char* GetTagName(MemoryTag tag) const;
    [[nodiscard]] std::size_t GetActiveAllocationCount() const { return m_activeAllocationCount; }
    [[nodiscard]] std::size_t GetDroppedAllocationCount() const { return m_droppedAllocationCount; }
    [[nodiscard]] std::size_t GetLeakCount() const { return m_activeAllocationCount; }
    [[nodiscard]] const AllocationInfo* GetActiveAllocations() const { return m_allocations; }
    [[nodiscard]] const AllocationInfo* GetLeak(std::size_t leakIndex) const;
    [[nodiscard]] std::size_t GetMaxTrackedAllocationCount() const { return MAX_TRACKED_ALLOCATIONS; }

private:
    static constexpr std::size_t TAG_COUNT = static_cast<std::size_t>(MemoryTag::COUNT);

    [[nodiscard]] std::size_t ToIndex(MemoryTag tag) const;
    [[nodiscard]] AllocationInfo* FindAllocation(void* ptr);
    [[nodiscard]] const AllocationInfo* FindAllocation(void* ptr) const;

    MemoryStats m_stats[TAG_COUNT] = {};
    AllocationInfo m_allocations[MAX_TRACKED_ALLOCATIONS] = {};
    std::size_t m_activeAllocationCount = 0;
    std::size_t m_droppedAllocationCount = 0;
    std::uint64_t m_nextAllocationId = 1;
};

// allocator から T を構築し、MemoryTracker に呼び出し元を記録する。
// WHY: リーク検出では「何バイト残ったか」だけでなく、生成位置と用途タグが必要になる。
template <class T, class... Args>
[[nodiscard]] T* CreateTrackedObject(Allocator& allocator,
                                     MemoryTracker& tracker,
                                     MemoryTag tag,
                                     const char* allocatorName,
                                     const char* file,
                                     int line,
                                     Args&&... args)
{
    T* object = CreateObject<T>(allocator, std::forward<Args>(args)...);
    if (object == nullptr) {
        return nullptr;
    }

    AllocationInfo info;
    info.pointer = object;
    info.size = sizeof(T);
    info.alignment = alignof(T);
    info.tag = tag;
    info.allocatorName = allocatorName;
    info.file = file;
    info.line = line;
    tracker.RecordAllocation(info);
    return object;
}

// T を破棄し、MemoryTracker の未解放台帳から削除する。
// WHAT: RecordFree が失敗しても、所有アロケータへは必ず Free を転送して実メモリの状態を戻す。
template <class T>
void DestroyTrackedObject(Allocator& allocator, MemoryTracker& tracker, T* ptr)
{
    if (ptr == nullptr) {
        return;
    }

    tracker.RecordFree(ptr);
    DestroyObject(allocator, ptr);
}

} // namespace fbzz::core

#define FBZZ_CREATE_TRACKED(allocator, tracker, tag, allocatorName, type, ...) \
    ::fbzz::core::CreateTrackedObject<type>(allocator, tracker, tag, allocatorName, __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)

#define FBZZ_DESTROY_TRACKED(allocator, tracker, ptr) \
    ::fbzz::core::DestroyTrackedObject(allocator, tracker, ptr)
