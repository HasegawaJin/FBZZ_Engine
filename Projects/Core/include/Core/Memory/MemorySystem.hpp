/// @file    MemorySystem.hpp
/// @brief   エンジン全体で共有するメモリ管理の入口。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note フレーム用アロケータとメモリ統計を束ね、Application など所有者から明示的に初期化する。
#pragma once
#include <Core/Api.hpp>

#include "Core/Memory/FrameAllocator.hpp"
#include "Core/Memory/MemoryTracker.hpp"

namespace fbzz::core {

/// @brief メモリ関連サービスをまとめる所有型システム。
/// @note グローバル変数を増やさず、上位の Application が寿命を管理できる形で導入する。
class MemorySystem {
public:
    MemorySystem() = default;
    FBZZ_CORE_API ~MemorySystem();

    MemorySystem(const MemorySystem&)            = delete;
    MemorySystem& operator=(const MemorySystem&) = delete;

    FBZZ_CORE_API MemorySystem(MemorySystem&& other) noexcept;
    FBZZ_CORE_API MemorySystem& operator=(MemorySystem&& other) noexcept;

    [[nodiscard]] FBZZ_CORE_API bool Initialize(std::size_t frameAllocatorCapacity);
    FBZZ_CORE_API void Shutdown();

    FBZZ_CORE_API void BeginFrame();
    FBZZ_CORE_API void EndFrame();
    [[nodiscard]] FBZZ_CORE_API bool HasLeaks() const;
    [[nodiscard]] FBZZ_CORE_API std::size_t GetLeakCount() const;

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

} /// @note namespace fbzz::core
