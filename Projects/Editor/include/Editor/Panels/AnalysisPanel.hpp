/// @file    AnalysisPanel.hpp
/// @brief   旧 AnalysisPanel 名を Performance Profiler に接続する互換ヘッダー。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <Editor/Panels/PerformanceProfilerPanel.hpp>

namespace fbzz::editor {

/// @note 旧 include と型名を維持し、保存済み Analysis docking ID は新パネルが引き継ぐ。
using AnalysisPanel = PerformanceProfilerPanel;

} /// @note namespace fbzz::editor
