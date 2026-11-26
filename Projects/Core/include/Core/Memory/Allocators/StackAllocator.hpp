/// @file    StackAllocator.hpp
/// @brief   LIFO 解放を前提としたスタック型アロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 入れ子の一時処理やスコープ単位の作業領域を、確保と逆順に解放する。
#pragma once
#include <Core/Api.hpp>

#include "Core/Memory/Allocators/Allocator.hpp"

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
    /// @brief 返却時にスタックを巻き戻すため、各確保領域の直前に置く情報。
    struct AllocationHeader {
        std::size_t previousOffset = 0;           ///< この確保を解放したときに戻す、領域先頭からのバイト数。
        void*       previousAllocation = nullptr; ///< 解放後にスタックの頂上となる確保先。最初の確保では nullptr。
    };

    std::uint8_t* m_memory = nullptr; ///< このアロケータが所有する連続領域の先頭。
    std::size_t m_offset = 0; ///< 連続領域の先頭からスタック頂上までのバイト数。
    void* m_lastAllocation = nullptr; ///< 現在返却できる最後の確保先。スタックが空なら nullptr。
    MemoryStats m_stats; ///< スタック領域の使用量と確保回数を記録する統計。
};

} // namespace fbzz::core
