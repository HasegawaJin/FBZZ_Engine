// FBZZ Engine
// VFXPreview.cpp | fbzz::editor
// VFX Graph の構造プレビューと専用 VFX Editor への接続。
// WHY: 実時間 VFX の Preview World は FBZZVFXEditor.exe が所有するため、
//      メイン Editor では構造と時間を確認し、実描画だけを専用プロセスへ委譲する。
#include <Editor/Panels/VFXPreview.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/VFXEditorLauncher.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::editor {

bool DrawVFXPreviewWidget(EditorContext& ctx,
                          std::string_view assetPath,
                          float previewHeight)
{
    // VFX の実時間レンダーは専用プロセスが所有するため、ここでは同じ .vfx を読み込んで
    // 構造・時間・予算を先に確認できるようにする。これにより、Preview Panel が別の
    // Preview World を生成して Editor Scene へ混入させることを防ぐ。
    const std::string path(assetPath);
    static std::string cachedPath;
    static std::filesystem::file_time_type cachedWriteTime{};
    static asset::VFXGraphAsset cachedGraph;
    static std::string cachedError;
    static bool cachedLoaded = false;
    std::error_code writeTimeError;
    const auto writeTime = std::filesystem::last_write_time(
        asset::AssetManager::ResolveAssetPath(path), writeTimeError);
    if (cachedPath != path ||
        (!writeTimeError && writeTime != cachedWriteTime)) {
        cachedPath = path;
        cachedWriteTime = writeTimeError ? std::filesystem::file_time_type{} : writeTime;
        cachedGraph = {};
        cachedError.clear();
        cachedLoaded = asset::ParseVFXGraphAsset(path, cachedGraph, &cachedError);
    }
    if (!cachedLoaded) {
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Failed to load .vfx");
        if (!cachedError.empty()) ImGui::TextWrapped("%s", cachedError.c_str());
        return false;
    }
    const asset::VFXGraphAsset& graph = cachedGraph;

    std::vector<float> startTimes;
    float duration = 0.0f;
    const bool hasSchedule = asset::BuildVFXGraphSchedule(
        graph, startTimes, duration, nullptr);
    const auto budget = asset::CalculateVFXGraphBudget(graph);

    ImGui::TextUnformatted(graph.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("v%d", graph.version);
    ImGui::LabelText("Nodes", "%zu", graph.nodes.size());
    ImGui::LabelText("Duration", hasSchedule ? "%.3f s" : "(invalid graph)", duration);
    ImGui::LabelText("Budget", "%d particles / %d lights / %d audio",
                     budget.particles, budget.lights, budget.audioVoices);

    const float timelineHeight = (std::max)(previewHeight - 106.0f, 70.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size((std::max)(ImGui::GetContentRegionAvail().x, 64.0f), timelineHeight);
    ImGui::InvisibleButton("##VFXPreviewSummary", size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    drawList->AddRectFilled(origin, max, IM_COL32(20, 22, 28, 255), 5.0f);
    drawList->AddRect(origin, max, IM_COL32(90, 96, 108, 255), 5.0f);

    // ノードの開始時刻を横方向へ並べ、グラフの時間的な密度を一目で確認する。
    // ノードの種類は色ではなくラベルでも表示し、色覚差があっても判別できるようにする。
    const float contentWidth = (std::max)(size.x - 20.0f, 1.0f);
    const float contentHeight = (std::max)(size.y - 30.0f, 1.0f);
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const auto& node = graph.nodes[i];
        const float normalized = hasSchedule && duration > 0.0f && i < startTimes.size()
            ? std::clamp(startTimes[i] / duration, 0.0f, 1.0f) : 0.0f;
        const float x = origin.x + 10.0f + normalized * contentWidth;
        const float y = origin.y + 15.0f +
                        static_cast<float>(i % 5) * (contentHeight / 5.0f);
        const ImVec2 nodeMin(x - 4.0f, y - 4.0f);
        const ImVec2 nodeMax(x + 4.0f, y + 4.0f);
        drawList->AddRectFilled(nodeMin, nodeMax, IM_COL32(245, 130, 82, 255), 2.0f);
        if (i < 12) {
            const char* typeName = asset::VFXNodeTypeName(node.type);
            drawList->AddText(ImVec2(x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f),
                              IM_COL32(190, 196, 208, 255), typeName);
        }
    }
    drawList->AddText(ImVec2(origin.x + 10.0f, max.y - ImGui::GetTextLineHeight() - 7.0f),
                      IM_COL32(130, 138, 150, 255), "Graph timeline");

    if (ImGui::Button("Open VFX Editor"))
        VFXEditorLauncher::Launch(ctx.projectRoot, path);
    ImGui::SameLine();
    ImGui::TextDisabled("Live particle preview is hosted by FBZZVFXEditor.exe");
    return true;
}


} // namespace fbzz::editor
