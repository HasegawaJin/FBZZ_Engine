/// @file    MemorySystem.cpp
/// @brief   MemorySystem の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note フレームアロケータとトラッカーの寿命を明示的に管理する。
#include "Core/Memory/MemorySystem.hpp"

#include <cassert>
#include <utility>

namespace fbzz::core {

MemorySystem::~MemorySystem()
{
    Shutdown();
}

MemorySystem::MemorySystem(MemorySystem&& other) noexcept
{
    *this = std::move(other);
}

MemorySystem& MemorySystem::operator=(MemorySystem&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Shutdown();

    m_frameAllocator = std::move(other.m_frameAllocator);
    m_tracker = other.m_tracker;
    m_isInitialized = other.m_isInitialized;

    other.m_tracker.Reset();
    other.m_isInitialized = false;
    return *this;
}

bool MemorySystem::Initialize(std::size_t frameAllocatorCapacity)
{
    Shutdown();

    if (!m_frameAllocator.Initialize(frameAllocatorCapacity)) {
        return false;
    }

    m_tracker.Reset();
    m_isInitialized = true;
    return true;
}

void MemorySystem::Shutdown()
{
    /// @note Shutdown で台帳を消す前に未解放を検出する。assert で止まった場合は GetLeak() から発生位置を追える。
    assert(!HasLeaks());

    m_frameAllocator.Shutdown();
    m_tracker.Reset();
    m_isInitialized = false;
}

void MemorySystem::BeginFrame()
{
    assert(m_isInitialized);

    if (!m_isInitialized) {
        return;
    }

    m_frameAllocator.BeginFrame();
}

void MemorySystem::EndFrame()
{
    assert(m_isInitialized);

    if (!m_isInitialized) {
        return;
    }

    m_frameAllocator.EndFrame();
}

bool MemorySystem::HasLeaks() const
{
    return m_tracker.GetLeakCount() > 0;
}

std::size_t MemorySystem::GetLeakCount() const
{
    return m_tracker.GetLeakCount();
}

} /// @note namespace fbzz::core
