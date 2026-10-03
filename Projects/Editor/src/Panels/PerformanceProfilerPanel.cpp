/// @file    PerformanceProfilerPanel.cpp
/// @brief   CPU と Rendering の独立した Performance 表示。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Panels/PerformanceProfilerPanel.hpp>
#include <Editor/Profiler/ProfilerWidgets.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <imgui.h>
namespace fbzz::editor {
void PerformanceProfilerPanel::OnLoadSettings(const EditorSettings& settings) { profiler::Profiler::RequestRecording(settings.performanceProfilerRecording); }
void PerformanceProfilerPanel::OnSaveSettings(EditorSettings& settings) const { settings.performanceProfilerRecording = profiler::Profiler::IsEnabled(); }
void PerformanceProfilerPanel::OnRenderContent(EditorContext& ctx)
{
    if (ImGui::BeginTabBar("PerformanceTabs")) {
        if (ImGui::BeginTabItem("CPU")) {
            DrawPerformanceCpu(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
            DrawPerformanceRendering(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
}
