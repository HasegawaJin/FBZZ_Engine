/// @file    MemoryDebug.hpp
/// @brief   スマートポインタ所有リソースの生存状況を追跡するデバッグ台帳。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// weak_ptr で監視するため所有権を増やさず、解放忘れの候補だけを検出する。
#pragma once

#include "Engine/Core/Memory/AllocationInfo.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace fbzz::core {

// std::shared_ptr で所有されるリソースを弱参照で追跡する。
// WHY: エンジン本体はスマートポインタで安全に所有し、MemoryDebug は寿命を観測するだけに留める。
class MemoryDebug {
public:
    static constexpr std::size_t MAX_DEBUG_ALLOCATIONS = 8192;

    MemoryDebug();

    void Reset();

    template <class T>
    [[nodiscard]] bool TrackShared(const std::shared_ptr<T>& ptr,
                                   MemoryTag tag,
                                   const char* allocatorName,
                                   const char* file,
                                   int line)
    {
        if (!ptr) {
            return false;
        }

        AllocationInfo info;
        info.pointer = ptr.get();
        info.size = sizeof(T);
        info.alignment = alignof(T);
        info.tag = tag;
        info.allocatorName = allocatorName;
        info.file = file;
        info.line = line;
        return TrackShared(ptr, info);
    }

    [[nodiscard]] bool TrackShared(std::shared_ptr<void> ptr, AllocationInfo info);
    [[nodiscard]] bool Untrack(const void* ptr);
    void SweepExpired();

    [[nodiscard]] std::size_t GetLiveCount() const;
    [[nodiscard]] std::size_t GetDroppedCount() const { return m_droppedCount; }
    [[nodiscard]] const AllocationInfo* GetLive(std::size_t liveIndex) const;

private:
    struct Entry {
        std::weak_ptr<void> weak;
        AllocationInfo info;
    };

    [[nodiscard]] Entry* FindEntry(const void* ptr);
    [[nodiscard]] const Entry* FindEntry(const void* ptr) const;

    std::vector<Entry> m_entries;
    std::size_t m_droppedCount = 0;
    std::uint64_t m_nextAllocationId = 1;
};

} // namespace fbzz::core

#define FBZZ_MEMORY_DEBUG_TRACK_SHARED(debug, ptr, tag, name) \
    (debug).TrackShared(ptr, tag, name, __FILE__, __LINE__)
