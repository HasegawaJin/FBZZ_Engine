/// @file    ProfilerThreadState.hpp
/// @brief   計測器、公開データ、互換スコープのスレッド単位の所有境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include "PerformanceDescriptorTable.hpp"
#include "PerformanceFramePublication.hpp"
#include <memory>

namespace fbzz::profiler::detail {

struct LegacyScopeStack {
    void Clear()
    {
        m_tokens.clear();
        m_suppressedDepth = 0;
        m_overflowCheckpoint = {};
        m_overflowRootScopeId = 0;
    }

    std::vector<ProfileToken> m_tokens;
    uint32_t m_suppressedDepth = 0;
    ProfileCheckpoint m_overflowCheckpoint;
    uint64_t m_overflowRootScopeId = 0;
};

/// @note scope の親子関係、計測対象の文字列、確定フレームは記録したスレッドが所有する。
struct ProfilerThreadState {
    ProfileRecorder m_recorder;
    PerformanceDescriptorTable m_descriptorTable;
    PerformanceFramePublication m_publication;
    LegacyScopeStack m_legacyScopes;
    std::shared_ptr<const PerformanceCaptureFilter> m_frameFilter;
    uint64_t m_nextHostFrameSerial = 0;
};

[[nodiscard]] ProfilerThreadState& CurrentThreadState();

}
