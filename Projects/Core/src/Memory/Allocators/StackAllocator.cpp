/// @file    StackAllocator.cpp
/// @brief   StackAllocator の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 確保順と逆順の Free に限定し、ヘッダー 1 個で高速に巻き戻す。
#include "Core/Memory/Allocators/StackAllocator.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>

namespace fbzz::core {

StackAllocator::~StackAllocator()
{
    Shutdown();
}

StackAllocator::StackAllocator(StackAllocator&& other) noexcept
{
    *this = std::move(other);
}

StackAllocator& StackAllocator::operator=(StackAllocator&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Shutdown();

    m_memory = std::exchange(other.m_memory, nullptr);
    m_offset = std::exchange(other.m_offset, 0);
    m_lastAllocation = std::exchange(other.m_lastAllocation, nullptr);
    m_stats = std::exchange(other.m_stats, MemoryStats{});
    return *this;
}

bool StackAllocator::Initialize(std::size_t capacity)
{
    assert(capacity > 0);
    Shutdown();

    if (capacity == 0) {
        return false;
    }

    m_memory = static_cast<std::uint8_t*>(std::malloc(capacity));
    if (m_memory == nullptr) {
        return false;
    }

    m_offset = 0;
    m_lastAllocation = nullptr;
    m_stats = {};
    m_stats.capacity = capacity;
    return true;
}

void StackAllocator::Shutdown()
{
    if (m_memory != nullptr) {
        std::free(m_memory);
    }

    m_memory = nullptr;
    m_offset = 0;
    m_lastAllocation = nullptr;
    m_stats = {};
}

void* StackAllocator::Allocate(std::size_t size, std::size_t alignment)
{
    assert(m_memory != nullptr);
    assert(size > 0);
    assert(IsPowerOfTwo(alignment));

    if (m_memory == nullptr || size == 0 || !IsPowerOfTwo(alignment)) {
        return nullptr;
    }

    const std::size_t availableBytes = m_stats.capacity - m_offset;
    if (availableBytes < sizeof(AllocationHeader)) {
        return nullptr;
    }

    /// @note ペイロードの直前に置くヘッダーにもアラインメントが必要。
    const std::size_t payloadAlignment = std::max(alignment, alignof(AllocationHeader));
    void* payloadStart = m_memory + m_offset + sizeof(AllocationHeader);
    std::size_t payloadSpace = availableBytes - sizeof(AllocationHeader);
    auto* payload = static_cast<std::uint8_t*>(std::align(payloadAlignment, size, payloadStart, payloadSpace));
    if (payload == nullptr) {
        return nullptr;
    }

    auto* headerAddress = payload - sizeof(AllocationHeader);
    auto* header = static_cast<AllocationHeader*>(static_cast<void*>(headerAddress));
    header->previousOffset = m_offset;
    header->previousAllocation = m_lastAllocation;

    const std::size_t payloadOffset = static_cast<std::size_t>(payload - m_memory);
    m_offset = payloadOffset + size;
    m_lastAllocation = payload;
    m_stats.used = m_offset;
    ++m_stats.allocationCount;
    ++m_stats.activeCount;
    UpdatePeak(m_stats);

    return payload;
}

void StackAllocator::Free(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }

    assert(Owns(ptr));
    assert(ptr == m_lastAllocation);

    if (!Owns(ptr) || ptr != m_lastAllocation) {
        return;
    }

    auto* headerAddress = static_cast<std::uint8_t*>(ptr) - sizeof(AllocationHeader);
    const auto* header = static_cast<const AllocationHeader*>(static_cast<const void*>(headerAddress));
    m_offset = header->previousOffset;
    m_stats.used = m_offset;
    ++m_stats.freeCount;
    --m_stats.activeCount;
    m_lastAllocation = header->previousAllocation;
}

void StackAllocator::Reset()
{
    m_offset = 0;
    m_lastAllocation = nullptr;
    m_stats.used = 0;
    m_stats.allocationCount = 0;
    m_stats.freeCount = 0;
    m_stats.activeCount = 0;
}

bool StackAllocator::Owns(const void* ptr) const
{
    if (m_memory == nullptr || ptr == nullptr) {
        return false;
    }

    const auto* bytePtr = static_cast<const std::uint8_t*>(ptr);
    return bytePtr >= m_memory && bytePtr < (m_memory + m_stats.capacity);
}

} /// @note namespace fbzz::core
