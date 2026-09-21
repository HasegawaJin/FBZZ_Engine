/// @file    PoolAllocator.hpp
/// @brief   固定長ブロックを再利用するプールアロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note Component など同サイズのオブジェクトを頻繁に生成破棄する場面の断片化を抑える。
#pragma once

#include "Core/Memory/Allocator.hpp"

#include <cstdint>

namespace fbzz::core {

/// @brief 同じサイズのブロックだけを扱う高速アロケータ。
/// @note 汎用ヒープより用途を狭める代わりに、空きリスト 1 本で O(1) の Allocate/Free を実現する。
class PoolAllocator final : public Allocator {
public:
    PoolAllocator() = default;
    ~PoolAllocator() override;

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    PoolAllocator(PoolAllocator&& other) noexcept;
    PoolAllocator& operator=(PoolAllocator&& other) noexcept;

    [[nodiscard]] bool Initialize(std::size_t blockSize,
                                  std::size_t blockCount,
                                  std::size_t alignment = alignof(std::max_align_t));
    void Shutdown();

    [[nodiscard]] void* Allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    [[nodiscard]] void* AllocateBlock();
    void Free(void* ptr) override;
    void Reset() override;

    [[nodiscard]] bool Owns(const void* ptr) const override;
    [[nodiscard]] MemoryStats GetStats() const override { return m_stats; }
    [[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }
    [[nodiscard]] std::size_t BlockSize() const { return m_blockSize; }
    [[nodiscard]] std::size_t BlockCount() const { return m_blockCount; }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    void*         m_rawMemory = nullptr;
    std::uint8_t* m_memory = nullptr;
    FreeNode*     m_freeList = nullptr;
    std::size_t   m_blockSize = 0;
    std::size_t   m_blockCount = 0;
    std::size_t   m_stride = 0;
    std::size_t   m_alignment = 0;
    MemoryStats   m_stats;
};

} /// @note namespace fbzz::core
