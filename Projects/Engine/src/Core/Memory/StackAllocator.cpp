// FBZZ Engine
// StackAllocator.cpp | fbzz::core
// StackAllocator の実装
// 確保順と逆順の Free に限定し、ヘッダー 1 個で高速に巻き戻す。
#include "Engine/Core/Memory/StackAllocator.hpp"

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

    m_memory = other.m_memory;
    m_offset = other.m_offset;
    m_lastAllocation = other.m_lastAllocation;
    m_stats = other.m_stats;

    other.m_memory = nullptr;
    other.m_offset = 0;
    other.m_lastAllocation = nullptr;
    other.m_stats = {};
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

    if (m_stats.capacity - m_offset < sizeof(AllocationHeader)) {
        return nullptr;
    }

    const std::size_t effectiveAlignment =
        (alignment > alignof(AllocationHeader)) ? alignment : alignof(AllocationHeader);
    void* payload = m_memory + m_offset + sizeof(AllocationHeader);
    std::size_t remaining = m_stats.capacity - m_offset - sizeof(AllocationHeader);
    payload = std::align(effectiveAlignment, size, payload, remaining);
    if (payload == nullptr) {
        return nullptr;
    }

    auto* headerAddress = static_cast<std::uint8_t*>(payload) - sizeof(AllocationHeader);
    auto* header = static_cast<AllocationHeader*>(static_cast<void*>(headerAddress));
    header->previousOffset = m_offset;
    header->previousAllocation = m_lastAllocation;

    const std::size_t required =
        static_cast<std::size_t>((static_cast<std::uint8_t*>(payload) + size) - (m_memory + m_offset));
    m_offset += required;
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

} // namespace fbzz::core
