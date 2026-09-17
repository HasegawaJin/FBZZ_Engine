/// @file    StackAllocator.hpp
/// @brief   LIFO 解放を前提としたスタック型アロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// 入れ子の一時処理やスコープ単位の作業領域を、確保と逆順に解放する。
#pragma once

#include "Engine/Core/Memory/Allocator.hpp"

#include <cstdint>

namespace fbzz::core {

/// @brief 最後に確保した領域から順に Free するアロケータ。
/// @note LinearAllocator より細かく戻せるが、任意順解放を許さないことで実装とコストを小さく保つ。
class StackAllocator final : public Allocator {
public:
    StackAllocator() = default;
    ~StackAllocator() override;

    StackAllocator(const StackAllocator&)            = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    StackAllocator(StackAllocator&& other) noexcept;
    StackAllocator& operator=(StackAllocator&& other) noexcept;

    [[nodiscard]] bool Initialize(std::size_t capacity);
    void Shutdown();

    [[nodiscard]] void* Allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    void Free(void* ptr) override;
    void Reset() override;

    [[nodiscard]] bool Owns(const void* ptr) const override;
    [[nodiscard]] MemoryStats GetStats() const override { return m_stats; }
    [[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }

private:
    struct AllocationHeader {
        std::size_t previousOffset = 0;
        void*       previousAllocation = nullptr;
    };

    std::uint8_t* m_memory = nullptr;
    std::size_t  m_offset = 0;
    void*        m_lastAllocation = nullptr;
    MemoryStats  m_stats;
};

} // namespace fbzz::core
