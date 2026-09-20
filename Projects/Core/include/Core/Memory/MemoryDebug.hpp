/// @file    MemoryDebug.hpp
/// @brief   所有者が «まだ返していない» リソースを記録するデバッグ台帳。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 台帳へ載るリソースの所有者は ResourcePool ただ 1 つで、参照カウントが 0 になるのは Untrack を呼ぶその場所しか無いため、所有は unique_ptr とし、この台帳は登録/抹消を明示的に受け取るだけにする。
#pragma once

#include "Core/Memory/AllocationInfo.hpp"

#include <cstddef>
#include <vector>

namespace fbzz::core {

class MemoryDebug {
public:
    static constexpr std::size_t MAX_DEBUG_ALLOCATIONS = 8192;

    MemoryDebug();

    void Reset();

    /// @note 確保した実体を台帳へ載せる。@return 枠が尽きていたら false (機能自体は成立する)。
    [[nodiscard]] bool Track(const AllocationInfo& info);
    /// @note 返した実体を台帳から外す。@return 載っていなければ false。
    [[nodiscard]] bool Untrack(const void* ptr);

    [[nodiscard]] std::size_t GetLiveCount() const { return m_liveCount; }
    /// @note 生存中の実体が申告したバイト数の合計。フレーム単位の増加を見張るために O(1) で持つ。
    [[nodiscard]] std::size_t GetLiveBytes() const { return m_liveBytes; }
    [[nodiscard]] std::size_t GetDroppedCount() const { return m_droppedCount; }
    [[nodiscard]] const AllocationInfo* GetLive(std::size_t liveIndex) const;
    /// @brief 生存中の追跡情報を out へ追記する。
    /// @note GetLive() は呼ぶたび台帳を先頭から走るため、全件の列挙は要素数の 2 乗になる。
    void CollectLive(std::vector<AllocationInfo>& out) const;

private:
    [[nodiscard]] AllocationInfo* FindEntry(const void* ptr);

    std::vector<AllocationInfo> m_entries;
    std::size_t m_liveCount = 0;
    std::size_t m_liveBytes = 0;
    std::size_t m_droppedCount = 0;
    std::uint64_t m_nextAllocationId = 1;
};

} /// @note namespace fbzz::core
