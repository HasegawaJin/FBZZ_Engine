/// @file    MemoryDebugPanel.cpp
/// @brief   Memory Debug の独立ウィンドウ。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Panels/MemoryDebugPanel.hpp>
#include <Editor/Profiler/ProfilerWidgets.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Util/EditorSettings.hpp>
namespace fbzz::editor {
void MemoryDebugPanel::OnLoadSettings(const EditorSettings& settings) { SetMemoryHistoryRecording(settings.memoryProfilerRecording); }
void MemoryDebugPanel::OnSaveSettings(EditorSettings& settings) const { settings.memoryProfilerRecording = IsMemoryHistoryRecording(); }
void MemoryDebugPanel::OnRenderContent(EditorContext& ctx) { DrawMemoryDebug(ctx); }
}
