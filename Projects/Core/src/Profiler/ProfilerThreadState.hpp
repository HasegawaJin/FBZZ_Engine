/// @file    ProfilerThreadState.hpp
/// @brief   計測器、公開データ、互換スコープのスレッド単位の所有境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include "PerformanceDescriptorTable.hpp"
#include "PerformanceFramePublication.hpp"
#include <memory>

namespace fbzz::profiler::detail {

/// @brief トークンを返さない旧 API で開始と終了を対応付けるスタック。
struct LegacyScopeStack {
    void Clear()
    {
        m_tokens.clear();
        m_suppressedDepth = 0;
        m_overflowCheckpoint = {};
        m_overflowRootScopeId = 0;
    }

    std::vector<ProfileToken> m_tokens;     ///< 互換 API で開始し、対応する EndSample を待つトークン列。
    uint32_t m_suppressedDepth = 0;         ///< スタックの上限を超えて開始した未終了区間の数。
    ProfileCheckpoint m_overflowCheckpoint; ///< 最初の上限超過の直前に保存した計測器の復元位置。
    uint64_t m_overflowRootScopeId = 0;     ///< 最初に上限を超えた区間の ID。別の抑制区間への復元を防ぐ。
};

/// @brief 一つのスレッドが記録・公開する計測データ。
/// @note 区間の親子関係、計測対象の文字列、確定したフレームの結果は、記録したスレッドが所有する。
struct ProfilerThreadState {
    ProfileRecorder m_recorder;                   ///< このスレッドの CPU 区間と入れ子を記録する計測器。
    PerformanceDescriptorTable m_descriptorTable; ///< このスレッドの記述子キーを割り当て、名前と分類を所有する表。
    PerformanceFramePublication m_publication;    ///< 確定済みフレームと旧 API 向けの表示レコードを保持する。
    LegacyScopeStack m_legacyScopes;              ///< このスレッドの BeginSample と EndSample の対応を保持する。
    std::shared_ptr<const PerformanceCaptureFilter> m_frameFilter; ///< フレーム開始時に取得し、そのフレーム中は固定する収集条件。
    uint64_t m_nextHostFrameSerial = 0;           ///< 引数なしの BeginFrame がフレーム番号を採番するためのカウンター。
};

[[nodiscard]] ProfilerThreadState& CurrentThreadState();

}
