/// @file    RenderExtractionPhases.hpp
/// @brief   明示的に選択された描画抽出の段階別計測。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Core/Profiler/ProfileScope.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fbzz::scene::detail {

/// @note 段階別計測は親マーカーが要求された場合だけ開始する。記録対象のフレームは適用済みフィルターで決まる。
class RenderExtractionPhases {
public:
    explicit RenderExtractionPhases(std::string_view parentMarker)
    {
        if (profiler::Profiler::IsEnabled()) {
            const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
            m_enabled = filter.scope == parentMarker && (filter.category.empty() || filter.category == "General");
        }
    }

    void Begin(const char* name)
    {
        if (!m_enabled) return;
        m_scope.reset();
        m_scope.emplace(profiler::ProfilerMarker(name, "General"));
    }

    void End() { m_scope.reset(); }

private:
    std::optional<profiler::ProfileScope> m_scope;
    bool m_enabled = false;
};

/// @note 有効時だけ経過時間 [ns] を積算する。積算先はタイマーより長く生存すること。
template<bool DIAGNOSTICS_ENABLED = true>
class RenderPhaseTimer {
public:
    explicit RenderPhaseTimer(int64_t& elapsedNs, bool enabled = true) noexcept
    {
        if constexpr (DIAGNOSTICS_ENABLED) {
            if (enabled) {
                m_elapsedNs = &elapsedNs;
                m_start = std::chrono::steady_clock::now();
            }
        }
    }

    RenderPhaseTimer(const RenderPhaseTimer&) = delete;
    RenderPhaseTimer& operator=(const RenderPhaseTimer&) = delete;
    ~RenderPhaseTimer() noexcept { Finish(); }

    /// @note 明示的に終了した区間は、破棄時に再集計しない。
    void Finish() noexcept
    {
        if constexpr (DIAGNOSTICS_ENABLED) {
            if (!m_elapsedNs) return;
            *m_elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_start).count();
            m_elapsedNs = nullptr;
        }
    }

private:
    int64_t* m_elapsedNs = nullptr;
    std::chrono::steady_clock::time_point m_start{};
};

}
