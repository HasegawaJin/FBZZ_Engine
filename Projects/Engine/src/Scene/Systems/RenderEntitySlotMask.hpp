/// @file    RenderEntitySlotMask.hpp
/// @brief   Bounded invocation-local candidate membership for ordered render collection.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fbzz::scene::detail {

/// @brief Prunes unrelated slots without changing Scene order or final component checks.
/// @note The source spans are consumed during construction and never retained. Collection
///       must remain callback-free while the lease is alive; resource work follows release.
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
        std::fill_n(m_scratch->words.begin(), m_scratch->usedWords, uint64_t{0});
        m_scratch->usedWords = 0;
        m_scratch->busy = false;
    }

    RenderEntitySlotMask(const RenderEntitySlotMask&) = delete;
    RenderEntitySlotMask& operator=(const RenderEntitySlotMask&) = delete;
    RenderEntitySlotMask(RenderEntitySlotMask&&) = delete;
    RenderEntitySlotMask& operator=(RenderEntitySlotMask&&) = delete;

    /// @brief Unknown IDs and unavailable scratch preserve the original scan conservatively.
    /// @note Generation is deliberately ignored: the existing GetComponent checks the full ID.
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

    /// @note Inline function-local TLS shares one numeric scratch pool across extractor TUs.
    static Scratch& ScratchForThread() noexcept
    {
        static thread_local Scratch scratch;
        return scratch;
    }

    Scratch* m_scratch = nullptr;
};

} // namespace fbzz::scene::detail
