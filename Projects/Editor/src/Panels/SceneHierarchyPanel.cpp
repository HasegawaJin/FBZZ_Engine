// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// シーン内 GameObject をリスト表示し選択状態を EditorContext に書き込む
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>

namespace fbzz::editor {

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    for (auto& go : ctx.activeScene->GameObjects()) {
        scene::EntityID id = go.GetID();

        bool selected = std::find(ctx.selectedEntities.begin(),
                                  ctx.selectedEntities.end(), id)
                      != ctx.selectedEntities.end();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::TreeNodeEx(go.name.c_str(), flags);

        if (ImGui::IsItemClicked()) {
            if (!ImGui::GetIO().KeyCtrl)
                ctx.selectedEntities.clear();
            // 既に選択済みの場合は Ctrl+Click で解除
            auto it = std::find(ctx.selectedEntities.begin(),
                                ctx.selectedEntities.end(), id);
            if (it != ctx.selectedEntities.end())
                ctx.selectedEntities.erase(it);
            else
                ctx.selectedEntities.push_back(id);
        }
    }

    // 空白部分クリックで選択解除
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
        ctx.selectedEntities.clear();
}

} // namespace fbzz::editor
