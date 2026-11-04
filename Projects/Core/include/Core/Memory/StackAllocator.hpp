/// @file    StackAllocator.hpp
/// @brief   LIFO 解放を前提としたスタック型アロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 入れ子の一時処理やスコープ単位の作業領域を、確保と逆順に解放する。
#pragma once
#include <Core/Api.hpp>

#include "Core/Memory/Allocator.hpp"

#include <cstdint>

namespace fbzz::core {

/// @brief 最後に確保した領域から順に Free するアロケータ。
/// @note LinearAllocator より細かく戻せるが、任意順解放を許さないことで実装とコストを小さく保つ。
class StackAllocator final : public Allocator {
public:
    StackAllocator() = default;
    FBZZ_CORE_API ~StackAllocator() override;

    StackAllocator(const StackAllocator&)            = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    FBZZ_CORE_API StackAllocator(StackAllocator&& other) noexcept;
    FBZZ_CORE_API StackAllocator& operator=(StackAllocator&& other) noexcept;

    [[nodiscard]] FBZZ_CORE_API bool Initialize(std::size_t capacity);
    FBZZ_CORE_API void Shutdown();

    [[nodiscard]] FBZZ_CORE_API void* Allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    FBZZ_CORE_API void Free(void* ptr) override;
    FBZZ_CORE_API void Reset() override;

    [[nodiscard]] FBZZ_CORE_API bool Owns(const void* ptr) const override;
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

} /// @note namespace fbzz::core
