/// @file    RenderEntitySlotMask.hpp
/// @brief   順序付きの描画抽出で候補スロットを絞る、一時ビットマスク。
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once

#include <Engine/Scene/References/Entity.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fbzz::scene::detail {

/// @brief シーンの順序と各コンポーネントの最終確認を保ち、候補外のスロットを除く。
/// @note 入力の span は構築時だけ参照する。作業領域の貸出中はコールバックを呼ばず、リソース処理は返却後に行う。
class RenderEntitySlotMask final {
public:
    static constexpr uint32_t kSlotCapacity = 1u << 20;

    explicit RenderEntitySlotMask(std::span<const EntityID> first,
        std::span<const EntityID> second = {}) noexcept
    {
        auto& scratch = ScratchForThread();
        if (scratch.busy) return;
        scratch.busy = true;
        size_t usedWords = 0;
        const auto measure = [&usedWords](std::span<const EntityID> entities) {
            for (const EntityID id : entities) {
                if (!id.IsValid() || id.index >= kSlotCapacity) return false;
                usedWords = (std::max)(usedWords, static_cast<size_t>(id.index / 64u) + 1u);
            }
            return true;
        };
        if (!measure(first) || !measure(second)) {
            scratch.busy = false;
            return;
        }
        /// @note 返却時にビットを消去しないため、貸出時に参照可能な全ワードを初期化する。
        std::fill_n(scratch.words.begin(), usedWords, uint64_t{0});
        const auto insert = [&scratch](std::span<const EntityID> entities) {
            for (const EntityID id : entities)
                scratch.words[id.index / 64u] |= uint64_t{1} << (id.index % 64u);
        };
        insert(first);
        insert(second);
        scratch.usedWords = usedWords;
        m_scratch = &scratch;
    }

    ~RenderEntitySlotMask()
    {
        if (!m_scratch) return;
        m_scratch->usedWords = 0;
        m_scratch->busy = false;
    }

    RenderEntitySlotMask(const RenderEntitySlotMask&) = delete;
    RenderEntitySlotMask& operator=(const RenderEntitySlotMask&) = delete;
    RenderEntitySlotMask(RenderEntitySlotMask&&) = delete;
    RenderEntitySlotMask& operator=(RenderEntitySlotMask&&) = delete;

    /// @brief ID が不明な場合や作業領域を借りられない場合は候補に残す。
    /// @note 世代の照合は後段の GetComponent が完全な EntityID を使って行う。
    bool MayContain(EntityID id) const noexcept
    {
        if (!m_scratch || !id.IsValid() || id.index >= kSlotCapacity) return true;
        const size_t word = id.index / 64u;
        return word < m_scratch->usedWords
            && (m_scratch->words[word] & (uint64_t{1} << (id.index % 64u))) != 0;
    }

private:
    struct Scratch {
        std::array<uint64_t, kSlotCapacity / 64u> words{};
        size_t usedWords = 0;
        bool busy = false;
    };

    /// @note 同一スレッドの抽出処理は翻訳単位をまたいで作業領域を共有する。
    static Scratch& ScratchForThread() noexcept
    {
        static thread_local Scratch scratch;
        return scratch;
    }

    Scratch* m_scratch = nullptr;
};

}
