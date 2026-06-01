// FBZZ Engine
// FrameAllocator.cpp | fbzz::core
// FrameAllocator の実装
// LinearAllocator をフレーム境界で Reset する薄いラッパーとして扱う。
#include "Engine/Core/Memory/FrameAllocator.hpp"

namespace fbzz::core {

bool FrameAllocator::Initialize(std::size_t capacity)
{
    return m_allocator.Initialize(capacity);
}

void FrameAllocator::Shutdown()
{
    m_allocator.Shutdown();
    m_frameIndex = 0;
}

void FrameAllocator::BeginFrame()
{
    m_allocator.Reset();
    ++m_frameIndex;
}

void FrameAllocator::EndFrame()
{
    // EndFrame は現時点では統計確認用の境界。実メモリの再利用は次の BeginFrame で行う。
}

void* FrameAllocator::Allocate(std::size_t size, std::size_t alignment)
{
    return m_allocator.Allocate(size, alignment);
}

void FrameAllocator::Free(void* ptr)
{
    m_allocator.Free(ptr);
}

void FrameAllocator::Reset()
{
    m_allocator.Reset();
}

bool FrameAllocator::Owns(const void* ptr) const
{
    return m_allocator.Owns(ptr);
}

MemoryStats FrameAllocator::GetStats() const
{
    return m_allocator.GetStats();
}

} // namespace fbzz::core
