/// @file    MemorySystem.hpp
/// @brief   エンジン全体で共有するメモリ管理の入口。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// フレーム用アロケータとメモリ統計を束ね、Application など所有者から明示的に初期化する。
#pragma once

#include "Engine/Core/Memory/FrameAllocator.hpp"
#include "Engine/Core/Memory/MemoryTracker.hpp"

namespace fbzz::core {

// メモリ関連サービスをまとめる所有型システム。
// WHY: グローバル変数を増やさず、上位の Application が寿命を管理できる形で導入する。
class MemorySystem {
public:
    MemorySystem() = default;
    ~MemorySystem();

    MemorySystem(const MemorySystem&)            = delete;
    MemorySystem& operator=(const MemorySystem&) = delete;

    MemorySystem(MemorySystem&& other) noexcept;
    MemorySystem& operator=(MemorySystem&& other) noexcept;

    [[nodiscard]] bool Initialize(std::size_t frameAllocatorCapacity);
    void Shutdown();

    void BeginFrame();
    void EndFrame();
    [[nodiscard]] bool HasLeaks() const;
    [[nodiscard]] std::size_t GetLeakCount() const;

    [[nodiscard]] FrameAllocator& GetFrameAllocator() { return m_frameAllocator; }
    [[nodiscard]] const FrameAllocator& GetFrameAllocator() const { return m_frameAllocator; }
    [[nodiscard]] MemoryTracker& GetTracker() { return m_tracker; }
    [[nodiscard]] const MemoryTracker& GetTracker() const { return m_tracker; }
    [[nodiscard]] bool IsInitialized() const { return m_isInitialized; }

private:
    FrameAllocator m_frameAllocator;
    MemoryTracker  m_tracker;
    bool           m_isInitialized = false;
};

} // namespace fbzz::core
