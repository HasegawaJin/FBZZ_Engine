// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// シーン内 GameObject をリスト表示し選択状態を EditorContext に書き込む
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <functional>

namespace fbzz::editor {

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    // Create / Duplicate は GameObjects() のイテレータを無効化するため、
    // ループ後に実行するよう遅延させる
    std::function<void()> deferred;

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

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (!ImGui::GetIO().KeyCtrl)
                ctx.selectedEntities.clear();
            auto it = std::find(ctx.selectedEntities.begin(),
                                ctx.selectedEntities.end(), id);
            if (it != ctx.selectedEntities.end())
                ctx.selectedEntities.erase(it);
            else
                ctx.selectedEntities.push_back(id);
        }

        // ノード右クリックメニュー
        if (ImGui::BeginPopupContextItem()) {
            ctx.selectedEntities = { id };

            if (ImGui::MenuItem("Duplicate")) {
                std::string srcName = go.name;
                std::string srcTag  = go.tag;
                int         srcLayer= go.layer;
                scene::Transform srcTransform = go.transform;
                deferred = [&ctx, id, srcName, srcTag, srcLayer, srcTransform]() {
                    auto& dst      = ctx.activeScene->CreateGameObject(srcName + " (Clone)");
                    dst.tag        = srcTag;
                    dst.layer      = srcLayer;
                    dst.transform  = srcTransform;
                    ctx.activeScene->DuplicateComponents(id, dst.GetID());
                    ctx.selectedEntities = { dst.GetID() };
                };
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                scene::GameObject::Destroy(go);
                ctx.selectedEntities.clear();
            }
            ImGui::EndPopup();
        }
    }

    // 空白部分クリックで選択解除
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
        ctx.selectedEntities.clear();

    // 空白部分右クリック — GameObject 生成
    if (ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Empty")) {
            deferred = [&ctx]() {
                auto& newGo = ctx.activeScene->CreateGameObject("GameObject");
                ctx.selectedEntities = { newGo.GetID() };
            };
        }
        ImGui::EndPopup();
    }

    // ループ後に一度だけ実行（Create / Duplicate はイテレータを無効化するため遅延）
    if (deferred) deferred();
}

} // namespace fbzz::editor
