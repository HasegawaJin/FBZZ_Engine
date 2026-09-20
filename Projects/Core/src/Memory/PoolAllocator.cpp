/// @file    PoolAllocator.cpp
/// @brief   PoolAllocator の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 固定長ブロックを空きリストで管理し、同サイズ割り当てを O(1) で再利用する。
#include "Core/Memory/PoolAllocator.hpp"

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

    m_rawMemory = other.m_rawMemory;
    m_memory = other.m_memory;
    m_freeList = other.m_freeList;
    m_blockSize = other.m_blockSize;
    m_blockCount = other.m_blockCount;
    m_stride = other.m_stride;
    m_alignment = other.m_alignment;
    m_stats = other.m_stats;

    other.m_rawMemory = nullptr;
    other.m_memory = nullptr;
    other.m_freeList = nullptr;
    other.m_blockSize = 0;
    other.m_blockCount = 0;
    other.m_stride = 0;
    other.m_alignment = 0;
    other.m_stats = {};
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

    const std::size_t effectiveAlignment =
        (alignment > alignof(FreeNode)) ? alignment : alignof(FreeNode);
    const std::size_t minBlockSize = (blockSize > sizeof(FreeNode)) ? blockSize : sizeof(FreeNode);
    if (minBlockSize > std::numeric_limits<std::size_t>::max() - (effectiveAlignment - 1)) {
        return false;
    }

    m_blockSize = blockSize;
    m_blockCount = blockCount;
    m_stride = AlignSize(minBlockSize, effectiveAlignment);
    m_alignment = effectiveAlignment;

    if (m_stride > std::numeric_limits<std::size_t>::max() / m_blockCount) {
        Shutdown();
        return false;
    }

    const std::size_t capacity = m_stride * m_blockCount;
    if (capacity > std::numeric_limits<std::size_t>::max() - (effectiveAlignment - 1)) {
        Shutdown();
        return false;
    }

    const std::size_t reserved = capacity + effectiveAlignment - 1;
    m_rawMemory = std::malloc(reserved);
    if (m_rawMemory == nullptr) {
        Shutdown();
        return false;
    }

    void* aligned = m_rawMemory;
    std::size_t remaining = reserved;
    if (std::align(effectiveAlignment, capacity, aligned, remaining) == nullptr) {
        Shutdown();
        return false;
    }

    m_memory = static_cast<std::uint8_t*>(aligned);
    Reset();
    m_stats.capacity = capacity;
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
