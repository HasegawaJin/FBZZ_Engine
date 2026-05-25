// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// Scene GameObject hierarchy and selection editing
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

enum class PrimitiveTemplate {
    Cube,
    Sphere,
    Plane,
    Cylinder,
    Cone,
    Torus,
    Capsule
};

std::shared_ptr<renderer::Material> CreateTemplateMaterial()
{
    auto* resources = renderer::ResourceManager::Active();
    if (!resources) return {};

    auto material = std::make_shared<renderer::Material>();
    material->shaderPath = "Assets/shaders/Material/Phong.hlsl";
    material->shader = resources->LoadShader(material->shaderPath);
    material->params.roughness = 0.65f;
    material->Init(*resources);
    return material;
}

std::shared_ptr<renderer::Mesh> CreatePrimitiveMesh(PrimitiveTemplate type)
{
    auto* resources = renderer::ResourceManager::Active();
    if (!resources) return {};

    switch (type) {
    case PrimitiveTemplate::Cube:     return renderer::PrimitiveMesh::Cube(*resources);
    case PrimitiveTemplate::Sphere:   return renderer::PrimitiveMesh::Sphere(*resources);
    case PrimitiveTemplate::Plane:    return renderer::PrimitiveMesh::Plane(*resources);
    case PrimitiveTemplate::Cylinder: return renderer::PrimitiveMesh::Cylinder(*resources);
    case PrimitiveTemplate::Cone:     return renderer::PrimitiveMesh::Cone(*resources);
    case PrimitiveTemplate::Torus:    return renderer::PrimitiveMesh::Torus(*resources);
    case PrimitiveTemplate::Capsule:  return renderer::PrimitiveMesh::Capsule(*resources);
    }
    return {};
}

const char* GetPrimitivePath(PrimitiveTemplate type)
{
    switch (type) {
    case PrimitiveTemplate::Cube:     return "primitive:cube";
    case PrimitiveTemplate::Sphere:   return "primitive:sphere";
    case PrimitiveTemplate::Plane:    return "primitive:plane";
    case PrimitiveTemplate::Cylinder: return "primitive:cylinder";
    case PrimitiveTemplate::Cone:     return "primitive:cone";
    case PrimitiveTemplate::Torus:    return "primitive:torus";
    case PrimitiveTemplate::Capsule:  return "primitive:capsule";
    }
    return "";
}

scene::ColliderComponent CreateTemplateCollider(PrimitiveTemplate type)
{
    scene::ColliderComponent collider;
    switch (type) {
    case PrimitiveTemplate::Sphere:
        collider.collider = std::make_shared<physics::SphereCollider>(0.5f);
        break;
    case PrimitiveTemplate::Capsule:
        collider.collider = std::make_shared<physics::CapsuleCollider>(0.25f, 0.25f);
        break;
    case PrimitiveTemplate::Plane:
        collider.collider = std::make_shared<physics::AABBCollider>(math::Vector3{ 0.5f, 0.01f, 0.5f });
        break;
    case PrimitiveTemplate::Cube:
    case PrimitiveTemplate::Cylinder:
    case PrimitiveTemplate::Cone:
    case PrimitiveTemplate::Torus:
        collider.collider = std::make_shared<physics::AABBCollider>(math::Vector3{ 0.5f, 0.5f, 0.5f });
        break;
    }
    return collider;
}

void CreatePrimitiveObject(EditorContext& ctx, const char* name, PrimitiveTemplate type)
{
    auto& go = ctx.activeScene->CreateGameObject(name);

    scene::MeshRenderer mr;
    mr.meshPath = GetPrimitivePath(type);
    mr.mesh = CreatePrimitiveMesh(type);
    go.AddComponent<scene::MeshRenderer>(mr);

    scene::MaterialComponent mc;
    mc.shaderPath = "Assets/shaders/Material/Phong.hlsl";
    mc.material = CreateTemplateMaterial();
    go.AddComponent<scene::MaterialComponent>(mc);

    go.AddComponent<scene::ColliderComponent>(CreateTemplateCollider(type));

    ctx.selectedEntities = { go.GetID() };
}

void CreateLightObject(EditorContext& ctx, const char* name, scene::LightComponent::Type type)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::LightComponent light;
    light.type = type;
    if (type == scene::LightComponent::Type::Point) {
        light.intensity = 4.0f;
        light.range = 8.0f;
    } else if (type == scene::LightComponent::Type::Spot) {
        light.intensity = 5.0f;
        light.range = 12.0f;
    }
    go.AddComponent<scene::LightComponent>(light);
    ctx.selectedEntities = { go.GetID() };
}

void CreateCameraObject(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Camera");
    go.transform.localPosition = { 0.0f, 2.0f, -5.0f };
    go.AddComponent<scene::CameraComponent>();
    ctx.selectedEntities = { go.GetID() };
}

void RemoveSelection(EditorContext& ctx, scene::EntityID id)
{
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove(selected.begin(), selected.end(), id), selected.end());
}

void PruneSelection(EditorContext& ctx)
{
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove_if(selected.begin(), selected.end(),
        [&ctx](scene::EntityID id) { return !ctx.activeScene->IsValid(id); }),
        selected.end());
}

void DestroySelected(EditorContext& ctx, const std::vector<scene::EntityID>& ids)
{
    for (scene::EntityID id : ids)
        ctx.activeScene->DestroyGameObject(id);
    PruneSelection(ctx);
}

void DrawCreateObjectMenu(EditorContext& ctx, std::function<void()>& deferred)
{
    if (ImGui::MenuItem("Empty")) {
        deferred = [&ctx]() {
            auto& newGo = ctx.activeScene->CreateGameObject("GameObject");
            ctx.selectedEntities = { newGo.GetID() };
        };
    }

    if (ImGui::BeginMenu("3D Object")) {
        if (ImGui::MenuItem("Cube"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cube", PrimitiveTemplate::Cube); };
        if (ImGui::MenuItem("Sphere"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Sphere", PrimitiveTemplate::Sphere); };
        if (ImGui::MenuItem("Plane"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Plane", PrimitiveTemplate::Plane); };
        if (ImGui::MenuItem("Cylinder"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cylinder", PrimitiveTemplate::Cylinder); };
        if (ImGui::MenuItem("Cone"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cone", PrimitiveTemplate::Cone); };
        if (ImGui::MenuItem("Torus"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Torus", PrimitiveTemplate::Torus); };
        if (ImGui::MenuItem("Capsule"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Capsule", PrimitiveTemplate::Capsule); };
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Light")) {
        if (ImGui::MenuItem("Directional Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Directional Light", scene::LightComponent::Type::Directional); };
        if (ImGui::MenuItem("Point Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Point Light", scene::LightComponent::Type::Point); };
        if (ImGui::MenuItem("Spot Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Spot Light", scene::LightComponent::Type::Spot); };
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Camera"))
        deferred = [&ctx]() { CreateCameraObject(ctx); };
}

bool ReadEntityPayload(const ImGuiPayload* payload, scene::EntityID& outId)
{
    if (!payload || payload->DataSize != sizeof(scene::EntityID)) return false;
    std::memcpy(&outId, payload->Data, sizeof(outId));
    return true;
}

bool ContainsEntity(const std::vector<scene::EntityID>& entities, scene::EntityID id)
{
    return std::find(entities.begin(), entities.end(), id) != entities.end();
}

// 子孫を再帰的に visited に追加するだけ（ImGui 呼び出しなし）。
// 親が閉じているとき子が第2ループで誤って root 描画されるのを防ぐ。
void MarkDescendantsVisited(scene::GameObject& go,
                            std::vector<scene::EntityID>& visited)
{
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (!child || ContainsEntity(visited, child->GetID())) continue;
        visited.push_back(child->GetID());
        MarkDescendantsVisited(*child, visited);
    }
}

void DrawHierarchyNode(EditorContext& ctx,
                       scene::GameObject& go,
                       size_t rootIndex,          // roots 配列内のインデックス（Order メニュー用, root のみ有効）
                       size_t rootCount,
                       std::vector<scene::EntityID>& visited,
                       std::function<void()>& deferred,
                       scene::EntityID* pendingExpand)
{
    const scene::EntityID id = go.GetID();
    if (ContainsEntity(visited, id)) return;
    visited.push_back(id);

    const bool hasChildren = go.GetChildCount() > 0;
    const bool selected    = ContainsEntity(ctx.selectedEntities, id);
    const bool isRoot      = go.GetParent() == nullptr;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected)     flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(id.index));

    // SetParent 後の次フレームで強制 open
    if (pendingExpand && *pendingExpand == id) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        *pendingExpand = scene::EntityID{};
    }

    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (!ImGui::GetIO().KeyCtrl)
            ctx.selectedEntities.clear();
        auto it = std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id);
        if (it != ctx.selectedEntities.end())
            ctx.selectedEntities.erase(it);
        else
            ctx.selectedEntities.push_back(id);
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("FBZZ_HIERARCHY_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(go.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId) &&
            draggedId != id) {
            deferred = [&ctx, draggedId, id, pendingExpand]() {
                auto* dragged = ctx.activeScene->GetGameObject(draggedId);
                auto* target  = ctx.activeScene->GetGameObject(id);
                if (dragged && target && dragged->SetParent(target)) {
                    ctx.selectedEntities = { draggedId };
                    if (pendingExpand) *pendingExpand = id;  // 次フレームで親を open
                }
            };
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        ctx.selectedEntities = { id };

        if (ImGui::BeginMenu("Create")) {
            DrawCreateObjectMenu(ctx, deferred);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate")) {
            std::string      srcName      = go.name;
            std::string      srcTag       = go.tag;
            int              srcLayer     = go.layer;
            scene::Transform srcTransform = go.transform;
            deferred = [&ctx, id, srcName, srcTag, srcLayer, srcTransform]() {
                auto& dst     = ctx.activeScene->CreateGameObject(srcName + " (Clone)");
                dst.tag       = srcTag;
                dst.layer     = srcLayer;
                dst.transform = srcTransform;
                ctx.activeScene->DuplicateComponents(id, dst.GetID());
                ctx.selectedEntities = { dst.GetID() };
            };
        }
        if (ImGui::BeginMenu("Hierarchy")) {
            const bool hasParent = go.GetParent() != nullptr;
            if (ImGui::MenuItem("Set As Root", nullptr, false, hasParent))
                deferred = [&ctx, id]() {
                    if (auto* target = ctx.activeScene->GetGameObject(id))
                        target->ClearParent();
                };
            ImGui::EndMenu();
        }
        if (isRoot && ImGui::BeginMenu("Order")) {
            const bool canMoveUp   = rootIndex > 0;
            const bool canMoveDown = rootIndex + 1 < rootCount;
            if (ImGui::MenuItem("Move Up", nullptr, false, canMoveUp))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObject(id, -1); };
            if (ImGui::MenuItem("Move Down", nullptr, false, canMoveDown))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObject(id, 1); };
            ImGui::Separator();
            if (ImGui::MenuItem("Move To Top", nullptr, false, canMoveUp))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObjectToIndex(id, 0); };
            if (ImGui::MenuItem("Move To Bottom", nullptr, false, canMoveDown))
                deferred = [&ctx, id]() {
                    ctx.activeScene->MoveGameObjectToIndex(id, ctx.activeScene->GameObjectCount() - 1);
                };
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            deferred = [&ctx, id]() {
                ctx.activeScene->DestroyGameObject(id);
                RemoveSelection(ctx, id);
                PruneSelection(ctx);
            };
        }
        ImGui::EndPopup();
    }

    if (hasChildren) {
        if (opened) {
            for (int i = 0; i < go.GetChildCount(); ++i) {
                if (auto* child = go.GetChild(i))
                    DrawHierarchyNode(ctx, *child, 0, rootCount, visited, deferred, pendingExpand);
            }
            ImGui::TreePop();
        } else {
            // 閉じていても子孫を visited に入れる。
            // これをしないと第2ループが子を root レベルで誤描画する。
            MarkDescendantsVisited(go, visited);
        }
    }

    ImGui::PopID();
}

} // namespace

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    // deferred: ノード描画ループ内でシーンを変更すると、その後のイテレーションで
    // ポインタが無効になる。変更操作 (生成/削除/親付け) はすべてラムダに包み、
    // ループ終了後にまとめて実行する。
    std::function<void()> deferred;
    std::vector<scene::EntityID> visited;
    visited.reserve(ctx.activeScene->GameObjectCount());

    // root オブジェクトだけを起点にツリーを描画する。
    // DrawHierarchyNode 内で子孫も全て visited に登録される（collapsed でも）。
    const auto   roots     = ctx.activeScene->GetRootGameObjects();
    const size_t rootCount = roots.size();
    for (size_t i = 0; i < roots.size(); ++i)
        if (roots[i]) DrawHierarchyNode(ctx, *roots[i], i, rootCount, visited, deferred, &m_pendingExpand);

    // 親がいないのに GetRootGameObjects に含まれなかった孤立オブジェクトを救済する。
    // 正常なシーンでは実行されない。
    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!ContainsEntity(visited, go.GetID()) && go.GetParent() == nullptr)
            DrawHierarchyNode(ctx, go, 0, 0, visited, deferred, &m_pendingExpand);
    }

    if (ImGui::BeginDragDropTarget()) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            deferred = [&ctx, draggedId]() {
                if (auto* dragged = ctx.activeScene->GetGameObject(draggedId)) {
                    dragged->ClearParent();
                    ctx.selectedEntities = { draggedId };
                }
            };
        }
        ImGui::EndDragDropTarget();
    }

    if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) && !ctx.selectedEntities.empty()) {
        std::vector<scene::EntityID> ids = ctx.selectedEntities;
        deferred = [&ctx, ids]() { DestroySelected(ctx, ids); };
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
        ctx.selectedEntities.clear();

    if (ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateObjectMenu(ctx, deferred);
        ImGui::EndPopup();
    }

    if (deferred) deferred();
}

} // namespace fbzz::editor
