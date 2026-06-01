// FBZZ Engine
// LinearAllocator.cpp | fbzz::core
// LinearAllocator の実装
// 連続メモリを前方へ切り出し、Reset でまとめて再利用する。
#include "Engine/Core/Memory/LinearAllocator.hpp"

#include <cstdlib>
#include <memory>
#include <utility>

namespace fbzz::core {

LinearAllocator::~LinearAllocator()
{
    Shutdown();
}

LinearAllocator::LinearAllocator(LinearAllocator&& other) noexcept
{
    *this = std::move(other);
}

LinearAllocator& LinearAllocator::operator=(LinearAllocator&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Shutdown();

    m_memory = other.m_memory;
    m_offset = other.m_offset;
    m_stats = other.m_stats;

    other.m_memory = nullptr;
    other.m_offset = 0;
    other.m_stats = {};
    return *this;
}

bool LinearAllocator::Initialize(std::size_t capacity)
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
    m_stats = {};
    m_stats.capacity = capacity;
    return true;
}

void LinearAllocator::Shutdown()
{
    if (m_memory != nullptr) {
        std::free(m_memory);
    }

    m_memory = nullptr;
    m_offset = 0;
    m_stats = {};
}

void* LinearAllocator::Allocate(std::size_t size, std::size_t alignment)
{
    assert(m_memory != nullptr);
    assert(size > 0);
    assert(IsPowerOfTwo(alignment));

    if (m_memory == nullptr || size == 0 || !IsPowerOfTwo(alignment)) {
        return nullptr;
    }

    void* current = m_memory + m_offset;
    std::size_t remaining = m_stats.capacity - m_offset;
    void* aligned = std::align(alignment, size, current, remaining);
    if (aligned == nullptr) {
        return nullptr;
    }

    const std::size_t padding =
        static_cast<std::size_t>(static_cast<std::uint8_t*>(aligned) - (m_memory + m_offset));
    const std::size_t required = padding + size;

    m_offset += required;
    m_stats.used = m_offset;
    ++m_stats.allocationCount;
    UpdatePeak(m_stats);

    return aligned;
}

void LinearAllocator::Free(void* ptr)
{
    // LinearAllocator は個別 Free を持たない。所有確認だけ行い、実際の再利用は Reset に集約する。
    assert(ptr == nullptr || Owns(ptr));
    (void)ptr;
}

void LinearAllocator::Reset()
{
    m_offset = 0;
    m_stats.used = 0;
    m_stats.allocationCount = 0;
    m_stats.freeCount = 0;
}

bool LinearAllocator::Owns(const void* ptr) const
{
    if (m_memory == nullptr || ptr == nullptr) {
        return false;
    }

    const auto* bytePtr = static_cast<const std::uint8_t*>(ptr);
    return bytePtr >= m_memory && bytePtr < (m_memory + m_stats.capacity);
}

} // namespace fbzz::core
