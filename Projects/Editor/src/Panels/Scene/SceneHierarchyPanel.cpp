/// @file    SceneHierarchyPanel.cpp
/// @brief   シーン内のオブジェクトの階層表示と選択操作。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "../../UI/PanelToolbar.hpp"
#include "../../UI/ContentGlyph.hpp"
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Scene/Components/Physics/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VFX/VFXComponent.hpp>
#include <Engine/Scene/Components/Audio/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Water/WaterfallComponent.hpp>
#include <Engine/Asset/Model/Import/ModelSource.hpp>
#include <Editor/Panels/Scene/SceneHierarchyPanel.hpp>
#include "../../Material/MaterialAuthoringEditing.hpp"
#include <Editor/Asset/Browser/AssetDragPayload.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Play/PlayModeController.hpp>
#include <Editor/Asset/Storage/AssetPath.hpp>
#include <Editor/Inspector/Authoring/ColliderFit.hpp>
#include <Editor/Op/CreateObjectMenu.hpp>
#include <Editor/UI/Widgets/DragDropSet.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <Editor/Asset/Authoring/ModelPlacement.hpp>
#include <Editor/Prefab/PrefabSerializer.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Scene/Editing/SceneEditUtils.hpp>
#include <Editor/Scene/Persistence/SceneIO.hpp>
#include <Editor/Scene/Editing/Selection.hpp>
#include <Editor/Viewport/SelectionVisuals.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Editor/UI/Widgets/ImGuiWidgets.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Camera/CameraComponent.hpp>
#include <Engine/Scene/Components/Physics/ColliderComponent.hpp>
#include <Engine/Scene/Components/Lighting/LightComponent.hpp>
#include <Engine/Scene/Components/Rendering/DecalComponent.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <Engine/Scene/Components/VFX/VFXComponent.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Engine/Scene/Components/Terrain/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Components/UI/UICanvas.hpp>
#include <Engine/Scene/Components/UI/UIImage.hpp>
#include <Engine/Scene/Components/UI/UIText.hpp>
#include <Engine/Scene/Components/UI/UIButton.hpp>
#include <Engine/Scene/Components/UI/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UI/UIAnimator.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Scripting/ScriptFactory.hpp>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <Graphics/Renderer/Shaders/IShader.hpp>
#include <Graphics/Renderer/Geometry/PrimitiveMesh.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <cstring>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Util/Text/StringUtils.hpp>
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/Shapes/OBBCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

void HierarchyTraversal::BeginFrame(scene::Scene& scene, bool showGeneratedObjects)
{
    visited.clear();
    const uint64_t generation = scene.GetRenderSceneGeneration();
    const uint64_t revision = scene.GetHierarchyRevision();
    if (m_sceneGeneration != generation || m_revision != revision) {
        m_sceneGeneration = generation;
        m_revision = revision;
        roots.clear();
        recoveredRoots.clear();
        for (const auto* root : scene.GetRootGameObjects()) {
            roots.push_back(root->GetID());
            Visit(root->GetID());
        }
        for (const auto& object : scene.GameObjects())
            if (!object.GetParent() && Visit(object.GetID())) {
                recoveredRoots.push_back(object.GetID());
            }
        visited.clear();
        hiddenCounts.clear();
        visibleChildren.clear();
    }
    if (m_showGeneratedObjects != showGeneratedObjects) {
        m_showGeneratedObjects = showGeneratedObjects;
        visibleChildren.clear();
    }
}

namespace {

struct DeferredHierarchyEdit {
    void operator=(std::function<void()> edit)
    {
        action = std::move(edit);
        snapshot = true;
    }

    void SetLocal(std::function<void()> edit)
    {
        action = std::move(edit);
        snapshot = false;
    }

    void Run(EditorContext& ctx) const
    {
        if (!action) {
            return;
        }
        if (snapshot) {
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", action);
        } else {
            action();
        }
    }

    std::function<void()> action;
    bool snapshot = false;
};

void ToggleEditorHiding(EditorContext& ctx, scene::EntityID id)
{
    if (ctx.editScene && ctx.activeScene != ctx.editScene) {
        return;
    }
    auto* object = ctx.activeScene->GetGameObject(id);
    if (!object) {
        return;
    }
    const std::string guid = object->instanceId;
    const bool hide = !ctx.editorHiddenGuids.contains(guid);
    auto apply = [context = &ctx, scene = ctx.activeScene, guid, id](bool hidden) {
        if (context->activeScene != scene) {
            return;
        }
        auto* target = scene->GetGameObject(id);
        if (!target || target->instanceId != guid) {
            target = scene->FindByGuid(guid);
        }
        if (!target) {
            return;
        }
        const bool active = GetAuthoringActive(*context, *target);
        if (hidden) {
            context->editorHiddenGuids.insert_or_assign(guid, active);
        } else {
            context->editorHiddenGuids.erase(guid);
        }
        target->SetActive(!hidden && active);
        if (context->markSceneDirty) {
            context->markSceneDirty();
        }
    };
    if (ctx.undoStack)
        ctx.undoStack->Execute(std::make_unique<LambdaCommand>("Toggle Editor Visibility",
            [apply, hide] { apply(hide); }, [apply, hide] { apply(!hide); }));
    else {
        apply(hide);
    }
}

void SetParentWithUndo(EditorContext& ctx,
                       scene::EntityID childId,
                       scene::EntityID newParentId,
                       const char* description)
{
    if (!ctx.activeScene) {
        return;
    }

    scene::GameObject* child = ctx.activeScene->GetGameObject(childId);
    if (!child) {
        return;
    }

    const scene::GameObject* oldParent = child->GetParent();
    const std::string childInstanceId = child->instanceId;
    const std::string oldParentInstanceId = oldParent ? oldParent->instanceId : std::string{};
    scene::GameObject* newParent = newParentId.IsValid()
        ? ctx.activeScene->GetGameObject(newParentId)
        : nullptr;
    const std::string newParentInstanceId = newParent ? newParent->instanceId : std::string{};
    const bool changed = newParent ? child->SetParent(newParent) : child->ClearParent();
    if (!changed) {
        return;
    }

    scene::Scene* scene = ctx.activeScene;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, childInstanceId, markDirty](const std::string& parentInstanceId) {
        if (auto* target = scene->FindByGuid(childInstanceId)) {
            if (!parentInstanceId.empty()) {
                if (auto* parent = scene->FindByGuid(parentInstanceId)) {
                    target->SetParent(parent);
                }
            } else {
                target->ClearParent();
            }
            if (markDirty) {
                markDirty();
            }
        }
    };

    if (ctx.undoStack && ctx.undoStack->IsRecordingEnabled()) {
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            description,
            [apply, newParentInstanceId]() { apply(newParentInstanceId); },
            [apply, oldParentInstanceId]() { apply(oldParentInstanceId); }));
    }
    if (ctx.markSceneDirty) {
        ctx.markSceneDirty();
    }
}

bool ReadEntityPayload(const ImGuiPayload* payload, scene::EntityID& outId)
{
    if (!payload || payload->DataSize != sizeof(scene::EntityID)) {
        return false;
    }
    std::memcpy(&outId, payload->Data, sizeof(outId));
    return true;
}

void SetHierarchyMembers(HierarchyTraversal::EntitySet& members, std::span<const scene::EntityID> ids)
{
    members.clear();
    members.reserve(ids.size());
    for (const auto id : ids) {
        members.insert(HierarchyTraversal::EntityKey(id));
    }
}

/// @brief ドラッグで運ばれている GameObject のうち、祖先が同じ集合に居ないものを行の並び順で返す。
/// @param[in] order 行の並び (前フレームの表示順)。載っていないものは末尾へ回す。
/// @note 祖先ごと動くものまで個別に付け替えると、選択内の親子関係が平らに崩れる。
std::vector<scene::EntityID> DraggedRoots(scene::Scene& scene, scene::EntityID payloadId,
                                          const std::vector<scene::EntityID>& order)
{
    const auto roots = CollectHierarchyRoots(scene, dragdrop::DraggedEntities(payloadId));
    if (roots.size() < 2) {
        return roots;
    }
    HierarchyTraversal::EntitySet remaining;
    SetHierarchyMembers(remaining, roots);
    std::vector<scene::EntityID> sorted;
    sorted.reserve(roots.size());
    for (const auto id : order) {
        if (remaining.erase(HierarchyTraversal::EntityKey(id))) {
            sorted.push_back(id);
        }
        if (remaining.empty()) {
            return sorted;
        }
    }
    for (const auto id : roots)
        if (remaining.erase(HierarchyTraversal::EntityKey(id))) {
            sorted.push_back(id);
        }
    return sorted;
}

/// @brief 掴んだ 1 件を先頭 (主選択) にして選択を置き換える。
void SelectMoved(EditorContext& ctx, std::vector<scene::EntityID> moved, scene::EntityID grabbed)
{
    if (moved.empty()) {
        return;
    }
    if (const auto it = std::find(moved.begin(), moved.end(), grabbed); it != moved.end()) {
        std::rotate(moved.begin(), it, it + 1);
    }
    SelectEntities(ctx, std::move(moved), SelectionReveal::Skip);
}



/// @note .vfx はプレハブと同じ中身だが、AssetBrowser の色分け・サムネイル・VFXRef のドロップ受理・
/// @note 開いたときの振る舞いがすべて拡張子で分岐する。同じ保存経路を通しつつ «これは演出だ»
/// @note という区別だけ呼び出し側が決められるよう拡張子と置き場所を引数にする。
std::string UniqueAssetPath(const EditorContext& ctx, const std::string& objectName,
                            const char* subDirectory, const char* extension)
{
    const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    const std::string directory = assetRoot + "/" + subDirectory;
    util::FileSystem::EnsureDirectory(directory);

    const std::string base = directory + "/" + SanitizePrefabFileName(objectName);
    std::string path = base + extension;
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i) {
        path = base + " " + std::to_string(i) + extension;
    }
    return path;
}

void SaveSelectedAsAsset(EditorContext& ctx, const std::string& objectName,
                         const char* subDirectory, const char* extension)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) {
        return;
    }
    scene::Scene& scene = *ctx.activeScene;

    // 保存 + インスタンス接続をまとめて実行する (Unity 互換の Create Prefab)。
    // 接続したルート GO は rootSelection に返り、Undo で接続解除に使う。
    const std::string path = UniqueAssetPath(ctx, objectName, subDirectory, extension);
    std::vector<scene::EntityID> rootSelection;
    if (!PrefabSerializer::SaveSelectionAndConnect(scene, ctx.selectedEntities, path, rootSelection)) {
        return;
    }
    const std::string relPath = scene::CanonicalPrefabAssetRef(path);

    // Undo 対象はシーン側のリンク (prefabAssetPath) だけ。.prefab ファイルは残す。
    // ファイル削除まで Undo すると、無関係な作業のあと Ctrl+Z を重ねたときに編集ぶんごと
    // ファイルが消える。消したいときは Asset Browser の Delete でごみ箱へ送る。
    if (ctx.undoStack) {
        EditorContext* context = &ctx;
        const std::vector<scene::EntityID> roots = rootSelection;
        auto applyLink = [context, roots](const std::string& assetPath) {
            if (!context->activeScene) {
                return;
            }
            for (scene::EntityID id : roots)
                if (auto* go = context->activeScene->GetGameObject(id)) {
                    go->prefabAssetPath = assetPath;
                }
            context->requestAssetBrowserRefresh = true;
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Link Prefab Instance",
            [applyLink, relPath]() { applyLink(relPath); },
            [applyLink]()          { applyLink({}); }));
    }
    if (ctx.markSceneDirty) {
        ctx.markSceneDirty();
    }
    ctx.requestAssetBrowserRefresh = true;
}



/// @brief アセットのドロップを、遅延編集と生成予約へ振り分ける。複数アセットはまとめて置く。
/// @param[in] parentId 無効ならルートへ置く。.mat はドロップ先の GameObject が要るので有効なときだけ受ける。
/// @param[out] pendingExpand 置いた後に開くノードの書き先。null 可。
void QueueAssetDrop(EditorContext& ctx, const std::string& payloadPath,
                    scene::EntityID parentId, const std::string& parentGuid,
                    DeferredHierarchyEdit& deferred, PendingObjectCreate& pendingCreate,
                    scene::EntityID* pendingExpand)
{
    std::vector<std::string> fbxPaths;
    std::vector<OpArgs>      instantiate;
    std::string              materialPath;
    for (const std::string& path : dragdrop::DraggedAssetPaths(payloadPath)) {
        const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
        if (asset::IsModelSourcePath(ext)) {
            fbxPaths.push_back(path);
        } else if (IsInstantiableAssetExtension(ext)) {
            // ノードへのドロップはその子、背景へのドロップはルート。メニューの Create Child / Create at Root と同じ規則。
            OpArgs args;
            args.Set("path", path);
            if (parentId.IsValid()) {
                args.Set("parent", parentGuid);
            } else {
                args.Set("placeInView", true);
            }
            instantiate.push_back(std::move(args));
        } else if (ext == ".mat" && parentId.IsValid() && materialPath.empty()) {
            // 1 つの GameObject に割り当てられるのは 1 枚なので、複数なら先頭だけ。
            materialPath = path;
        }
    }

    if (!instantiate.empty()) {
        pendingCreate.operatorId  = "prefab.instantiate";
        pendingCreate.beginRename = false;
        pendingCreate.args        = std::move(instantiate.front());
        pendingCreate.moreArgs.assign(std::make_move_iterator(instantiate.begin() + 1),
                                      std::make_move_iterator(instantiate.end()));
    }
    if (fbxPaths.empty() && materialPath.empty()) {
        return;
    }

    deferred = [&ctx, fbxPaths, materialPath, parentId, pendingExpand]() {
        std::vector<scene::EntityID> placed;
        // モデル原本は未インポートでもその場で自動インポートして配置する。
        for (const std::string& fbxPath : fbxPaths) {
            const std::string modelPath = ResolveOrImportFbxModel(fbxPath);
            if (modelPath.empty()) {
                continue;
            }
            const scene::EntityID rootId = SpawnModelAssetHierarchy(ctx, modelPath, nullptr, parentId);
            if (rootId != scene::EntityID::INVALID) {
                placed.push_back(rootId);
            }
        }
        if (!materialPath.empty()) {
            if (auto* target = ctx.activeScene->GetGameObject(parentId)) {
                // Viewport へのドロップと同じく、ドロップ先の GameObject へ直接割り当てる。
                auto* mc = target->GetComponent<scene::MaterialComponent>();
                if (!mc) {
                    mc = &target->AddComponent<scene::MaterialComponent>();
                }
                (void)material_edit_detail::AssignSlotReference(material_edit_detail::SlotTarget(ctx, *target, 0), materialPath);
                // 空にすると次フレームの SyncMaterial が新パスを解決し直す。

                if (placed.empty()) {
                    placed.push_back(parentId);
                }
            }
        }
        if (placed.empty()) {
            return;
        }
        SelectEntities(ctx, std::move(placed), SelectionReveal::Skip);
        if (pendingExpand && parentId.IsValid() && !fbxPaths.empty()) {
            *pendingExpand = parentId;
        }
        if (ctx.markSceneDirty) {
            ctx.markSceneDirty();
        }
    };
}

/// @note ランタイム生成 GO を Hierarchy から隠すか。ctx.showGeneratedObjects が真なら全部見せる。
/// @note 隠す判断を 1 箇所に集約する。ツリー・検索リスト・展開矢印が別々の条件を持つと
/// @note 「矢印は出るが開いても空」のような不整合が必ず起きる。
bool IsHiddenGenerated(const EditorContext& ctx, const scene::GameObject& go)
{
    return go.IsRuntimeGenerated() && !ctx.showGeneratedObjects;
}

/// @note 実際に描画される子がいるか。生成物を隠しているときは、それを除いて判定する。
/// @note GetChildCount() をそのまま使うと、子が生成物だけのノードで「開けるのに何も出ない」状態になる。
bool HasVisibleChild(const EditorContext& ctx, const scene::GameObject& go, HierarchyTraversal& traversal)
{
    if (go.GetChildCount() == 0) {
        return false;
    }
    const auto [entry, inserted] = traversal.visibleChildren.try_emplace(&go, false);
    if (!inserted) {
        return entry->second;
    }
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (child && !IsHiddenGenerated(ctx, *child)) {
            entry->second = true;
            break;
        }
    }
    return entry->second;
}

/// @note 隠している生成物の総数 (子孫すべて)。行末のバッジに出し、
/// @note 「Hierarchy に出ていない = 存在しない」と誤解させないための表示。
int HiddenGeneratedCount(const EditorContext& ctx, const scene::GameObject& go, HierarchyTraversal& traversal)
{
    if (ctx.showGeneratedObjects || go.GetChildCount() == 0) {
        return 0;
    }
    if (const auto found = traversal.hiddenCounts.find(&go); found != traversal.hiddenCounts.end()) {
        return found->second;
    }
    int count = 0;
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (!child) {
            continue;
        }
        if (IsHiddenGenerated(ctx, *child)) {
            ++count;
            // 生成物の下はまとめて 1 段だけ数える
            count += child->GetChildCount();
        } else {
            count += HiddenGeneratedCount(ctx, *child, traversal);
        }
    }
    traversal.hiddenCounts.emplace(&go, count);
    return count;
}

/// @brief F2 インラインリネームの状態一式 (パネルメンバーへの参照)。
struct InlineRenameState {
    scene::EntityID& id;        /// @note リネーム対象 (無効 = リネーム中でない)
    char*            buffer;
    size_t           bufferSize;
    bool&            focusPending;
};

/// @brief ツリーを「開いて見せる」ための状態一式 (パネルメンバーへの参照)。
struct HierarchyRevealState {
    scene::EntityID&              pendingExpand; /// @note SetParent 直後に開くノード
    scene::EntityID&              scrollTo;      /// @note 行までスクロールする対象
    std::unordered_map<std::string, bool>& expansionRequests;
};

bool HierarchyMenuItem(EditorContext& ctx, const char* label, const char* operatorId, const OpArgs& args = {})
{
    const auto* hotkey = ctx.hotkeyManager ? ctx.hotkeyManager->FindByOperator(operatorId) : nullptr;
    const auto binding = hotkey ? HotkeyManager::FormatBinding(*hotkey) : std::string{};
    return ImGui::MenuItem(LOC(label), binding.empty() ? nullptr : binding.c_str(), false,
                          CanInvokeOperator(ctx, operatorId, args));
}

void DrawHierarchyOperation(EditorContext& ctx, DeferredHierarchyEdit& deferred, const char* label,
                            const char* operatorId, OpArgs args = {})
{
    if (HierarchyMenuItem(ctx, label, operatorId, args)) {
        deferred.SetLocal([&ctx, operatorId, args = std::move(args)] {
            InvokeOperator(ctx, operatorId, args);
        });
    }
}

void DrawHierarchyStateMenu(EditorContext& ctx, DeferredHierarchyEdit& deferred,
                            const scene::GameObject& object, bool multi, bool canEdit)
{
    if (!ImGui::BeginMenu(LOC("Object State"), canEdit || !multi)) {
        return;
    }

    bool anyEnabled = false;
    bool anyDisabled = false;
    for (const auto id : ctx.selectedEntities) {
        if (const auto* selected = ctx.activeScene->GetGameObject(id)) {
            if (GetAuthoringActive(ctx, *selected)) {
                anyEnabled = true;
            } else {
                anyDisabled = true;
            }
        }
    }
    const auto setActive = [&](const char* label, bool active) {
        if (ImGui::MenuItem(LOC(label), nullptr, false, canEdit)) {
            deferred.SetLocal([&ctx, targets = ctx.selectedEntities, label, active] {
                SetGameObjectsActive(ctx, targets, active, label);
            });
        }
    };
    if (anyDisabled) {
        setActive(multi ? "Enable Selection" : "Enable Object", true);
    }
    if (anyEnabled) {
        setActive(multi ? "Disable Selection" : "Disable Object", false);
    }

    if (!multi) {
        const auto id = object.GetID();
        ImGui::Separator();
        const bool hidden = ctx.editorHiddenGuids.contains(object.instanceId);
        if (ImGui::MenuItem(LOC(hidden ? "Show in Editor" : "Hide in Editor"), nullptr, false, canEdit)) {
            deferred.SetLocal([&ctx, id] { ToggleEditorHiding(ctx, id); });
        }
        if (ImGui::MenuItem(LOC(ctx.IsLocked(id) ? "Unlock Object" : "Lock Object"))) {
            ctx.ToggleLock(id);
        }
    }
    ImGui::EndMenu();
}

void DrawHierarchyPrefabMenu(EditorContext& ctx, DeferredHierarchyEdit& deferred,
                             const scene::GameObject& object, bool multi, bool canEdit)
{
    if (!ImGui::BeginMenu(LOC("Prefab"), canEdit)) {
        return;
    }

    if (ImGui::MenuItem(LOC(multi ? "Save Selection as Prefab" : "Save As Prefab"))) {
        deferred.SetLocal([&ctx, name = object.name] { SaveSelectedAsAsset(ctx, name, "Prefabs", ".prefab"); });
    }
    if (!multi && ctx.activeScene->HasComponent<scene::VFXComponent>(object.GetID())
        && ImGui::MenuItem(LOC("Save As VFX"))) {
        deferred.SetLocal([&ctx, name = object.name] { SaveSelectedAsAsset(ctx, name, "VFX", ".vfx"); });
    }
    if (!multi && !object.prefabAssetPath.empty()) {
        const auto id = object.GetID();
        ImGui::Separator();
        OpArgs args;
        args.Set("path", object.prefabAssetPath);
        DrawHierarchyOperation(ctx, deferred, "Open Prefab", "prefab.edit", std::move(args));
        if (ImGui::MenuItem(LOC("Apply to Prefab"))) {
            deferred = [&ctx, id] {
                const int updated = PrefabSerializer::ApplyAndPropagate(*ctx.activeScene, id, ctx.projectRoot);
                if (updated < 0) {
                    return;
                }
                if (updated > 0) {
                    ClearEntitySelection(ctx);
                }
                ctx.requestAssetBrowserRefresh = true;
            };
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", LOCT("Save this instance to the source prefab and update other instances."));
        }
        if (ImGui::MenuItem(LOC("Revert from Prefab"))) {
            deferred.SetLocal([&ctx, id] {
                ExecuteSceneEditWithUndo(ctx, "Revert Prefab", [&ctx, id] {
                    std::vector<scene::EntityID> roots;
                    if (PrefabSerializer::Revert(*ctx.activeScene, id, roots, ctx.projectRoot) && !roots.empty()) {
                        SelectEntities(ctx, std::move(roots), SelectionReveal::Skip);
                    }
                });
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", LOCT("Discard this instance's changes and restore the source prefab."));
        }
    }
    ImGui::EndMenu();
}

void DrawHierarchyNodeMenu(EditorContext& ctx, const scene::GameObject& object, bool hovered,
                           HierarchyTraversal& visited, DeferredHierarchyEdit& deferred,
                           PendingObjectCreate& pendingCreate)
{
    constexpr const char* POPUP_ID = "##node_context";
    const auto id = object.GetID();
    const auto key = HierarchyTraversal::EntityKey(id);
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (!visited.selected.contains(key)) {
            SelectEntity(ctx, id, SelectionReveal::Skip);
            SetHierarchyMembers(visited.selected, ctx.selectedEntities);
        }
        ImGui::OpenPopup(POPUP_ID);
    }
    if (!ImGui::BeginPopup(POPUP_ID)) {
        return;
    }
    if (!visited.selected.contains(key)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool multi = ctx.selectedEntities.size() > 1;
    const bool canEdit = !ctx.playMode || ctx.playMode->IsInEditor();
    if (multi) {
        ImGui::TextDisabled(LOCT("%zu objects selected"), ctx.selectedEntities.size());
    } else {
        widgets::LabelEllipsis(object.name.c_str(), ImGui::GetFontSize() * 18.0f);
    }
    ImGui::Separator();

    const bool canCreate = CanInvokeOperator(ctx, "node.create_empty");
    if (!multi && ImGui::BeginMenu(LOC("Create Child"), canCreate)) {
        DrawCreateObjectMenuItems(ctx, object.instanceId, pendingCreate);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(LOC("Create at Root"), canCreate)) {
        DrawCreateObjectMenuItems(ctx, {}, pendingCreate);
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (!multi && HierarchyMenuItem(ctx, "Rename", "edit.rename")) {
        InvokeOperator(ctx, "edit.rename");
    }
    DrawHierarchyOperation(ctx, deferred, "Frame Selected", "view.frame_selected");
    DrawHierarchyOperation(ctx, deferred, "Copy", "edit.copy");
    DrawHierarchyOperation(ctx, deferred, "Paste at Root", "edit.paste");
    if (!multi) {
        DrawHierarchyOperation(ctx, deferred, "Paste As Child", "edit.paste_as_child");
    }
    DrawHierarchyOperation(ctx, deferred, "Duplicate", "edit.duplicate");
    ImGui::Separator();

    DrawHierarchyStateMenu(ctx, deferred, object, multi, canEdit);
    if (multi) {
        DrawHierarchyOperation(ctx, deferred, "Group Selection", "node.group_selection");
    } else if (ImGui::BeginMenu(LOC("Hierarchy"), canEdit &&
                              (object.GetParent() || ctx.activeScene->GetRootGameObjects().size() > 1))) {
        if (object.GetParent() && ImGui::MenuItem(LOC("Set As Root"))) {
            deferred.SetLocal([&ctx, id] { SetParentWithUndo(ctx, id, {}, "Set GameObject As Root"); });
            ImGui::Separator();
        }
        for (const auto& [label, direction] : {
                 std::pair{"Move Up", "up"}, std::pair{"Move Down", "down"},
                 std::pair{"Move To Top", "first"}, std::pair{"Move To Bottom", "last"}}) {
            OpArgs args;
            args.Set("node", object.instanceId);
            args.Set("direction", std::string(direction));
            DrawHierarchyOperation(ctx, deferred, label, "node.reorder", std::move(args));
        }
        ImGui::EndMenu();
    }
    DrawHierarchyPrefabMenu(ctx, deferred, object, multi, canEdit);
    ImGui::Separator();
    DrawHierarchyOperation(ctx, deferred, multi ? "Delete Selected" : "Delete", "edit.delete_selected");
    ImGui::EndPopup();
}

struct ObjectAppearance {
    ui::ContentGlyph glyph;
    ThemeColor color;
    const char* type;
};

ObjectAppearance ObjectIcon(scene::GameObject& object)
{
    using Glyph = ui::ContentGlyph;
    if (object.GetComponent<scene::CameraComponent>()) {
        return {Glyph::CAMERA, ThemeColor::Info, "Camera"};
    }
    if (object.GetComponent<scene::LightComponent>()) {
        return {Glyph::LIGHT, ThemeColor::Warning, "Light"};
    }
    if (object.GetComponent<scene::WaterComponent>() || object.GetComponent<scene::WaterfallComponent>()) {
        return {Glyph::WATER, ThemeColor::Info, "Water"};
    }
    if (object.GetComponent<scene::TerrainComponent>() || object.GetComponent<scene::TerrainGridComponent>()) {
        return {Glyph::TERRAIN, ThemeColor::Success, "Terrain"};
    }
    if (object.GetComponent<scene::AudioSourceComponent>()) {
        return {Glyph::AUDIO, ThemeColor::Secondary, "Audio"};
    }
    if (object.GetComponent<scene::VFXComponent>() || object.GetComponent<scene::ParticleEmitter>()) {
        return {Glyph::EFFECTS, ThemeColor::Secondary, "Effects"};
    }
    if (object.GetComponent<scene::UICanvas>()) {
        return {Glyph::CANVAS, ThemeColor::Info, "UI Canvas"};
    }
    if (object.GetComponent<scene::AnimatorComponent>()) {
        return {Glyph::ANIMATION, ThemeColor::Secondary, "Animation"};
    }
    if (object.GetComponent<scene::MeshRenderer>() || object.GetComponent<scene::SkinnedMeshRenderer>()) {
        return {Glyph::MESH, ThemeColor::TextMuted, "Mesh"};
    }
    if (!object.prefabAssetPath.empty()) {
        return {Glyph::PREFAB, ThemeColor::Accent, "Prefab"};
    }
    if (object.GetComponent<scene::RigidBodyComponent>()) {
        return {Glyph::PHYSICS, ThemeColor::TextMuted, "Physics"};
    }
    return {Glyph::OBJECT, ThemeColor::TextMuted, "GameObject"};
}

/// @note 可視行だけで Component を引く。アイコン・名前・件数は同じ行の非対話描画で、入力判定はツリーに残す。
void DrawObjectLabel(scene::GameObject& object, float left, float right, int hiddenCount, bool active)
{
    if (!ImGui::IsItemVisible() || right <= left) {
        return;
    }
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float size = ImGui::GetFontSize();
    const float y = (min.y + max.y - size) * 0.5f;
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const auto icon = ObjectIcon(object);
    auto& draw = *ImGui::GetWindowDrawList();
    draw.PushClipRect({left, min.y}, {right, max.y}, true);
    ui::DrawContentGlyph(icon.glyph, {left + size * 0.5f, y + size * 0.5f}, size,
                         EditorTheme::ColorU32(active ? icon.color : ThemeColor::TextFaint));
    const float textLeft = left + size + gap;
    char badge[32]{};
    float badgeWidth = 0;
    if (hiddenCount > 0) {
        std::snprintf(badge, sizeof(badge), "(+%d)", hiddenCount);
        badgeWidth = ImGui::CalcTextSize(badge).x + gap;
    }
    const float textRight = right - badgeWidth;
    const char* name = object.name.c_str();
    if (textRight > textLeft)
        ImGui::RenderTextEllipsis(&draw, {textLeft, y}, {textRight, max.y}, textRight,
                                  name, name + object.name.size(), nullptr);
    if (badgeWidth > 0 && right - left >= badgeWidth) {
        const float x = std::min(right - badgeWidth + gap, textLeft + ImGui::CalcTextSize(name).x + gap);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::RenderText({x, y}, badge);
        ImGui::PopStyleColor();
    }
    draw.PopClipRect();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
        ImGui::IsMouseHoveringRect({left, min.y}, {right, max.y})) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name);
        ImGui::TextDisabled("%s", LOCT(icon.type));
        if (hiddenCount) {
            ImGui::Text(LOCT("%d generated objects hidden"), hiddenCount);
        }
        ImGui::EndTooltip();
    }
}

/// @param[out] deferred 描画後に実行するシーン編集を書き込む。
/// @param[out] pendingCreate 描画ループの後で呼ぶ生成 Operator (Undo は Operator が積む)。
void DrawHierarchyNode(EditorContext& ctx,
                       scene::GameObject& go,
                       HierarchyTraversal& visited,
                       DeferredHierarchyEdit& deferred,
                       PendingObjectCreate& pendingCreate,
                       HierarchyRevealState reveal,
                       scene::EntityID& lastClicked,
                       scene::EntityID& pendingClick,
                       bool& hierarchyDragStarted,
                       bool& hierarchyNodeHovered,
                       std::vector<scene::EntityID>& outVisible,
                       const std::vector<scene::EntityID>& prevVisible,
                       InlineRenameState rename,
                       bool parentActive = true)
{
    scene::EntityID* const pendingExpand = &reveal.pendingExpand;
    const scene::EntityID id = go.GetID();
    if (!visited.Visit(id)) {
        return;
    }
    if (IsHiddenGenerated(ctx, go)) {
        return;
    }
    outVisible.push_back(id);

    // F2 インラインリネーム: 行をそのまま入力欄に置き換える (Unity 互換)
    // 画面隅のポップアップだと視線移動とマウス移動が毎回発生するため、その場で書き換える。
    if (rename.id == id) {
        ImGui::PushID(static_cast<int>(id.index));
        ImGui::SetNextItemWidth(-1.0f);
        if (rename.focusPending) {
            ImGui::SetKeyboardFocusHere();
            rename.focusPending = false;
        }
        const bool confirmed = ImGui::InputText("##inline_rename", rename.buffer,
            rename.bufferSize,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            rename.id = scene::EntityID{};
        } else if (confirmed || ImGui::IsItemDeactivated()) {
            // Enter またはフォーカス喪失で確定する
            const std::string newName = rename.buffer;
            if (!newName.empty() && newName != go.name) {
                // operator (node.rename) 経由にする: 旧経路の ExecuteSceneEditWithUndo は
                // 名前を 1 つ変えるだけでシーン全体を 2 回 TOML シリアライズしていた。
                OpArgs args;
                args.Set("node", go.instanceId);
                args.Set("name", newName);
                InvokeOperator(ctx, "node.rename", args);
            }
            rename.id = scene::EntityID{};
        }
        ImGui::PopID();
        return;
    }

    const bool hasChildren    = HasVisibleChild(ctx, go, visited);
    const auto key            = HierarchyTraversal::EntityKey(id);
    const bool selected       = visited.selected.contains(key);
    const bool isActive       = parentActive && go.activeSelf();
    // 目アイコンとメニューが切り替えるのは自分の値 (activeSelf)。表示もそれに合わせ、親のせいで無効なときだけ薄く描く。
    const bool isActiveSelf   = GetAuthoringActive(ctx, go);
    const bool isLocked       = visited.locked.contains(key);
    const bool isEditorHidden  = ctx.editorHiddenGuids.count(go.instanceId) > 0;
    const bool isPrimarySelected = ctx.PrimarySelected() == id;
    // Prefab インスタンスを青色で識別し、通常 GO と視覚的に区別する (Unity 互換の UX)。
    const bool isPrefabInstance = !go.prefabAssetPath.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::PushID(static_cast<int>(id.index));

    // SetParent 後の次フレームで強制 open
    if (pendingExpand && *pendingExpand == id) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        *pendingExpand = scene::EntityID{};
    }
    // 他の面で選ばれた対象までの祖先を開く。開いた状態は StateStorage に残るので、
    // 要求はこのフレームの描画が終わった時点で捨ててよい (畳み直しを妨げない)。
    if (visited.revealed.contains(key)) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (const auto request = reveal.expansionRequests.find(go.instanceId); request != reveal.expansionRequests.end()) {
        ImGui::SetNextItemOpen(request->second, ImGuiCond_Always);
        reveal.expansionRequests.erase(request);
    }

    // エディタ専用非表示はシアン（runtime 非アクティブより優先）、非アクティブはグレー、
    // ロック中はオレンジ、プレファブインスタンスは水色
    ui::PushHierarchySelectionColors();
    if (isEditorHidden) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Info));
    } else if (!isActive) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
    } else if (isLocked) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
    } else if (isPrefabInstance) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
    }
    const float labelLeft = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags, "%s", "");
    const float slot = ImGui::GetItemRectSize().y + 2.0f;
    const int actionSlots = isPrefabInstance ? 4 : (ImGui::IsItemHovered() || isEditorHidden) ? 3 :
                            !isActive ? 2 : isLocked ? 1 : 0;
    const float labelRight = ImGui::GetItemRectMax().x - actionSlots * slot;
    DrawObjectLabel(go, labelLeft, labelRight, HiddenGeneratedCount(ctx, go, visited), isActive);
    if (isEditorHidden || !isActive || isLocked || isPrefabInstance) {
        ImGui::PopStyleColor();
    }
    ui::PopHierarchySelectionColors();

    // 右端の操作と行選択に同じ矩形を使い、アイコン上のクリックを行選択から除く。
    const ImVec2 nodeMin         = ImGui::GetItemRectMin();
    const ImVec2 nodeMax         = ImGui::GetItemRectMax();
    const bool   nodeHovered     = ImGui::IsItemHovered();
    const bool   nodeClicked     = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool   nodeToggledOpen = ImGui::IsItemToggledOpen();
    // 右クリックメニュー用のホバー。別ノードのメニューを開いたまま行を移れるよう、
    // BeginPopupContextItem() 内部と同じ AllowWhenBlockedByPopup を使う。
    const bool   nodeHoveredForMenu =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    hierarchyNodeHovered = hierarchyNodeHovered || nodeHovered;

    // 祖先を開いた結果この行が現れたので、そこまでスクロールする。
    if (reveal.scrollTo == id) {
        ImGui::SetScrollHereY(0.5f);
        reveal.scrollTo = scene::EntityID{};
    }

    // 右端 visibility/editor-hide/lock アイコン (DrawList で直接描画)
    {
        const float  h       = nodeMax.y - nodeMin.y;
        const float  btnW    = h + 2.0f;
        // SpanAvailWidth のためnodeMax.x = ウィンドウコンテンツ右端
        const float  rx      = nodeMax.x;

        // アイコン配置: lock | vis | editorHide (右から)
        const ImVec2 ehMin   = { rx - btnW * 3.0f, nodeMin.y };
        const ImVec2 ehMax   = { rx - btnW * 2.0f, nodeMax.y };
        const ImVec2 visMin  = { rx - btnW * 2.0f, nodeMin.y };
        const ImVec2 visMax  = { rx - btnW,         nodeMax.y };
        const ImVec2 lockMin = { rx - btnW,          nodeMin.y };
        const ImVec2 lockMax = { rx,                 nodeMax.y };

        // ヒット判定
        const bool ehHov   = nodeHovered && ImGui::IsMouseHoveringRect(ehMin,   ehMax,   false);
        const bool visHov  = nodeHovered && ImGui::IsMouseHoveringRect(visMin,  visMax,  false);
        const bool lockHov = nodeHovered && ImGui::IsMouseHoveringRect(lockMin, lockMax, false);
        const bool ehClick   = ehHov   && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool visClick  = visHov  && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool lockClick = lockHov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (ehClick) {
            deferred.SetLocal([&ctx, id] { ToggleEditorHiding(ctx, id); });
        }
        if (visClick) deferred.SetLocal([&ctx, id, isActiveSelf] {
            SetGameObjectsActive(ctx, {&id, 1}, !isActiveSelf, "Toggle GameObject");
        });
        if (lockClick) {
            ctx.ToggleLock(id);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ui::DrawSelectionAccent(dl, nodeMin, nodeMax, selected, isPrimarySelected);

        const float glyphSize = ImGui::GetFontSize() * 0.92f;
        const auto drawToggle = [&](ui::ContentGlyph glyph, ImVec2 min, ImVec2 max, bool hovered, ThemeColor color) {
            if (hovered) {
                dl->AddRectFilled(min, max, EditorTheme::ColorU32(ThemeColor::SurfaceHover), 3.0f);
            }
            ui::DrawContentGlyph(glyph, {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f},
                                 glyphSize, EditorTheme::ColorU32(color));
        };
        if (nodeHovered || isEditorHidden)
            drawToggle(isEditorHidden ? ui::ContentGlyph::EYE_OFF : ui::ContentGlyph::EYE, ehMin, ehMax,
                       ehHov, isEditorHidden ? ThemeColor::Info : ThemeColor::TextMuted);
        if (ehHov) {
            ImGui::SetTooltip("%s", LOCT(isEditorHidden ? "Show in Editor" : "Hide in Editor"));
        }

        if (nodeHovered || !isActive)
            drawToggle(isActiveSelf ? ui::ContentGlyph::ENABLED : ui::ContentGlyph::DISABLED, visMin, visMax,
                       visHov, isActive ? ThemeColor::Success : ThemeColor::TextFaint);
        if (visHov) ImGui::SetTooltip("%s", LOCT(isActiveSelf && !isActive ? "Inactive parent" :
                                                 isActiveSelf ? "Deactivate object" : "Activate object"));

        if (nodeHovered || isLocked)
            drawToggle(isLocked ? ui::ContentGlyph::LOCK : ui::ContentGlyph::UNLOCK, lockMin, lockMax,
                       lockHov, isLocked ? ThemeColor::Warning : ThemeColor::TextMuted);
        if (lockHov) {
            ImGui::SetTooltip("%s", LOCT(isLocked ? "Unlock selection" : "Lock selection"));
        }

        // prefab インスタンスバッジ (右端 4 番目スロット, 常時表示の水色ダイヤ)。青いテキスト
        // だけだと非アクティブや選択ハイライトと重なった際に判別しづらいため常設マーカーを置き、
        // ホバーで参照パスをツールチップ表示する。
        if (isPrefabInstance) {
            const ImVec2 pfMin = { rx - btnW * 4.0f, nodeMin.y };
            const ImVec2 pfMax = { rx - btnW * 3.0f, nodeMax.y };
            const ImVec2 pfCenter = { (pfMin.x + pfMax.x) * 0.5f, (pfMin.y + pfMax.y) * 0.5f };
            const float  pfRadius = h * 0.22f;
            // 外周を少し濃く、内側を塗って小さなダイヤ(菱形)にする
            dl->AddNgonFilled(pfCenter, pfRadius, IM_COL32(100, 180, 255, 235), 4);
            dl->AddNgon(pfCenter, pfRadius, IM_COL32(40, 90, 150, 235), 4, 1.0f);
            if (nodeHovered && ImGui::IsMouseHoveringRect(pfMin, pfMax, false)) {
                ImGui::SetTooltip("Prefab instance\n%s", go.prefabAssetPath.c_str());
            }
        }

        // ノードのクリック/ダブルクリック判定 (アイコン領域は除外)
        const bool iconAreaClick = ehClick || visClick || lockClick;
        if (nodeClicked && !nodeToggledOpen
            && !isLocked && !iconAreaClick) {
            // ここではまだ選択を変更しない。MouseClicked はドラッグ開始より先に来るため、
            // 先に選択すると「つかんだ瞬間に Inspector が別 GO へ切り替わる」。
            // クリックかドラッグか確定する MouseReleased まで保留する。
            pendingClick = id;
        }
        if (nodeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && !isLocked && !iconAreaClick) {
            // メッシュバウンズ基準で注視点・距離を決める (Unity の Frame Selected 互換)
            math::Vector3 focusCenter = go.transform.position;
            float focusRadius = 0.0f;
            ComputeGameObjectBounds(go, focusCenter, focusRadius);
            ctx.focusTargetPosition    = focusCenter;
            ctx.focusTargetRadius      = focusRadius;
            ctx.requestFocusOnSelected = true;
        }
    }

    if (!isLocked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        hierarchyDragStarted = true;
        pendingClick = {};
        ImGui::SetDragDropPayload("FBZZ_HIERARCHY_ENTITY", &id, sizeof(id));
        // 掴んだ行が選択に含まれていれば選択全体を運ぶ。payload は掴んだ 1 体のまま。
        dragdrop::SetEntityDrag(id, ctx.selectedEntities);
        const size_t dragCount = dragdrop::DraggedEntities(id).size();
        if (dragCount > 1) {
            ImGui::Text("%s (+%d)", go.name.c_str(), static_cast<int>(dragCount - 1));
        } else {
            ImGui::TextUnformatted(go.name.c_str());
        }
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            std::vector<scene::EntityID> dragged = DraggedRoots(*ctx.activeScene, draggedId, prevVisible);
            deferred.SetLocal([&ctx, dragged, draggedId, id, pendingExpand]() {
                auto* target = ctx.activeScene->GetGameObject(id);
                if (!target) {
                    return;
                }
                std::vector<scene::EntityID> moved;
                for (scene::EntityID childId : dragged) {
                    auto* child = ctx.activeScene->GetGameObject(childId);
                    // 自分自身と、落とし先の祖先は子にできない
                    if (!child || childId == id || target->IsDescendantOf(*child)) {
                        continue;
                    }
                    SetParentWithUndo(ctx, childId, id, "Reparent GameObject");
                    moved.push_back(childId);
                }
                if (moved.empty()) {
                    return;
                }
                SelectMoved(ctx, std::move(moved), draggedId);
                if (pendingExpand) {
                    *pendingExpand = id;
                }
            });
        }
        std::string assetPath;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath)) {
            QueueAssetDrop(ctx, assetPath, id, go.instanceId, deferred, pendingCreate, pendingExpand);
        }
        ImGui::EndDragDropTarget();
    }

    // 兄弟間並べ替え: ノード下端の細い帯を「この直後に挿入」ドロップ先にする
    // ノード本体へのドロップは「子にする」操作。境界線ドロップが無いと並び順の変更に
    // Order メニュー (ルート限定) を往復する羽目になる。
    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        dragging && dragging->IsDataType("FBZZ_HIERARCHY_ENTITY")) {
        // 帯は行の下端に置く。ここで GetItemRect を読み直すと、バッジを持つノードでは
        // バッジの矩形 (行の一部) だけが挿入先になり、行のどこを狙っても入らなくなる。
        // 挿入帯の半分の高さ (px)
        constexpr float kBandHalf = 3.0f;
        const ImRect band({ nodeMin.x, nodeMax.y - kBandHalf },
                          { nodeMax.x, nodeMax.y + kBandHalf });
        const ImGuiID bandId = ImGui::GetID("##reorder_after");
        if (ImGui::BeginDragDropTargetCustom(band, bandId)) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "FBZZ_HIERARCHY_ENTITY",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                // ドロップ位置プレビューの挿入ライン
                ImGui::GetWindowDrawList()->AddLine(
                    { nodeMin.x, nodeMax.y }, { nodeMax.x, nodeMax.y },
                    IM_COL32(100, 180, 255, 255), 2.0f);

                scene::EntityID draggedId;
                if (payload->IsDelivery() && ReadEntityPayload(payload, draggedId)) {
                    std::vector<scene::EntityID> dragged =
                        DraggedRoots(*ctx.activeScene, draggedId, prevVisible);
                    deferred = [&ctx, dragged, draggedId, id]() {
                        auto* anchor = ctx.activeScene->GetGameObject(id);
                        if (!anchor) {
                            return;
                        }
                        scene::GameObject* parent = anchor->GetParent();
                        // 運ぶものを行の並び順のまま anchor の後ろへ 1 つずつ継ぎ足す
                        scene::GameObject* insertAfter = anchor;
                        std::vector<scene::EntityID> moved;
                        for (scene::EntityID movedId : dragged) {
                            auto* item = ctx.activeScene->GetGameObject(movedId);
                            // 自分自身と、自分の子孫の隣には置けない
                            if (!item || movedId == id || anchor->IsDescendantOf(*item)) {
                                continue;
                            }

                            // anchor と同じ親に揃えてから挿入する
                            if (item->GetParent() != parent) {
                                const bool ok = parent ? item->SetParent(parent) : item->ClearParent();
                                if (!ok) {
                                    continue;
                                }
                            }
                            // 一旦末尾へ送って index 計算を単純化する
                            item->SetSiblingIndex(1 << 30);
                            item->SetSiblingIndex(insertAfter->GetSiblingIndex() + 1);
                            insertAfter = item;
                            moved.push_back(movedId);
                        }
                        if (moved.empty()) {
                            return;
                        }
                        SelectMoved(ctx, std::move(moved), draggedId);
                        if (ctx.markSceneDirty) {
                            ctx.markSceneDirty();
                        }
                    };
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (pendingClick == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
        && nodeHovered && !hierarchyDragStarted) {
        const bool shiftHeld = ImGui::GetIO().KeyShift;
        const bool ctrlHeld  = ImGui::GetIO().KeyCtrl;
        // ここでの選択は既に見えている行なので、見せに行く要求は出さない (Skip)。
        if (shiftHeld && lastClicked.IsValid()) {
            // Shift+クリック: prevVisible の順番で lastClicked〜id の範囲を選択
            auto it1 = std::find(prevVisible.begin(), prevVisible.end(), lastClicked);
            auto it2 = std::find(prevVisible.begin(), prevVisible.end(), id);
            if (it1 != prevVisible.end() && it2 != prevVisible.end()) {
                std::vector<scene::EntityID> range =
                    ctrlHeld ? ctx.selectedEntities : std::vector<scene::EntityID>{};
                if (it1 > it2) {
                    std::swap(it1, it2);
                }
                for (auto it = it1; it <= it2; ++it)
                    if (!ctrlHeld || visited.selected.insert(HierarchyTraversal::EntityKey(*it)).second) {
                        range.push_back(*it);
                    }
                SelectEntities(ctx, std::move(range), SelectionReveal::Skip);
            }
        } else {
            if (ctrlHeld) {
                ToggleSelection(ctx, id, SelectionReveal::Skip);
            } else {
                SelectEntity(ctx, id, SelectionReveal::Skip);
            }
            lastClicked = id;
        }
        SetHierarchyMembers(visited.selected, ctx.selectedEntities);
        pendingClick = {};
    }

    // バッジは ID のない描画なので、TreeNodeEx 直後に記録したホバーでポップアップを開く。
    DrawHierarchyNodeMenu(ctx, go, nodeHoveredForMenu, visited, deferred, pendingCreate);

    if (hasChildren && opened) {
        for (int i = 0; i < go.GetChildCount(); ++i) {
            if (auto* child = go.GetChild(i))
                DrawHierarchyNode(ctx, *child, visited, deferred, pendingCreate, reveal,
                                  lastClicked, pendingClick, hierarchyDragStarted,
                                  hierarchyNodeHovered, outVisible, prevVisible, rename, isActive);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

/// @brief 行の無い所を右クリックしたときのメニュー。ツリー・検索結果・Map Mode の 3 表示で共有する。
/// @note Select All はロック済みを除く規則を持つので、Ctrl+A と同じ Operator を呼ぶ。
void DrawHierarchyBackgroundMenu(EditorContext& ctx, DeferredHierarchyEdit& deferred,
                                 PendingObjectCreate& pendingCreate)
{
    if (!ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }

    ImGui::TextDisabled("%s", LOCT("Scene Root"));
    ImGui::Separator();
    if (ImGui::BeginMenu(LOC("Create"), CanInvokeOperator(ctx, "node.create_empty"))) {
        DrawCreateObjectMenuItems(ctx, {}, pendingCreate);
        ImGui::EndMenu();
    }
    DrawHierarchyOperation(ctx, deferred, "Paste at Root", "edit.paste");
    ImGui::Separator();
    DrawHierarchyOperation(ctx, deferred, "Select All", "select.all");
    DrawHierarchyOperation(ctx, deferred, "Clear Selection", "select.clear");
    ImGui::EndPopup();
}

} // namespace

void SceneHierarchyPanel::HandlePanelRequests(EditorContext& ctx)
{
    // フォーカスの申告は IPanel::OnRender が GetHotkeyScope() を見て行う。
    // ここに残すのは、パネル内部の状態 (編集バッファ) を要する要求だけ。
    if (!ctx.requestRenameSelected) {
        return;
    }
    ctx.requestRenameSelected = false;

    if (!ctx.activeScene || ctx.selectedEntities.size() != 1) {
        return;
    }
    auto* go = ctx.activeScene->GetGameObject(ctx.selectedEntities[0]);
    if (!go) {
        return;
    }

    m_renamingId = ctx.selectedEntities[0];
    std::strncpy(m_renameBuffer, go->name.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_renameFocusPending = true;
}

void SceneHierarchyPanel::HandleKeyboardNavigation(EditorContext& ctx)
{
    if (ctx.activeScene == nullptr || m_visibleOrder.empty()) {
        return;
    }
    // 検索欄やインラインリネームへ打っている間は奪わない。
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return;
    }
    if (ImGui::GetIO().WantTextInput || m_renamingId.IsValid()) {
        return;
    }

    // IsKeyPressed でなく Shortcut を使う: 矢印キーは ImGui のキーボードナビも使うため、
    // 素のキー読みだと «ナビの枠» と «選択» が別々に動いて 2 つ光る。Shortcut は routing で
    // キーの所有権を取るのでナビ側が同じキーを消費しない。押しっぱなしのリピートも受ける。
    constexpr ImGuiInputFlags kRepeat = ImGuiInputFlags_Repeat;
    int  step   = 0;
    bool extend = false;
    if (ImGui::Shortcut(ImGuiKey_DownArrow, kRepeat)) {
        step = 1;
    } else if (ImGui::Shortcut(ImGuiKey_UpArrow, kRepeat)) {
        step = -1;
    } else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_DownArrow, kRepeat)) {
        step = 1;
        extend = true;
    } else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_UpArrow, kRepeat)) {
        step = -1;
        extend = true;
    }
    const bool toHome = ImGui::Shortcut(ImGuiKey_Home);
    const bool toEnd  = ImGui::Shortcut(ImGuiKey_End);
    if (step == 0 && !toHome && !toEnd) {
        return;
    }

    const int  last    = static_cast<int>(m_visibleOrder.size()) - 1;
    const auto primary = ctx.PrimarySelected();

    int index = -1;
    for (int i = 0; i <= last; ++i)
        if (m_visibleOrder[static_cast<std::size_t>(i)] == primary) {
            index = i;
            break;
        }

    int next = 0;
    if (toHome) {
        next = 0;
    } else if (toEnd) {
        next = last;
    }
    // 選択が無い / 畳まれて消えた
    else if (index < 0)  next = (step > 0) ? 0 : last;
    else {
        next = std::clamp(index + step, 0, last);
    }

    const scene::EntityID target = m_visibleOrder[static_cast<std::size_t>(next)];
    if (target == primary) {
        return;
    }

    // Shift は «アンカーから今の行まで» を選ぶ。アンカーの決め方も範囲の作り方も
    // Shift+クリック (DrawHierarchyNode) と同じにして、経路で結果が変わらないようにする。
    if (extend && m_lastClickedEntity.IsValid()) {
        auto from = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), m_lastClickedEntity);
        auto to   = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), target);
        if (from != m_visibleOrder.end() && to != m_visibleOrder.end()) {
            if (from > to) {
                std::swap(from, to);
            }
            std::vector<scene::EntityID> range;
            for (auto it = from; it <= to; ++it) {
                range.push_back(*it);
            }
            SelectEntities(ctx, std::move(range), SelectionReveal::Skip);
        }
    } else {
        SelectEntity(ctx, target, SelectionReveal::Skip);
        m_lastClickedEntity = target;
    }

    // 行まで運ぶ。ここは描画が終わったあとなので、次フレームの描画が拾う。
    m_revealScrollTo = target;
}

void SceneHierarchyPanel::ConsumeRevealRequest(EditorContext& ctx)
{
    if (!ctx.hierarchyRevealTarget.IsValid() || !ctx.activeScene) {
        return;
    }

    const scene::EntityID target = ctx.hierarchyRevealTarget;
    ctx.hierarchyRevealTarget = scene::EntityID{};

    auto* go = ctx.activeScene->GetGameObject(target);
    if (!go) {
        return;
    }

    // 検索フィルタ中はツリーそのものが出ていない。対象がヒットしないフィルタなら、
    // 開いても見えないままなので外す。
    if (m_searchFilter[0] != '\0' && !util::StringUtils::ContainsCI(go->name, m_searchFilter)) {
        m_searchFilter[0] = '\0';
    }

    // ランタイム生成物は既定で行が出ない。それを選んだのなら表示を開ける。
    if (go->IsRuntimeGenerated()) {
        ctx.showGeneratedObjects = true;
    }

    m_revealOpenChain.clear();
    for (auto* parent = go->GetParent(); parent; parent = parent->GetParent()) {
        m_revealOpenChain.push_back(parent->GetID());
    }
    m_revealScrollTo = target;
}

void SceneHierarchyPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    EditorOperator inspect;
    inspect.id = "hierarchy.view.inspect";
    inspect.label = "Inspect Hierarchy View";
    inspect.category = "Hierarchy";
    inspect.kind = OpKind::Query;
    inspect.desc = "Hierarchyの検索とマップオブジェクト絞り込みを返す。";
    inspect.exec = [this](OpContext& context, const OpArgs&) {
        auto data = OpData::MakeObject();
        data.Set("search", std::string(m_searchFilter));
        data.Set("mapOnly", context.ctx.mapHierarchyFilter);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(inspect));

    EditorOperator configure;
    configure.id = "hierarchy.view.configure";
    configure.label = "Configure Hierarchy View";
    configure.category = "Hierarchy";
    configure.kind = OpKind::Action;
    configure.desc = "検索・絞り込みを変更する。node/openを指定すると次のツリー描画時に枝を開閉する。";
    configure.params = {
        {"search", OpParamType::String, "Search string", false, {}},
        {"mapOnly", OpParamType::Bool, "Map objects only", false, {}},
        {"node", OpParamType::String, "Object GUID", false, {}},
        {"open", OpParamType::Bool, "Expand the branch", false, {}}
    };
    configure.exec = [this](OpContext& context, const OpArgs& args) {
        auto& ctx = context.ctx;
        const auto search = args.GetString("search");
        if (search.size() >= sizeof(m_searchFilter) || search.find('\0') != std::string::npos
            || args.Has("node") != args.Has("open")) {
            return OpResult::Err("BAD_ARG", "search/node/open");
        }
        if (args.Has("node")) {
            auto* object = ctx.activeScene ? ctx.activeScene->FindByGuid(args.GetString("node")) : nullptr;
            if (!object) {
                return OpResult::Err("NODE_NOT_FOUND", "node");
            }
            if (m_expansionScene != ctx.activeScene) {
                m_expansionScene = ctx.activeScene;
                m_expansionRequests.clear();
            }
            m_expansionRequests.insert_or_assign(object->instanceId, args.GetBool("open"));
        }
        if (args.Has("search")) {
            std::copy(search.begin(), search.end(), m_searchFilter);
            m_searchFilter[search.size()] = '\0';
        }
        if (args.Has("mapOnly")) {
            ctx.mapHierarchyFilter = args.GetBool("mapOnly");
        }
        return OpResult::Ok();
    };
    registry.Register(std::move(configure));
}

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (m_expansionScene != ctx.activeScene) {
        m_expansionScene = ctx.activeScene;
        m_expansionRequests.clear();
    }
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    ConsumeRevealRequest(ctx);
    auto& visited = m_traversal;
    SetHierarchyMembers(visited.selected, ctx.selectedEntities);
    SetHierarchyMembers(visited.locked, ctx.lockedEntities);
    SetHierarchyMembers(visited.revealed, m_revealOpenChain);

    // ドラッグ開始後の選択保留はマウス操作が終わったフレームで解放する。
    // MouseReleased のフレームにはまだドロップ判定が残っているので消さない。
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
        && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_pendingClickEntity = {};
        m_hierarchyDragStarted = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_hierarchyDragStarted = false;
    }
    bool hierarchyNodeHovered = false;

    widgets::UpdateDragAutoScroll();

    PendingObjectCreate pendingCreate;
    if (toolbar::IconButton(LOC("Create"), icons::kAdd)) {
        ImGui::OpenPopup("##hierarchyCreate");
    }
    const auto checkboxWidth = [](const char* caption) {
        return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(caption).x;
    };
    toolbar::SameLineIfFits(checkboxWidth(LOCT("Generated")));
    ImGui::Checkbox(LOC("Generated"), &ctx.showGeneratedObjects);
    ImGui::SetItemTooltip("%s", LOCT("Show temporary objects created by systems, such as VFX and water splashes."));
    if (ImGui::BeginPopup("##hierarchyCreate")) {
        DrawCreateObjectMenu(ctx, pendingCreate);
        ImGui::EndPopup();
    }

    if (ctx.mapEditingMode) {
        ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP MODE");
        toolbar::SameLineIfFits(checkboxWidth(LOCT("Map Objects Only")));
        ImGui::Checkbox(LOC("Map Objects Only"), &ctx.mapHierarchyFilter);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Show only Terrain Grid, Terrain, Water and their children\nUncheck to browse all objects in Map Mode");
        }
    }

    toolbar::SearchField("##hierarchy_search", "Search objects...", m_searchFilter, sizeof(m_searchFilter));

    if (ctx.activeScene->GameObjectCount() == 0 && m_searchFilter[0] == '\0') {
        ImGui::Spacing();
        ImGui::TextDisabled("Scene is empty");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Right-click here to create objects, or drag a model / prefab from the Asset Browser into the Scene.");
        ImGui::PopTextWrapPos();
    }

    // 生成は Operator が Undo を積むので、スナップショット Undo で包む deferred とは分けて持つ。

    // 検索中はフラットリスト。キーボード移動の並びもこのリストが正本になる。
    if (m_searchFilter[0] != '\0') {
        DeferredHierarchyEdit deferred;
        auto& visibleRows = m_nextVisibleOrder;
        visibleRows.clear();

        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!util::StringUtils::ContainsCI(go.name, m_searchFilter)) {
                continue;
            }
            // ツリーと同じ基準で隠す。検索したときだけ生成物が出ると同名の GO が 2 つあるように見える。
            if (IsHiddenGenerated(ctx, go)) {
                continue;
            }
            if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
                const bool isMapObject =
                    go.GetComponent<scene::TerrainGridComponent>()
                    || go.GetComponent<scene::TerrainComponent>()
                    || go.GetComponent<scene::WaterComponent>();
                auto* parentGO = go.GetParent();
                const bool isMapChild = parentGO
                    && parentGO->GetComponent<scene::TerrainComponent>();
                if (!isMapObject && !isMapChild) {
                    continue;
                }
            }
            const scene::EntityID id = go.GetID();
            const bool selected = visited.selected.contains(HierarchyTraversal::EntityKey(id));
            visibleRows.push_back(id);
            ImGui::PushID(static_cast<int>(id.index));

            if (m_renamingId == id) {
                ImGui::SetNextItemWidth(-1.0f);
                if (m_renameFocusPending) {
                    ImGui::SetKeyboardFocusHere();
                    m_renameFocusPending = false;
                }
                const bool confirmed = ImGui::InputText("##inline_rename", m_renameBuffer,
                    sizeof(m_renameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = {};
                } else if (confirmed || ImGui::IsItemDeactivated()) {
                    const std::string newName = m_renameBuffer;
                    if (!newName.empty() && newName != go.name) {
                        OpArgs args;
                        args.Set("node", go.instanceId);
                        args.Set("name", newName);
                        InvokeOperator(ctx, "node.rename", args);
                    }
                    m_renamingId = {};
                }
                ImGui::PopID();
                continue;
            }

            ui::PushHierarchySelectionColors();
            const float labelLeft = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable("##search_result", selected)) {
                if (ImGui::GetIO().KeyCtrl) {
                    ToggleSelection(ctx, id, SelectionReveal::Skip);
                } else {
                    SelectEntity(ctx, id, SelectionReveal::Skip);
                }
                SetHierarchyMembers(visited.selected, ctx.selectedEntities);
            }
            DrawObjectLabel(go, labelLeft, ImGui::GetItemRectMax().x, 0, go.activeSelf());
            ui::PopHierarchySelectionColors();
            if (m_revealScrollTo == id) {
                ImGui::SetScrollHereY(0.5f);
                m_revealScrollTo = {};
            }
            ui::DrawSelectionAccent(ImGui::GetWindowDrawList(),
                                    ImGui::GetItemRectMin(),
                                    ImGui::GetItemRectMax(),
                                    selected,
                                    ctx.PrimarySelected() == id);
            const bool hoveredForMenu = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
            DrawHierarchyNodeMenu(ctx, go, hoveredForMenu, visited, deferred, pendingCreate);
            ImGui::PopID();
        }

        m_revealOpenChain.clear();
        m_revealScrollTo = {};
        m_visibleOrder.swap(visibleRows);
        if (m_visibleOrder.empty()) {
            ImGui::TextDisabled("%s", LOCT("No matching objects"));
        }

        HandleKeyboardNavigation(ctx);
        HandlePanelRequests(ctx);
        DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);
        deferred.Run(ctx);
        InvokePendingObjectCreate(ctx, pendingCreate);
        return;
    }

    // Map Mode は Terrain Grid / Terrain / Water のルートだけをツリーで出す (子を親から切り離して見せない)。
    visited.BeginFrame(*ctx.activeScene, ctx.showGeneratedObjects);
    if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
        DeferredHierarchyEdit deferred;

        const auto& roots = visited.roots;
        auto& mapVisible = m_nextVisibleOrder;
        mapVisible.clear();
        for (const auto id : roots) {
            auto* go = ctx.activeScene->GetGameObject(id);
            if (!go) {
                continue;
            }
            if (!go->GetComponent<scene::TerrainGridComponent>()
                && !go->GetComponent<scene::TerrainComponent>()
                && !go->GetComponent<scene::WaterComponent>()) {
                continue;
            }
            DrawHierarchyNode(ctx, *go, visited, deferred, pendingCreate,
                HierarchyRevealState{ m_pendingExpand, m_revealScrollTo, m_expansionRequests },
                m_lastClickedEntity, m_pendingClickEntity, m_hierarchyDragStarted,
                hierarchyNodeHovered, mapVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
        }
        m_revealOpenChain.clear();
        m_revealScrollTo = {};
        m_visibleOrder.swap(mapVisible);

        HandleKeyboardNavigation(ctx);
        HandlePanelRequests(ctx);
        DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);
        deferred.Run(ctx);
        InvokePendingObjectCreate(ctx, pendingCreate);
        return;
    }

    // 描画ループ中にシーンを変えると後続の反復でポインタが無効になるので、変更はループ後にまとめて実行する。
    DeferredHierarchyEdit deferred;
    auto& outVisible = m_nextVisibleOrder;
    outVisible.clear();
    const ImVec2 hierarchyMin = ImGui::GetWindowPos();
    const ImVec2 hierarchyMax = {
        hierarchyMin.x + ImGui::GetWindowSize().x,
        hierarchyMin.y + ImGui::GetWindowSize().y
    };
    const ImGuiID hierarchyDropId = ImGui::GetID("##hierarchy_drop_target");

    const auto& roots = visited.roots;
    for (const auto id : roots)
        if (auto* root = ctx.activeScene->GetGameObject(id)) DrawHierarchyNode(ctx, *root, visited, deferred, pendingCreate,
            HierarchyRevealState{ m_pendingExpand, m_revealScrollTo, m_expansionRequests },
            m_lastClickedEntity, m_pendingClickEntity,
            m_hierarchyDragStarted, hierarchyNodeHovered, outVisible, m_visibleOrder,
            InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });

    // 失効した親を参照する孤立オブジェクトは、通常のルート順の末尾で救済する。
    for (const auto id : visited.recoveredRoots) {
        if (auto* go = ctx.activeScene->GetGameObject(id))
            DrawHierarchyNode(ctx, *go, visited, deferred, pendingCreate,
                HierarchyRevealState{ m_pendingExpand, m_revealScrollTo, m_expansionRequests },
                m_lastClickedEntity, m_pendingClickEntity,
                m_hierarchyDragStarted, hierarchyNodeHovered, outVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
    }

    // 開いた状態は ImGui 側に残るので、見せに行く要求を持ち続けると畳み直せなくなる。
    m_revealOpenChain.clear();
    m_revealScrollTo = {};

    m_visibleOrder.swap(outVisible);

    // 行の上では各ノードのドロップ先に任せる。背景まで受け取ると «自分の行で離した» をルート化と誤認する。
    if (!hierarchyNodeHovered
        && ImGui::BeginDragDropTargetCustom(ImRect(hierarchyMin, hierarchyMax), hierarchyDropId)) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            std::vector<scene::EntityID> dragged = DraggedRoots(*ctx.activeScene, draggedId, m_visibleOrder);
            deferred = [&ctx, dragged, draggedId]() {
                std::vector<scene::EntityID> moved;
                for (scene::EntityID movedId : dragged) {
                    if (auto* item = ctx.activeScene->GetGameObject(movedId)) {
                        item->ClearParent();
                        moved.push_back(movedId);
                    }
                }
                SelectMoved(ctx, std::move(moved), draggedId);
            };
        }
        std::string assetPath;
        // 背景へのドロップは選択に関係なくルート (エンティティを背景へ落とすとルート化するのと同じ)。
        // 選択の子に置きたいときはノードへ落とすか、メニューの Prefab を使う。
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath)) {
            QueueAssetDrop(ctx, assetPath, scene::EntityID{}, std::string{}, deferred, pendingCreate, nullptr);
        }
        ImGui::EndDragDropTarget();
    }

    HandleKeyboardNavigation(ctx);
    HandlePanelRequests(ctx);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered()) {
        ClearEntitySelection(ctx);
        m_lastClickedEntity = {};
    }

    DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);

    deferred.Run(ctx);
    InvokePendingObjectCreate(ctx, pendingCreate);
}

} // namespace fbzz::editor
