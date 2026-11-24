/// @file    PoolAllocator.hpp
/// @brief   固定長ブロックを再利用するプールアロケータ。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note Component など同サイズのオブジェクトを頻繁に生成破棄する場面の断片化を抑える。
#pragma once
#include <Core/Api.hpp>

#include "Core/Memory/Allocator.hpp"

#include <cstdint>

namespace fbzz::core {

/// @brief 同じサイズのブロックだけを扱う高速アロケータ。
/// @note 汎用ヒープより用途を狭める代わりに、空きリスト 1 本で O(1) の Allocate/Free を実現する。
class PoolAllocator final : public Allocator {
public:
    PoolAllocator() = default;
    FBZZ_CORE_API ~PoolAllocator() override;

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    FBZZ_CORE_API PoolAllocator(PoolAllocator&& other) noexcept;
    FBZZ_CORE_API PoolAllocator& operator=(PoolAllocator&& other) noexcept;

    [[nodiscard]] FBZZ_CORE_API bool Initialize(std::size_t blockSize,
                                  std::size_t blockCount,
                                  std::size_t alignment = alignof(std::max_align_t));
    FBZZ_CORE_API void Shutdown();

    [[nodiscard]] FBZZ_CORE_API void* Allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    [[nodiscard]] FBZZ_CORE_API void* AllocateBlock();
    FBZZ_CORE_API void Free(void* ptr) override;
    FBZZ_CORE_API void Reset() override;

    [[nodiscard]] FBZZ_CORE_API bool Owns(const void* ptr) const override;
    [[nodiscard]] MemoryStats GetStats() const override { return m_stats; }
    [[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }
    [[nodiscard]] std::size_t BlockSize() const { return m_blockSize; }
    [[nodiscard]] std::size_t BlockCount() const { return m_blockCount; }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    /// @note 確保時に返された先頭。位置合わせで先頭をずらしても、解放にはこのアドレスを使う。
    void*         m_rawMemory = nullptr;
    /// @note 位置合わせ後の最初のブロック。利用者に渡すアドレスはここから数える。
    std::uint8_t* m_memory = nullptr;
    /// @note 空きブロックをつないだ列の先頭。確保では一つ外し、返却ではここに戻す。
    FreeNode*     m_freeList = nullptr;
    /// @note 1 回の要求で利用者が書き込める最大バイト数。
    std::size_t   m_blockSize = 0;
    /// @note 同時に渡せる固定長ブロックの総数。
    std::size_t   m_blockCount = 0;
    /// @note 隣のブロック先頭までのバイト数。空きリストの管理領域と位置合わせの余白を含む。
    std::size_t   m_stride = 0;
    /// @note 各ブロックの先頭で保証する境界。空きブロックにポインターを置くための境界も満たす。
    std::size_t   m_alignment = 0;
    /// @note 実際に利用した容量と確保回数を記録し、プールの大きさを判断する材料にする。
    MemoryStats   m_stats;
};

} /// @note namespace fbzz::core
