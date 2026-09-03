/// @file    MemoryDebug.hpp
/// @brief   所有者が «まだ返していない» リソースを記録するデバッグ台帳。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// WHY weak_ptr をやめたか (2026-09-02):
///   以前は shared_ptr を弱参照で見張り «期限切れ = 解放された» と判定していた。
///   だが台帳へ載るリソースの所有者は ResourcePool ただ 1 つで、参照カウントが
///   0 になるのはプールが手放した瞬間 — つまり Untrack を呼ぶその場所しか無い。
///   参照カウントぶんのコストと «誰かが共有し得る» という誤解だけが残っていたので、
///   所有は unique_ptr に戻し、この台帳は登録 / 抹消を明示的に受け取るだけにした。
#pragma once

#include "Engine/Core/Memory/AllocationInfo.hpp"

#include <cstddef>
#include <vector>

namespace fbzz::core {

class MemoryDebug {
public:
    static constexpr std::size_t MAX_DEBUG_ALLOCATIONS = 8192;

    MemoryDebug();

    void Reset();

    /// 確保した実体を台帳へ載せる。@ret 枠が尽きていたら false (機能自体は成立する)。
    [[nodiscard]] bool Track(const AllocationInfo& info);
    /// 返した実体を台帳から外す。@ret 載っていなければ false。
    [[nodiscard]] bool Untrack(const void* ptr);

    [[nodiscard]] std::size_t GetLiveCount() const { return m_liveCount; }
    /// 生存中の実体が申告したバイト数の合計。フレーム単位の増加を見張るために O(1) で持つ。
    [[nodiscard]] std::size_t GetLiveBytes() const { return m_liveBytes; }
    [[nodiscard]] std::size_t GetDroppedCount() const { return m_droppedCount; }
    [[nodiscard]] const AllocationInfo* GetLive(std::size_t liveIndex) const;
    /// 生存中の追跡情報を out へ追記する。
    /// WHY: GetLive() は呼ぶたび台帳を先頭から走るため、全件の列挙は要素数の 2 乗になる。
    void CollectLive(std::vector<AllocationInfo>& out) const;

private:
    [[nodiscard]] AllocationInfo* FindEntry(const void* ptr);

    std::vector<AllocationInfo> m_entries;
    std::size_t m_liveCount = 0;
    std::size_t m_liveBytes = 0;
    std::size_t m_droppedCount = 0;
    std::uint64_t m_nextAllocationId = 1;
};

} // namespace fbzz::core
