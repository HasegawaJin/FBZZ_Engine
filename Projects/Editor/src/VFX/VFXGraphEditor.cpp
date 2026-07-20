// FBZZ Engine
// VFXGraphEditor.cpp | fbzz::editor
// Graph Canvas・Viewport・Inspector同時表示UIの実装
#include <Editor/VFX/VFXGraphEditor.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Panels/VFXEditorPanel.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <string>
#include <utility>

namespace fbzz::editor {

void VFXGraphEditor::Render(VFXEditorPanel& host, EditorContext& context)
{
    if (host.m_graphPath != context.selectedAssetPath) {
        // Graph切替時に破棄するのは専用Preview Worldだけで、ゲームSceneへは触れない。
        if (context.vfxPreviewScene != nullptr) context.vfxPreviewScene->Clear();
        host.m_graphPreviewEntity = scene::EntityID::INVALID;
        host.LoadGraph(context.selectedAssetPath);
    }

    // Inspector編集直後にも検証し、保存時まで破損理由が見えない状態を作らない。
    if (!host.m_graph.nodes.empty()) {
        std::string validationError;
        if (asset::ValidateVFXGraphAsset(host.m_graph, &validationError)) host.m_graphError.clear();
        else host.m_graphError = std::move(validationError);
    }

    host.DrawGraphMenuBar(context);
    host.DrawGraphToolbar(context);
    ImGui::Separator();
    if (!host.m_graphError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0.24f, 0.07f, 0.07f, 1.0f });
        ImGui::BeginChild("##VFXErrorBanner", { 0.0f, 42.0f }, true);
        ImGui::TextColored({ 1.0f, 0.55f, 0.48f, 1.0f }, "Validation failed");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", host.m_graphError.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (host.m_graph.nodes.empty()) return;
    }

    // WHAT: Graph、描画Viewport、Inspectorを独立した3列として常時表示する。
    // WHY: PreviewをInspector上部へ積む構成では縦幅と視認性を奪い合い、編集結果を見ながら配線できないため。
    const float workspaceHeight = (std::max)(ImGui::GetContentRegionAvail().y - 26.0f, 160.0f);
    if (ImGui::BeginTable("##VFXGraphWorkspace", 3,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_SizingStretchProp,
            { 0.0f, workspaceHeight })) {
        ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Viewport", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##VFXGraphCanvas", { 0.0f, 0.0f }, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        host.DrawGraphCanvas(context);
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##VFXGraphViewport", { 0.0f, 0.0f }, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        host.DrawPreviewViewport(context, true);
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("##VFXGraphInspector", { 0.0f, 0.0f }, false);
        host.DrawGraphInspector(context);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    host.DrawGraphStatusBar();
}

} // namespace fbzz::editor
