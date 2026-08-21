// FBZZ Engine
// SceneEditUtils.cpp | fbzz::editor
// シーン編集の共有ヘルパー実装
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace fbzz::editor {

namespace {

scene::Scene g_gameObjectClipboard;
std::vector<scene::EntityID> g_gameObjectClipboardRoots;

bool HasSelectedAncestor(EditorContext& ctx, scene::GameObject& go)
{
    for (scene::GameObject* parent = go.GetParent(); parent; parent = parent->GetParent()) {
        if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), parent->GetID())
            != ctx.selectedEntities.end())
            return true;
    }
    return false;
}

scene::EntityID CopyHierarchyToSceneRecursive(const scene::Scene& srcScene,
                                              scene::GameObject& src,
                                              scene::Scene& dstScene,
                                              scene::EntityID parentId,
                                              bool addCopySuffix)
{
    auto& dst = dstScene.CreateGameObject(src.name + (addCopySuffix ? " (Copy)" : ""));
    dst.tag             = src.tag;
    dst.layer           = src.layer;
    dst.prefabAssetPath = src.prefabAssetPath;
    dst.transform       = src.transform;
    dstScene.CopyComponentsFrom(srcScene, src.GetID(), dst.GetID());

    if (parentId.IsValid()) {
        if (auto* parent = dstScene.GetGameObject(parentId))
            dst.SetParent(parent);
    }

    for (int i = 0; i < src.GetChildCount(); ++i) {
        if (auto* child = src.GetChild(i))
            CopyHierarchyToSceneRecursive(srcScene, *child, dstScene, dst.GetID(), false);
    }
    return dst.GetID();
}

} // namespace

std::unique_ptr<ICommand> MakeSceneEditCommand(EditorContext& ctx,
                                               const char* description,
                                               const std::function<void()>& edit)
{
    if (!ctx.activeScene || !edit) return nullptr;

    const bool canRecordUndo =
        ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
    if (!canRecordUndo) {
        // Play 中などは履歴を残さない。編集そのものは行う。
        edit();
        if (ctx.markSceneDirty) ctx.markSceneDirty();
        return nullptr;
    }

    const std::string before = SceneIO::Serialize(*ctx.activeScene);
    const std::size_t historyRevisionBefore = ctx.undoStack->GetRevision();
    edit();
    const std::string after = SceneIO::Serialize(*ctx.activeScene);

    // Reparent 等が専用コマンドを追加済みなら、全シーンコマンドとの二重登録を避ける。
    if (before == after ||
        ctx.undoStack->GetRevision() != historyRevisionBefore) {
        if (before != after && ctx.markSceneDirty) ctx.markSceneDirty();
        return nullptr;
    }

    scene::Scene* scene = ctx.activeScene;
    EditorContext* context = &ctx;
    const auto markDirty = ctx.markSceneDirty;
    auto restore = [scene, context, markDirty](const std::string& snapshot) {
        if (SceneIO::Deserialize(*scene, snapshot)) {
            context->selectedEntities.clear();
            context->activeUICanvas = {};
            if (markDirty) markDirty();
        }
    };
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    return std::make_unique<LambdaCommand>(
        description,
        [restore, after]() { restore(after); },
        [restore, before]() { restore(before); });
}

void ExecuteSceneEditWithUndo(EditorContext& ctx,
                              const char* description,
                              const std::function<void()>& edit)
{
    auto command = MakeSceneEditCommand(ctx, description, edit);
    if (command && ctx.undoStack != nullptr)
        ctx.undoStack->Push(std::move(command));
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

scene::EntityID DuplicateHierarchyRecursive(EditorContext& ctx,
                                            scene::EntityID srcId,
                                            scene::EntityID parentId,
                                            bool addCloneSuffix)
{
    auto* src = ctx.activeScene->GetGameObject(srcId);
    if (!src) return scene::EntityID::INVALID;

    auto& dst = ctx.activeScene->CreateGameObject(
        src->name + (addCloneSuffix ? " (Clone)" : ""));
    dst.tag       = src->tag;
    dst.layer     = src->layer;
    dst.prefabAssetPath = src->prefabAssetPath;
    dst.transform = src->transform;
    ctx.activeScene->DuplicateComponents(srcId, dst.GetID());
    ctx.editorSceneState.CopyComponentOrder(src->instanceId, dst.instanceId);

    if (parentId.IsValid()) {
        if (auto* parent = ctx.activeScene->GetGameObject(parentId))
            dst.SetParent(parent);
    }

    for (int i = 0; i < src->GetChildCount(); ++i) {
        if (auto* child = src->GetChild(i))
            DuplicateHierarchyRecursive(ctx, child->GetID(), dst.GetID(), false);
    }
    return dst.GetID();
}

std::unique_ptr<ICommand> MakeDeleteSelectedCommand(EditorContext& ctx)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return nullptr;
    const std::vector<scene::EntityID> ids = ctx.selectedEntities;
    return MakeSceneEditCommand(ctx, "Delete GameObjects",
        [&ctx, ids]() { DestroySelected(ctx, ids); });
}

std::unique_ptr<ICommand> MakeDuplicateSelectedCommand(EditorContext& ctx)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return nullptr;
    const std::vector<scene::EntityID> toDup = ctx.selectedEntities;
    return MakeSceneEditCommand(ctx, "Duplicate GameObjects", [&ctx, toDup]() {
        std::vector<scene::EntityID> newIds;
        for (auto eid : toDup) {
            auto* src = ctx.activeScene->GetGameObject(eid);
            const scene::EntityID parentId = src && src->GetParent()
                ? src->GetParent()->GetID() : scene::EntityID{};
            const scene::EntityID newId =
                DuplicateHierarchyRecursive(ctx, eid, parentId, true);
            if (newId != scene::EntityID::INVALID)
                newIds.push_back(newId);
        }
        if (!newIds.empty()) ctx.selectedEntities = newIds;
    });
}

void DeleteSelectedWithUndo(EditorContext& ctx)
{
    auto command = MakeDeleteSelectedCommand(ctx);
    if (command && ctx.undoStack != nullptr) ctx.undoStack->Push(std::move(command));
}

void DuplicateSelectedWithUndo(EditorContext& ctx)
{
    auto command = MakeDuplicateSelectedCommand(ctx);
    if (command && ctx.undoStack != nullptr) ctx.undoStack->Push(std::move(command));
}

std::unique_ptr<ICommand> MakeRenameNodeCommand(EditorContext& ctx,
                                                scene::EntityID id,
                                                std::string newName,
                                                const char* label,
                                                bool applyNow)
{
    if (!ctx.activeScene || newName.empty()) return nullptr;
    scene::GameObject* go = ctx.activeScene->GetGameObject(id);
    if (go == nullptr || go->name == newName) return nullptr;

    // ポインタではなく EntityID を捕捉して毎回引き直す。
    // WHY: Undo/Redo の途中でシーンが差し替わっても対象を取り違えず、
    //      解放後参照にもならない。
    scene::Scene* scene = ctx.activeScene;
    const auto markDirty = ctx.markSceneDirty;
    const std::string oldName = go->name;

    if (applyNow) {
        go->name = newName;
        if (markDirty) markDirty();
    }

    return std::make_unique<LambdaCommand>(
        label,
        [scene, id, newName, markDirty]() {
            if (auto* g = scene->GetGameObject(id)) g->name = newName;
            if (markDirty) markDirty();
        },
        [scene, id, oldName, markDirty]() {
            if (auto* g = scene->GetGameObject(id)) g->name = oldName;
            if (markDirty) markDirty();
        });
}

void CopySelectedToClipboard(EditorContext& ctx)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return;

    g_gameObjectClipboard.Clear();
    g_gameObjectClipboardRoots.clear();

    for (scene::EntityID id : ctx.selectedEntities) {
        auto* src = ctx.activeScene->GetGameObject(id);
        if (!src || HasSelectedAncestor(ctx, *src)) continue;

        const scene::EntityID copiedRoot = CopyHierarchyToSceneRecursive(
            *ctx.activeScene, *src, g_gameObjectClipboard, scene::EntityID{}, false);
        if (copiedRoot != scene::EntityID::INVALID)
            g_gameObjectClipboardRoots.push_back(copiedRoot);
    }
}

bool HasGameObjectClipboard()
{
    return !g_gameObjectClipboardRoots.empty();
}

std::unique_ptr<ICommand> MakePasteClipboardCommand(EditorContext& ctx, scene::EntityID parentId)
{
    if (!ctx.activeScene || g_gameObjectClipboardRoots.empty()) return nullptr;

    const std::vector<scene::EntityID> roots = g_gameObjectClipboardRoots;
    return MakeSceneEditCommand(ctx, "Paste GameObjects", [&ctx, parentId, roots]() {
        std::vector<scene::EntityID> pastedIds;
        for (scene::EntityID sourceRootId : roots) {
            auto* sourceRoot = g_gameObjectClipboard.GetGameObject(sourceRootId);
            if (!sourceRoot) continue;

            const scene::EntityID pastedId = CopyHierarchyToSceneRecursive(
                g_gameObjectClipboard, *sourceRoot, *ctx.activeScene, parentId, true);
            if (pastedId != scene::EntityID::INVALID)
                pastedIds.push_back(pastedId);
        }

        if (!pastedIds.empty())
            ctx.selectedEntities = pastedIds;
    });
}

void PasteClipboardWithUndo(EditorContext& ctx, scene::EntityID parentId)
{
    auto command = MakePasteClipboardCommand(ctx, parentId);
    if (command && ctx.undoStack != nullptr) ctx.undoStack->Push(std::move(command));
}

namespace {

// 2 つの球を包含する最小球へ球 0 を拡張する
void MergeSpheres(math::Vector3& c0, float& r0, const math::Vector3& c1, float r1)
{
    const math::Vector3 d = { c1.x - c0.x, c1.y - c0.y, c1.z - c0.z };
    const float dist = d.Length();
    if (dist + r1 <= r0) return;                          // 球1 は球0 に内包
    if (dist + r0 <= r1) { c0 = c1; r0 = r1; return; }    // 球0 は球1 に内包
    const float newR = (dist + r0 + r1) * 0.5f;
    const float t = (dist > 0.0001f) ? (newR - r0) / dist : 0.0f;
    c0 = { c0.x + d.x * t, c0.y + d.y * t, c0.z + d.z * t };
    r0 = newR;
}

} // namespace

void ComputeGameObjectBounds(scene::GameObject& go,
                             math::Vector3& outCenter,
                             float& outRadius)
{
    const math::Matrix4 world = go.transform.GetWorldMatrix();
    const math::Vector3& ws = go.transform.worldScale;
    const float maxScale = (std::max)(
        (std::max)(std::abs(ws.x), std::abs(ws.y)), std::abs(ws.z));

    auto worldPoint = [&world](const math::Vector3& p) {
        const math::Vector4 v = world * math::Vector4{ p.x, p.y, p.z, 1.0f };
        return math::Vector3{ v.x, v.y, v.z };
    };

    // メッシュバウンズが無い GO (空・ライト等) は原点+固定半径で扱う
    outCenter = worldPoint({ 0.0f, 0.0f, 0.0f });
    outRadius = 0.5f;
    bool found = false;

    if (auto* mr = go.GetComponent<scene::MeshRenderer>();
        mr && mr->mesh && mr->mesh->boundsRadius > 0.0f) {
        outCenter = worldPoint(mr->mesh->boundsCenter);
        outRadius = mr->mesh->boundsRadius * (std::max)(maxScale, 0.0001f);
        found = true;
    }

    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>(); smr && smr->model) {
        // この Renderer が描く submesh の境界球を合成する (フレーム選択の範囲)。
        // 非表示スロットの submesh は描画されないので境界にも含めない。
        // i はローカルスロット番号なので、マテリアルスロットとそのまま対応する。
        const auto* mat = go.GetComponent<scene::MaterialComponent>();
        for (size_t i = 0; i < smr->SubmeshCount(); ++i) {
            const renderer::Mesh* meshPtr = smr->SubmeshMesh(i);
            if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
            if (mat && !mat->SlotAt(i).visible) continue;
            const math::Vector3 c = worldPoint(meshPtr->boundsCenter);
            const float r = meshPtr->boundsRadius * (std::max)(maxScale, 0.0001f);
            if (!found) { outCenter = c; outRadius = r; found = true; }
            else        MergeSpheres(outCenter, outRadius, c, r);
        }
    }
}

bool ComputeSelectionBounds(EditorContext& ctx,
                            math::Vector3& outCenter,
                            float& outRadius)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return false;

    bool any = false;
    math::Vector3 center{};
    float radius = 0.0f;
    for (scene::EntityID id : ctx.selectedEntities) {
        auto* go = ctx.activeScene->GetGameObject(id);
        if (!go) continue;
        math::Vector3 c{};
        float r = 0.0f;
        ComputeGameObjectBounds(*go, c, r);
        if (!any) { center = c; radius = r; any = true; }
        else      MergeSpheres(center, radius, c, r);
    }
    if (!any) return false;

    outCenter = center;
    outRadius = radius;
    return true;
}

} // namespace fbzz::editor
