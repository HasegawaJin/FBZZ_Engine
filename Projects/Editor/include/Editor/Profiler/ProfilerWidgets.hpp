/// @file    ProfilerWidgets.hpp
/// @brief   パネル表示から独立した診断履歴と既存表示部品。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
namespace fbzz::editor {
struct EditorContext;
/// @note 所有履歴を差分投影する。非表示でもスパイク検出を続け、Freeze 中は派生表示を固定する。
/// @see Docs/design/profiler.md 履歴と CPU/GPU の対応契約。
void TickProfilerWidgets(EditorContext& ctx);
void ResetProfilerWidgets();
void DrawPerformanceCpu(EditorContext& ctx);
void DrawPerformanceRendering(EditorContext& ctx);
void DrawMemoryDebug(EditorContext& ctx);
}
