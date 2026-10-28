/// @file    FiberPaintEditing.cpp
/// @brief   草ペイントの候補作成、プレビューと検証付き一括 Undo。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <algorithm>
#include <set>

namespace fbzz::editor {
namespace {
std::vector<std::string>& DraftPaths() { static std::vector<std::string> paths; return paths; }
asset::FiberPaintAsset* Resolve(const std::string& path)
{
    return asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(path));
}
void RegisterDirty(EditorContext& ctx, const FiberPaintEdit& edit, const std::shared_ptr<asset::FiberPaintAsset>& snapshot)
{
    const auto diskPath = ToProjectAssetDiskPath(ctx.projectRoot,edit.path);
    if (diskPath.empty()) return;
    auto& paths = DraftPaths();
    if (std::find(paths.begin(),paths.end(),diskPath) == paths.end()) paths.push_back(diskPath);
    AssetDirtyRegistry::RegisterDraft(diskPath,edit.path,"FIBER",snapshot,[path=edit.path,diskPath](const asset::FiberPaintAsset& draft) {
        const auto* current = Resolve(path);
        return current && current->data == draft.data && asset::SaveFiberPaintAssetToFile(diskPath,draft);
    });
}
bool ValidateEdits(EditorContext& ctx, const FiberPaintTransaction& transaction, bool applied, std::string& reason)
{
    if (!transaction.owner || ctx.activeScene != transaction.owner || transaction.discardEpoch != AssetDirtyRegistry::DiscardEpoch()) {
        reason = "草ペイントの編集シーンまたは未保存データが変わりました。"; return false;
    }
    for (const auto& edit : transaction.edits) {
        auto* go = transaction.owner->FindByGuid(edit.node);
        const auto* fiber = go ? go->GetComponent<scene::FiberComponent>() : nullptr;
        const auto* current = Resolve(edit.path);
        const auto& expected = applied ? edit.after : edit.before;
        const auto* mesh = go ? go->GetComponent<scene::MeshRenderer>() : nullptr;
        if (!fiber || fiber->m_paintPath != edit.path || !current || !expected || !mesh || !mesh->mesh
            || transaction.owner->GetComponentLifetime<scene::FiberComponent>(go->GetID()) != edit.lifetime
            || renderer::FiberMeshSignature(*mesh->mesh) != expected->data.meshSignature
            || !(current->data == expected->data)) {
            reason = "草の対象・形状・ペイントが変更されています。ストロークを適用できません。"; return false;
        }
    }
    return true;
}
class FiberPaintCommand final : public ICommand {
public:
    FiberPaintCommand(EditorContext& ctx, FiberPaintTransaction transaction) : m_context(&ctx), m_transaction(std::move(transaction)) {}
    void Execute() override { std::string reason; (void)TryExecute(reason); }
    void Undo() override { std::string reason; (void)TryUndo(reason); }
    std::string GetDescription() const override { return "Paint Grass"; }
    bool SupportsLegacyComposition() const override { return false; }
    bool ValidateHistory(std::string& reason) const override { return ValidateEdits(*m_context,m_transaction,m_applied,reason); }
    bool TryExecute(std::string& reason) override { return Apply(true,reason); }
    bool TryUndo(std::string& reason) override { return Apply(false,reason); }
private:
    bool Apply(bool after, std::string& reason)
    {
        if (after == m_applied) { reason = "草ペイントの履歴状態が一致しません。"; return false; }
        if (!ValidateEdits(*m_context,m_transaction,m_applied,reason)) return false;
        for (const auto& edit : m_transaction.edits) {
            auto* current = Resolve(edit.path);
            const auto& snapshot = after ? edit.after : edit.before;
            current->data = snapshot->data;
            asset::TouchFiberPaintAsset(*current);
            RegisterDirty(*m_context,edit,snapshot);
        }
        m_applied = after;
        if (m_context->markSceneDirty) m_context->markSceneDirty();
        reason.clear(); return true;
    }
    EditorContext* m_context;
    FiberPaintTransaction m_transaction;
    bool m_applied = true;
};
} /// @note namespace

void SynchronizeFiberPaintDrafts()
{
    static uint64_t epoch = AssetDirtyRegistry::DiscardEpoch();
    if (epoch == AssetDirtyRegistry::DiscardEpoch()) return;
    epoch = AssetDirtyRegistry::DiscardEpoch();
    for (const auto& path : DraftPaths()) asset::AssetManager::ReloadPath(path);
    DraftPaths().clear();
}
std::vector<scene::GameObject*> FiberPaintTargets(EditorContext& ctx, bool allRegistered, bool requireFiber)
{
    std::vector<scene::GameObject*> result;
    if (!ctx.activeScene) return result;
    std::set<std::string> seen;
    const auto add = [&](scene::GameObject& go) {
        if (!seen.insert(go.instanceId).second || !go.activeInHierarchy()) return;
        const auto* mesh = go.GetComponent<scene::MeshRenderer>();
        const auto* fiber = go.GetComponent<scene::FiberComponent>();
        if (!mesh || !mesh->enabled || !mesh->mesh || mesh->mesh->isSkinned || mesh->mesh->cpuIndices.empty()) return;
        if (requireFiber && (!fiber || !fiber->m_enabled || fiber->m_mode != scene::FiberRenderMode::BLADE || !fiber->m_chunkedPlacement)) return;
        result.push_back(&go);
    };
    if (allRegistered) { for (auto& go : ctx.activeScene->GameObjects()) add(go); }
    else {
        const auto visit = [&](auto&& self, scene::GameObject& go) -> void {
            add(go);
            for (int i = 0; i < go.GetChildCount(); ++i) if (auto* child = go.GetChild(i)) self(self,*child);
        };
        for (auto id : ctx.selectedEntities) if (auto* go = ctx.activeScene->GetGameObject(id)) visit(visit,*go);
    }
    return result;
}
scene::GameObject* FiberPaintRaycast(EditorContext& ctx, const math::Vector3& origin,
    const math::Vector3& direction, float& distance, uint32_t& triangle)
{
    scene::GameObject* hit = nullptr;
    if (!ctx.activeScene) return hit;
    for (auto& go : ctx.activeScene->GameObjects()) {
        const auto* mesh = go.GetComponent<scene::MeshRenderer>();
        if (!go.activeInHierarchy() || !mesh || !mesh->enabled || !mesh->lodVisible || !mesh->mesh) continue;
        if (renderer::RaycastFiberSurface(*mesh->mesh,go.transform.GetWorldMatrix(),origin,direction,distance,triangle)) hit = &go;
    }
    return hit;
}
bool PrepareFiberPaintEdits(EditorContext& ctx, const std::vector<renderer::FiberPaintStroke>& strokes,
    bool allRegistered, bool visibleOnly, std::vector<FiberPaintEdit>& out, std::string& reason)
{
    SynchronizeFiberPaintDrafts();
    if (!ctx.activeScene || strokes.empty() || strokes.size() > 4096 || (visibleOnly && !ctx.editorCamera)) {
        reason = "シーン・ブラシ・カメラの状態を確認してください。"; return false;
    }
    auto targets = FiberPaintTargets(ctx,allRegistered);
    if (targets.empty()) { reason = "草を追加した静的 Mesh を選択してください。"; return false; }
    std::vector<FiberPaintEdit> result;
    std::set<std::string> paths;
    for (auto* go : targets) {
        const auto* fiber = go->GetComponent<scene::FiberComponent>();
        const auto* mesh = go->GetComponent<scene::MeshRenderer>();
        const auto* source = Resolve(fiber->m_paintPath);
        if (!source || fiber->m_paintPath.empty() || !paths.insert(fiber->m_paintPath).second) {
            reason = "個体別のペイントが必要です。草を追加し直すか、共有ペイントの対象を一つ選択してください。"; return false;
        }
        FiberPaintEdit edit;
        edit.node = go->instanceId; edit.path = fiber->m_paintPath;
        edit.lifetime = ctx.activeScene->GetComponentLifetime<scene::FiberComponent>(go->GetID());
        edit.before = std::make_shared<asset::FiberPaintAsset>(*source);
        edit.after = std::make_shared<asset::FiberPaintAsset>(*source);
        std::function<bool(const math::Vector3&,uint32_t)> visible;
        if (visibleOnly) visible = [&ctx,go](const math::Vector3& point,uint32_t triangle) {
            auto from = ctx.editorCamera->m_position;
            if (ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic) {
                const auto forward = ctx.editorCamera->GetForward();
                from = point-forward*math::Vector3::Dot(point-from,forward);
            }
            const auto delta = point-from;
            const float length = delta.Length();
            if (length <= 1.0e-6f) return false;
            float distance = length+0.01f;
            uint32_t found = 0;
            auto* target = FiberPaintRaycast(ctx,from,delta/length,distance,found);
            return target == go && found == triangle && distance >= length-0.01f;
        };
        bool changed = false;
        for (auto stroke : strokes) {
            stroke.maxSlope = std::min(stroke.maxSlope,fiber->m_maxSlope);
            uint32_t count = 0;
            if (!renderer::PaintFiberSurface(*mesh->mesh,go->transform.GetWorldMatrix(),stroke,edit.after->data,count,visible)) {
                reason = "ブラシ値・Mesh の形状またはペイントの上限が不正です。変更は適用されていません。"; return false;
            }
            changed = changed || count > 0;
        }
        if (changed) result.push_back(std::move(edit));
    }
    out = std::move(result); reason.clear(); return true;
}
bool ApplyFiberPaintPreview(EditorContext& ctx, const std::vector<FiberPaintEdit>& edits, std::string& reason)
{
    FiberPaintTransaction transaction{ctx.activeScene,AssetDirtyRegistry::DiscardEpoch(),edits};
    if (!ValidateEdits(ctx,transaction,false,reason)) return false;
    for (const auto& edit : edits) {
        auto* current = Resolve(edit.path);
        current->data = edit.after->data; asset::TouchFiberPaintAsset(*current);
    }
    return true;
}
void CancelFiberPaintPreview(EditorContext& ctx, FiberPaintTransaction& transaction)
{
    if (ctx.activeScene == transaction.owner && transaction.discardEpoch == AssetDirtyRegistry::DiscardEpoch()) {
        for (const auto& edit : transaction.edits) {
            auto* go = transaction.owner->FindByGuid(edit.node);
            const auto* fiber = go ? go->GetComponent<scene::FiberComponent>() : nullptr;
            auto* current = Resolve(edit.path);
            if (fiber && fiber->m_paintPath == edit.path && current && current->data == edit.after->data) {
                current->data = edit.before->data; asset::TouchFiberPaintAsset(*current);
            }
        }
    }
    transaction = {};
}
OpResult CommitFiberPaintTransaction(EditorContext& ctx, const FiberPaintTransaction& transaction)
{
    std::string reason;
    if (!ValidateEdits(ctx,transaction,true,reason)) return OpResult::Err("STALE_STROKE",reason);
    if (transaction.edits.empty()) { OpResult result; result.noChange = true; return result; }
    for (const auto& edit : transaction.edits) RegisterDirty(ctx,edit,edit.after);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    OpResult result;
    result.command = std::make_unique<FiberPaintCommand>(ctx,transaction);
    return result;
}
} /// @note namespace fbzz::editor
