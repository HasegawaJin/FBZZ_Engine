/// @file    PerformanceProfilerPanel.cpp
/// @brief   CPU と Rendering の独立した Performance 表示。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Panels/PerformanceProfilerPanel.hpp>
#include <Editor/Profiler/ProfilerWidgets.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <cstdio>
#include <Editor/Util/EditorSettings.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <imgui.h>
namespace fbzz::editor {
namespace {
void DrawCollectionControls(EditorContext& ctx)
{
    if (!ImGui::CollapsingHeader("CPU collection range")) return;
    static char category[129] = {};
    static char scope[257] = {};
    static profiler::PerformanceCaptureFilter observed;
    const auto requested = profiler::Profiler::GetRequestedCaptureFilter();
    if (requested != observed) {
        std::snprintf(category, sizeof(category), "%s", requested.category.c_str());
        std::snprintf(scope, sizeof(scope), "%s", requested.scope.c_str());
        observed = requested;
    }
    ImGui::BeginDisabled(GetPerformanceBenchmarkCapture().IsActive());
    ImGui::InputTextWithHint("Category to collect", "Exact category; empty = all", category, sizeof(category));
    ImGui::InputTextWithHint("Scope to collect", "Case-sensitive name substring; empty = all", scope, sizeof(scope));
    if (ImGui::Button("Apply collection range")) {
        OpArgs args; args.Set("category", std::string(category)); args.Set("scope", std::string(scope));
        InvokeOperator(ctx, "profiler.performance.set_capture_filter", args);
    }
    ImGui::SameLine();
    if (ImGui::Button("Collect all")) {
        InvokeOperator(ctx, "profiler.performance.set_capture_filter");
        category[0] = '\0'; scope[0] = '\0'; observed = {};
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Applied before descriptor registration and timing, at the next frame boundary. Changes start a new capture session. Matching children remain visible under excluded parents.");
    ImGui::TextDisabled("Table search/category controls below only change the displayed rows.");
}
void DrawBenchmarkControls(EditorContext& ctx)
{
    if (!ImGui::CollapsingHeader("Same-condition benchmark")) return;
    static char label[257] = "capture";
    static char scenario[257] = {};
    static char camera[257] = {};
    static char configuration[1025] = {};
    static int warmupFrames = 120;
    static int sampleFrames = 600;
    const auto& capture = GetPerformanceBenchmarkCapture();
    const auto& status = capture.Status();
    ImGui::BeginDisabled(capture.IsActive());
    ImGui::InputText("Capture label", label, sizeof(label));
    ImGui::InputTextWithHint("Scenario", "Scene + workload/replay revision", scenario, sizeof(scenario));
    ImGui::InputTextWithHint("Camera", "Fixed camera or repeatable camera path", camera, sizeof(camera));
    ImGui::InputTextWithHint("Fixed configuration", "dt, seed, quality, pacing, build/hardware", configuration, sizeof(configuration));
    ImGui::InputInt("Warmup frames", &warmupFrames);
    ImGui::InputInt("Sample frames", &sampleFrames);
    if (ImGui::Button("Start benchmark")) {
        OpArgs args; args.Set("label", std::string(label)); args.Set("warmupFrames", warmupFrames); args.Set("sampleFrames", sampleFrames);
        args.Set("scenario", std::string(scenario)); args.Set("camera", std::string(camera)); args.Set("configuration", std::string(configuration));
        const auto result = InvokeOperator(ctx, "profiler.benchmark.start", args);
        if (!result.ok) ImGui::OpenPopup("Invalid benchmark settings");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Invalid benchmark settings")) {
        ImGui::TextWrapped("Provide a label, warmup 0..60000 and samples 1..60000.");
        ImGui::EndPopup();
    }
    ImGui::TextWrapped("Fix the workload and camera first. Conditions describe your setup; recording does not freeze simulation, set dt/seed or change frame pacing. Keep them identical when comparing.");
    if (capture.IsActive()) {
        ImGui::Text("%s: warmup %llu/%llu, samples %llu/%llu", capture.Label().c_str(),
            static_cast<unsigned long long>(status.warmedFrames), static_cast<unsigned long long>(status.warmupFrames),
            static_cast<unsigned long long>(status.capturedFrames), static_cast<unsigned long long>(status.sampleFrames));
        if (ImGui::Button("Cancel benchmark")) InvokeOperator(ctx, "profiler.benchmark.cancel");
    }
    if (status.state == PerformanceBenchmarkState::FAILED || status.state == PerformanceBenchmarkState::CANCELLED)
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "Capture interrupted. Restart the same conditions before comparing.");
    if (status.state != PerformanceBenchmarkState::COMPLETE) return;
    const auto report = capture.Report();
    ImGui::Text("%s: %llu frames; CPU p50 %.3f / p95 %.3f ms", report.label.c_str(),
        static_cast<unsigned long long>(status.capturedFrames), report.cpu.p50Ms, report.cpu.p95Ms);
    ImGui::TextWrapped("Captured scenario: %s / camera: %s / configuration: %s", report.conditions.scenario.c_str(),
        report.conditions.camera.c_str(), report.conditions.configuration.c_str());
    ImGui::Text("Incomplete %llu / dropped %llu / missing frames %llu",
        static_cast<unsigned long long>(status.incompleteFrames), static_cast<unsigned long long>(status.droppedSampleCount),
        static_cast<unsigned long long>(status.missingFrameCount));
    if (ImGui::Button("Use as comparison reference")) InvokeOperator(ctx, "profiler.benchmark.set_reference");
    ImGui::SameLine();
    if (ImGui::Button("Copy benchmark summary JSON")) {
        OpArgs args; args.Set("count", 1);
        const auto result = InvokeOperator(ctx, "profiler.benchmark.result", args);
        std::string json;
        if (result.ok && WriteOpDataJson(result.data, json)) ImGui::SetClipboardText(json.c_str());
    }
    const auto* reference = GetPerformanceBenchmarkReference();
    const auto comparison = ComparePerformanceBenchmarks(reference ? *reference : report, report);
    for (const auto& warning : comparison.warnings) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.25f, 1.0f));
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
    if (reference && comparison.comparable)
        ImGui::Text("vs %s: CPU p50 %+.3f / p95 %+.3f ms", reference->label.c_str(), comparison.cpuP50DeltaMs, comparison.cpuP95DeltaMs);
}
} /// @note namespace
void PerformanceProfilerPanel::OnLoadSettings(const EditorSettings& settings) { profiler::Profiler::RequestRecording(settings.performanceProfilerRecording); }
void PerformanceProfilerPanel::OnSaveSettings(EditorSettings& settings) const { settings.performanceProfilerRecording = profiler::Profiler::IsEnabled(); }
void PerformanceProfilerPanel::OnRenderContent(EditorContext& ctx)
{
    if (ImGui::BeginTabBar("PerformanceTabs")) {
        if (ImGui::BeginTabItem("CPU")) {
            DrawCollectionControls(ctx);
            DrawBenchmarkControls(ctx);
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
