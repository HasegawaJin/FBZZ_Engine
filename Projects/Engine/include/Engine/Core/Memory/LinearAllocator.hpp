/// @file    LinearAllocator.hpp
/// @brief   線形にメモリを切り出す一括解放型アロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// フレーム中の作業バッファなど、個別解放が不要な短命データに使う。
#pragma once

#include "Engine/Core/Memory/Allocator.hpp"

#include <cstdint>

namespace fbzz::core {

// Allocate のたびに末尾を進め、Reset で先頭へ戻すアロケータ。
// WHY: 一時メモリでは個別 Free の管理コストが不要で、一括再利用により断片化を避けられる。
class LinearAllocator final : public Allocator {
public:
    LinearAllocator() = default;
    ~LinearAllocator() override;

    LinearAllocator(const LinearAllocator&)            = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    LinearAllocator(LinearAllocator&& other) noexcept;
    LinearAllocator& operator=(LinearAllocator&& other) noexcept;

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
    std::uint8_t* m_memory = nullptr;
    std::size_t  m_offset = 0;
    MemoryStats  m_stats;
};

} // namespace fbzz::core
