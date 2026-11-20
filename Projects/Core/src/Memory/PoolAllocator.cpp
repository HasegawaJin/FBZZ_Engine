/// @file    PoolAllocator.cpp
/// @brief   PoolAllocator の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 固定長ブロックを空きリストで管理し、同サイズ割り当てを O(1) で再利用する。
#include "Core/Memory/PoolAllocator.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace fbzz::core {

PoolAllocator::~PoolAllocator()
{
    Shutdown();
}

PoolAllocator::PoolAllocator(PoolAllocator&& other) noexcept
{
    *this = std::move(other);
}

PoolAllocator& PoolAllocator::operator=(PoolAllocator&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Shutdown();

    m_rawMemory = std::exchange(other.m_rawMemory, nullptr);
    m_memory = std::exchange(other.m_memory, nullptr);
    m_freeList = std::exchange(other.m_freeList, nullptr);
    m_blockSize = std::exchange(other.m_blockSize, 0);
    m_blockCount = std::exchange(other.m_blockCount, 0);
    m_stride = std::exchange(other.m_stride, 0);
    m_alignment = std::exchange(other.m_alignment, 0);
    m_stats = std::exchange(other.m_stats, MemoryStats{});
    return *this;
}

bool PoolAllocator::Initialize(std::size_t blockSize, std::size_t blockCount, std::size_t alignment)
{
    assert(blockSize > 0);
    assert(blockCount > 0);
    assert(IsPowerOfTwo(alignment));

    Shutdown();

    if (blockSize == 0 || blockCount == 0 || !IsPowerOfTwo(alignment)) {
        return false;
    }

    /// @note 空きブロックには次の空きブロックへのポインタを格納する。
    const std::size_t blockAlignment = std::max(alignment, alignof(FreeNode));
    const std::size_t minimumBlockBytes = std::max(blockSize, sizeof(FreeNode));
    const std::size_t maxAlignmentPadding = blockAlignment - 1;
    constexpr std::size_t MAX_SIZE = std::numeric_limits<std::size_t>::max();
    if (minimumBlockBytes > MAX_SIZE - maxAlignmentPadding) {
        return false;
    }

    const std::size_t blockStride = AlignSize(minimumBlockBytes, blockAlignment);
    if (blockStride > MAX_SIZE / blockCount) {
        return false;
    }

    const std::size_t capacity = blockStride * blockCount;
    if (capacity > MAX_SIZE - maxAlignmentPadding) {
        return false;
    }

    const std::size_t reservedBytes = capacity + maxAlignmentPadding;
    m_rawMemory = std::malloc(reservedBytes);
    if (m_rawMemory == nullptr) {
        return false;
    }

    void* alignedMemory = m_rawMemory;
    std::size_t remainingBytes = reservedBytes;
    if (std::align(blockAlignment, capacity, alignedMemory, remainingBytes) == nullptr) {
        Shutdown();
        return false;
    }

    m_memory = static_cast<std::uint8_t*>(alignedMemory);
    m_blockSize = blockSize;
    m_blockCount = blockCount;
    m_stride = blockStride;
    m_alignment = blockAlignment;
    m_stats.capacity = capacity;
    Reset();
    return true;
}

void PoolAllocator::Shutdown()
{
    if (m_rawMemory != nullptr) {
        std::free(m_rawMemory);
    }

    m_rawMemory = nullptr;
    m_memory = nullptr;
    m_freeList = nullptr;
    m_blockSize = 0;
    m_blockCount = 0;
    m_stride = 0;
    m_alignment = 0;
    m_stats = {};
}

void* PoolAllocator::Allocate(std::size_t size, std::size_t alignment)
{
    assert(size <= m_blockSize);
    assert(alignment <= m_alignment);
    assert(IsPowerOfTwo(alignment));

    if (size > m_blockSize || alignment > m_alignment || !IsPowerOfTwo(alignment)) {
        return nullptr;
    }

    return AllocateBlock();
}

void* PoolAllocator::AllocateBlock()
{
    assert(m_memory != nullptr);

    if (m_memory == nullptr || m_freeList == nullptr) {
        return nullptr;
    }

    FreeNode* node = m_freeList;
    m_freeList = node->next;

    m_stats.used += m_stride;
    ++m_stats.allocationCount;
    UpdatePeak(m_stats);

    return node;
}

void PoolAllocator::Free(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }

    assert(Owns(ptr));

    if (!Owns(ptr)) {
        return;
    }

    auto* node = static_cast<FreeNode*>(ptr);
    node->next = m_freeList;
    m_freeList = node;

    assert(m_stats.used >= m_stride);
    if (m_stats.used >= m_stride) {
        m_stats.used -= m_stride;
    }
    ++m_stats.freeCount;
}

void PoolAllocator::Reset()
{
    if (m_memory == nullptr) {
        return;
    }

    m_freeList = nullptr;
    for (std::size_t i = 0; i < m_blockCount; ++i) {
        auto* node = static_cast<FreeNode*>(static_cast<void*>(m_memory + i * m_stride));
        node->next = m_freeList;
        m_freeList = node;
    }

    m_stats.used = 0;
    m_stats.allocationCount = 0;
    m_stats.freeCount = 0;
}

bool PoolAllocator::Owns(const void* ptr) const
{
    if (m_memory == nullptr || ptr == nullptr) {
        return false;
    }

    const auto* bytePtr = static_cast<const std::uint8_t*>(ptr);
    const std::uint8_t* begin = m_memory;
    const std::uint8_t* end = m_memory + m_stats.capacity;
    if (bytePtr < begin || bytePtr >= end) {
        return false;
    }

    const std::size_t offset = static_cast<std::size_t>(bytePtr - begin);
    return (offset % m_stride) == 0;
}

} /// @note namespace fbzz::core
