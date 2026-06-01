// FBZZ Engine
// FrameAllocator.hpp | fbzz::core
// 1 フレーム内の一時メモリを管理する専用アロケータ
// BeginFrame/EndFrame のタイミングで Reset し、短命データをまとめて再利用する。
#pragma once

#include "Engine/Core/Memory/LinearAllocator.hpp"

#include <cstdint>

namespace fbzz::core {

// ゲームループ単位で使い捨てる一時メモリ領域。
// WHY: レンダリングリストや一時的な計算結果はフレームを跨がないため、明示的な個別解放を不要にする。
class FrameAllocator final : public Allocator {
public:
    FrameAllocator() = default;
    ~FrameAllocator() override = default;

    FrameAllocator(const FrameAllocator&)            = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    FrameAllocator(FrameAllocator&&) noexcept = default;
    FrameAllocator& operator=(FrameAllocator&&) noexcept = default;

    [[nodiscard]] bool Initialize(std::size_t capacity);
    void Shutdown();

    void BeginFrame();
    void EndFrame();

    [[nodiscard]] void* Allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) override;
    void Free(void* ptr) override;
    void Reset() override;

    [[nodiscard]] bool Owns(const void* ptr) const override;
    [[nodiscard]] MemoryStats GetStats() const override;
    [[nodiscard]] bool IsInitialized() const { return m_allocator.IsInitialized(); }

private:
    LinearAllocator m_allocator;
    std::uint64_t   m_frameIndex = 0;
};

} // namespace fbzz::core
