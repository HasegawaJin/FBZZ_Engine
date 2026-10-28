/// @file    FiberOperators.cpp
/// @brief   Mesh への草の一括追加、全面配置とペイント操作の登録。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <optional>
#include <cmath>

namespace fbzz::editor {
namespace {
bool Editable(const EditorContext& ctx)
{
    return ctx.activeScene && (!ctx.playMode || ctx.playMode->IsInEditor());
}
OpParam Parameter(const char* name, OpParamType type, OpValue fallback)
{
    OpParam value; value.name = name; value.type = type; value.required = false; value.defaultValue = std::move(fallback); return value;
}
struct Registration {
    std::string node;
    std::optional<scene::FiberComponent> before;
    scene::FiberComponent after;
    uint64_t lifetime = 0;
};
bool SameRegistration(const scene::FiberComponent& a, const scene::FiberComponent& b)
{
    return a.m_enabled == b.m_enabled && a.m_mode == b.m_mode && a.m_materialPath == b.m_materialPath
        && a.m_paintPath == b.m_paintPath && a.m_chunkedPlacement == b.m_chunkedPlacement
        && a.m_bladeDensity == b.m_bladeDensity && a.m_bladeWidth == b.m_bladeWidth
        && a.m_seed == b.m_seed && a.m_chunkSize == b.m_chunkSize && a.m_maxSlope == b.m_maxSlope
        && a.m_maxBlades == b.m_maxBlades && a.m_lengthScale == b.m_lengthScale;
}
class FiberRegistrationCommand final : public ICommand {
public:
    FiberRegistrationCommand(EditorContext& ctx, std::vector<Registration> entries)
        : m_context(&ctx), m_owner(ctx.activeScene), m_entries(std::move(entries)) {}
    void Execute() override { std::string reason; (void)TryExecute(reason); }
    void Undo() override { std::string reason; (void)TryUndo(reason); }
    std::string GetDescription() const override { return "Add Grass"; }
    bool SupportsLegacyComposition() const override { return false; }
    bool ValidateHistory(std::string& reason) const override
    {
        if (m_context->activeScene != m_owner) { reason = "草を追加したシーンが変わりました。"; return false; }
        for (const auto& entry : m_entries) {
            auto* go = m_owner->FindByGuid(entry.node);
            const auto* fiber = go ? go->GetComponent<scene::FiberComponent>() : nullptr;
            const auto* expected = m_applied ? &entry.after : (entry.before ? &*entry.before : nullptr);
            if (!go || (expected ? (!fiber || !SameRegistration(*fiber,*expected)
                || m_owner->GetComponentLifetime<scene::FiberComponent>(go->GetID()) != entry.lifetime) : fiber != nullptr)) {
                reason = "草の対象または Component が変更されています。"; return false;
            }
        }
        return true;
    }
    bool TryExecute(std::string& reason) override { return Apply(true,reason); }
    bool TryUndo(std::string& reason) override { return Apply(false,reason); }
private:
    bool Apply(bool after, std::string& reason)
    {
        if (m_applied == after || !ValidateHistory(reason)) return false;
        for (auto& entry : m_entries) {
            auto* go = m_owner->FindByGuid(entry.node);
            const auto* value = after ? &entry.after : (entry.before ? &*entry.before : nullptr);
            if (value) {
                if (auto* fiber = go->GetComponent<scene::FiberComponent>()) *fiber = *value;
                else go->AddComponent<scene::FiberComponent>(*value);
                entry.lifetime = m_owner->GetComponentLifetime<scene::FiberComponent>(go->GetID());
            } else go->RemoveComponent<scene::FiberComponent>();
        }
        m_applied = after;
        if (m_context->markSceneDirty) m_context->markSceneDirty();
        return true;
    }
    EditorContext* m_context;
    scene::Scene* m_owner;
    std::vector<Registration> m_entries;
    bool m_applied = true;
};
} /// @note namespace

void RegisterFiberOperators(OperatorRegistry& registry)
{
    EditorOperator add;
    add.id = "fiber.add"; add.label = "Add Grass to Selected Meshes"; add.category = "Fiber";
    add.desc = "選択した Mesh と子 Mesh に草を追加する。個体別ペイントとチャンク配置を作成する。";
    add.kind = OpKind::Mutation; add.undoLabel = "Add Grass";
    add.params = {Parameter("preset",OpParamType::Int,1),Parameter("painted",OpParamType::Bool,false)};
    add.poll = [](const OpContext& c,const OpArgs&) { return Editable(c.ctx) && !c.ctx.projectRoot.empty() && !FiberPaintTargets(c.ctx,false,false).empty(); };
    add.exec = [](OpContext& c,const OpArgs& args) {
        const int preset = args.GetInt("preset",1);
        if (preset < 0 || preset > 2) return OpResult::Err("BAD_PRESET","preset は 0=短い芝、1=草原、2=背の高い草です。");
        std::vector<Registration> entries;
        for (auto* go : FiberPaintTargets(c.ctx,false,false)) {
            Registration entry; entry.node = go->instanceId;
            if (const auto* fiber = go->GetComponent<scene::FiberComponent>()) { entry.before = *fiber; entry.after = *fiber; }
            auto& fiber = entry.after;
            fiber.m_enabled = true; fiber.m_mode = scene::FiberRenderMode::BLADE; fiber.m_chunkedPlacement = true;
            fiber.m_bladeDensity = preset == 0 ? 400.0f : preset == 1 ? 200.0f : 100.0f;
            fiber.m_bladeWidth = preset == 2 ? 0.035f : 0.015f;
            fiber.m_materialPath = preset == 2 ? "Assets/Materials/Fiber/TallGrass.mat" : "Assets/Materials/Fiber/Grass.mat";
            fiber.m_lengthScale = preset == 0 ? 0.35f : 1.0f;
            asset::FiberPaintAsset paint;
            const auto* mesh = go->GetComponent<scene::MeshRenderer>();
            const auto world = go->transform.GetWorldMatrix();
            const float worldScale = std::max({math::Vector3(world.m[0][0],world.m[1][0],world.m[2][0]).Length(),
                math::Vector3(world.m[0][1],world.m[1][1],world.m[2][1]).Length(),
                math::Vector3(world.m[0][2],world.m[1][2],world.m[2][2]).Length(),0.0001f});
            const float cellSize = std::clamp(0.5f/worldScale,0.0001f,16.0f);
            if (!entry.before || !entry.before->m_chunkedPlacement) fiber.m_chunkSize = std::clamp(4.0f/worldScale,0.025f,128.0f);
            if (!renderer::CreateFiberPaintData(*mesh->mesh,cellSize,args.GetBool("painted") ? 0.0f : 1.0f,paint.data))
                return OpResult::Err("INVALID_MESH","CPU 三角形、形状またはペイントの解像度上限を確認してください。");
            if (!entry.before) {
                uint32_t seed = 2166136261u;
                for (unsigned char value : entry.node) seed = (seed^value)*16777619u;
                fiber.m_seed = static_cast<int>(seed&65535u);
            }
            /// @note 大きな Mesh でもプリセット追加直後に予算超過で全体が消えないよう、初期密度へ余裕を持たせる。
            double area = 0;
            for (size_t i = 0; i < mesh->mesh->cpuIndices.size(); i += 3) {
                math::Vector3 points[3];
                for (size_t j = 0; j < 3; ++j) {
                    const auto& local = mesh->mesh->cpuVertices[mesh->mesh->cpuIndices[i+j]].position;
                    const auto point = world*math::Vector4{local.x,local.y,local.z,1}; points[j] = {point.x,point.y,point.z};
                }
                area += static_cast<double>(math::Vector3::Cross(points[1]-points[0],points[2]-points[0]).Length())*0.5;
            }
            if (area > 0 && std::isfinite(area)) fiber.m_bladeDensity = std::min(fiber.m_bladeDensity,
                static_cast<float>(std::max(1.0,static_cast<double>(fiber.m_maxBlades)*0.9/area)));
            /// @note プリセットの変更で既存の塗りを失わない。新しい個体用アセットへ複製して共有編集を避ける。
            if (entry.before && !entry.before->m_paintPath.empty()) {
                const auto* previous = asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(entry.before->m_paintPath));
                if (previous && previous->data.meshSignature == paint.data.meshSignature) paint.data = previous->data;
            }
            const auto relative = "Assets/Fiber/"+asset::AssetDatabase::GenerateGuid()+".fiberpaint";
            const auto disk = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(c.ctx.projectRoot)/relative);
            if (!asset::SaveFiberPaintAssetToFile(disk,paint)) return OpResult::Err("WRITE_FAILED","草のペイントを保存できませんでした。");
            const auto guid = asset::AssetDatabase::GuidFromPath(disk);
            if (guid.empty()) return OpResult::Err("META_FAILED","草は保存済みですが .meta を作成できませんでした: "+disk);
            fiber.m_paintPath = "guid:"+guid+"|"+relative;
            entries.push_back(std::move(entry));
        }
        for (auto& entry : entries) {
            auto* go = c.ctx.activeScene->FindByGuid(entry.node);
            if (auto* fiber = go->GetComponent<scene::FiberComponent>()) *fiber = entry.after;
            else go->AddComponent<scene::FiberComponent>(entry.after);
            entry.lifetime = c.ctx.activeScene->GetComponentLifetime<scene::FiberComponent>(go->GetID());
        }
        c.ctx.requestAssetBrowserRefresh = true;
        if (c.ctx.markSceneDirty) c.ctx.markSceneDirty();
        OpResult result;
        result.command = std::make_unique<FiberRegistrationCommand>(c.ctx,std::move(entries));
        return result;
    };
    registry.Register(std::move(add));

    EditorOperator mode;
    mode.id = "fiber.paint_mode"; mode.label = "Paint Grass"; mode.category = "Fiber";
    mode.desc = "Scene View の草ブラシを切り替える。Shift=消去、[ ]=サイズ、Esc=中断。";
    mode.poll = [](const OpContext& c,const OpArgs&) { return Editable(c.ctx) && !c.ctx.mapEditingMode
        && (c.ctx.fiberPainting || !FiberPaintTargets(c.ctx,false).empty()); };
    mode.checked = [](const OpContext& c,const OpArgs&) { return c.ctx.fiberPainting; };
    mode.exec = [](OpContext& c,const OpArgs&) { c.ctx.fiberPainting = !c.ctx.fiberPainting; c.ctx.clothPinPainting = false; return OpResult::Ok(); };
    registry.Register(std::move(mode));

    EditorOperator commit;
    commit.id = "fiber.commit_stroke"; commit.label = "Commit Grass Stroke"; commit.category = "Fiber";
    commit.desc = "プレビュー済みの草ストロークを一括で確定する。"; commit.kind = OpKind::Mutation; commit.undoLabel = "Paint Grass";
    commit.poll = [](const OpContext& c,const OpArgs&) { return Editable(c.ctx) && c.ctx.fiberPaintPending; };
    commit.exec = [](OpContext& c,const OpArgs&) { return CommitFiberPaintTransaction(c.ctx,*c.ctx.fiberPaintPending); };
    registry.Register(std::move(commit));

    EditorOperator fill;
    fill.id = "fiber.fill"; fill.label = "Fill Grass Surface"; fill.category = "Fiber";
    fill.desc = "草の全面密度を変更する。高さペイントを保持し、複数 Mesh を一括 Undo にする。";
    fill.kind = OpKind::Mutation; fill.undoLabel = "Paint Grass";
    fill.params = {Parameter("density",OpParamType::Float,1.0f),Parameter("all",OpParamType::Bool,false)};
    fill.poll = [](const OpContext& c,const OpArgs&) { return Editable(c.ctx); };
    fill.exec = [](OpContext& c,const OpArgs& args) {
        const float density = args.GetFloat("density",1);
        if (!std::isfinite(density) || density < 0 || density > 1) return OpResult::Err("BAD_DENSITY","density は 0〜1 です。");
        FiberPaintTransaction transaction{c.ctx.activeScene,AssetDirtyRegistry::DiscardEpoch(),{}};
        for (auto* go : FiberPaintTargets(c.ctx,args.GetBool("all"))) {
            const auto* fiber = go->GetComponent<scene::FiberComponent>();
            const auto* source = asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(fiber->m_paintPath));
            if (!source) return OpResult::Err("NO_PAINT","個体別のペイントを作成してください。");
            FiberPaintEdit edit{go->instanceId,fiber->m_paintPath,c.ctx.activeScene->GetComponentLifetime<scene::FiberComponent>(go->GetID()),
                std::make_shared<asset::FiberPaintAsset>(*source),std::make_shared<asset::FiberPaintAsset>(*source)};
            edit.after->data.defaults.density = density;
            for (auto it = edit.after->data.cells.begin(); it != edit.after->data.cells.end();) {
                it->second.density = density;
                if (it->second == edit.after->data.defaults) it = edit.after->data.cells.erase(it); else ++it;
            }
            if (!(edit.before->data == edit.after->data)) transaction.edits.push_back(std::move(edit));
        }
        std::string reason;
        if (!ApplyFiberPaintPreview(c.ctx,transaction.edits,reason)) return OpResult::Err("STALE_STROKE",reason);
        return CommitFiberPaintTransaction(c.ctx,transaction);
    };
    registry.Register(std::move(fill));

    EditorOperator stroke;
    stroke.id = "fiber.stroke"; stroke.label = "Apply Grass Stroke"; stroke.category = "Fiber";
    stroke.desc = "ワールド座標の線分ブラシを適用する。密度・高さ・なじませる操作を一括 Undo にする。";
    stroke.kind = OpKind::Mutation; stroke.undoLabel = "Paint Grass";
    stroke.params = {Parameter("from",OpParamType::Vec3,math::Vector3{}),Parameter("to",OpParamType::Vec3,math::Vector3{}),
        Parameter("radius",OpParamType::Float,2.0f),Parameter("target",OpParamType::Float,1.0f),Parameter("softness",OpParamType::Float,0.35f),
        Parameter("mode",OpParamType::String,std::string("density")),Parameter("all",OpParamType::Bool,false),Parameter("visible",OpParamType::Bool,false)};
    stroke.params[5].enumValues = {"density","length","smooth"};
    stroke.poll = [](const OpContext& c,const OpArgs&) { return Editable(c.ctx); };
    stroke.exec = [](OpContext& c,const OpArgs& args) {
        renderer::FiberPaintStroke value;
        value.from = args.GetVec3("from"); value.to = args.GetVec3("to"); value.radius = args.GetFloat("radius",2);
        value.target = args.GetFloat("target",1); value.softness = args.GetFloat("softness",0.35f);
        const auto mode = args.GetString("mode","density");
        value.mode = mode == "length" ? renderer::FiberPaintMode::LENGTH : mode == "smooth" ? renderer::FiberPaintMode::SMOOTH : renderer::FiberPaintMode::DENSITY;
        FiberPaintTransaction transaction{c.ctx.activeScene,AssetDirtyRegistry::DiscardEpoch(),{}};
        std::string reason;
        if (!PrepareFiberPaintEdits(c.ctx,{value},args.GetBool("all"),args.GetBool("visible"),transaction.edits,reason)
            || !ApplyFiberPaintPreview(c.ctx,transaction.edits,reason)) return OpResult::Err("INVALID_STROKE",reason);
        return CommitFiberPaintTransaction(c.ctx,transaction);
    };
    registry.Register(std::move(stroke));
}
} /// @note namespace fbzz::editor
