/// @file    ComponentActionOperators.cpp
/// @brief   Inspector固有の再構築・ベイク要求・Controller生成を公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ComponentDefaults.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
namespace fbzz::editor {
void RegisterDiagnosticOperators(OperatorRegistry& registry);
void RegisterIntegrationOperators(OperatorRegistry& registry);
void RegisterHistoryOperators(OperatorRegistry& registry);
void RegisterFlipbookBakeOperators(OperatorRegistry& registry);
void RegisterVectorFieldOperators(OperatorRegistry& registry);
namespace {
scene::GameObject* Resolve(OpContext& context, const OpArgs& args)
{
    const auto node = args.GetString("node");
    return context.ctx.activeScene ? (node.empty() ? context.ctx.GetSelectedGO() : context.ctx.activeScene->FindByGuid(node)) : nullptr;
}
OpParam Node() { OpParam p; p.name = "node"; p.type = OpParamType::NodeId; p.required = false; return p; }
} /// @note namespace
void RegisterComponentActionOperators(OperatorRegistry& registry)
{
    RegisterDiagnosticOperators(registry);
    RegisterIntegrationOperators(registry);
    RegisterHistoryOperators(registry);
    RegisterFlipbookBakeOperators(registry);
    RegisterVectorFieldOperators(registry);
    {
        EditorOperator op; op.id = "component.collider.rebuild"; op.label = op.id; op.category = "Physics"; op.kind = OpKind::Action;
        op.desc = "MeshCollider/ConvexHullColliderのCPU形状をRendererまたはモデルから再構築する。失敗時は既存形状を保持する。";
        op.params.push_back(Node()); OpParam p; p.name = "component"; p.enumValues = {"MeshCollider", "ConvexHullCollider"}; op.params.push_back(p);
        p = {}; p.name = "source"; p.enumValues = {"renderer", "model"}; op.params.push_back(p);
        op.exec = [](OpContext& context, const OpArgs& args) {
            auto* go = Resolve(context, args); if (!go) return OpResult::Err("NO_NODE", "GameObjectが見つかりません。");
            const auto run = [&](auto* collider) -> OpResult {
                if (!collider) return OpResult::Err("NOT_PRESENT", "Colliderがありません。");
                auto path = collider->meshPath; auto index = collider->meshIndex;
                const auto* mesh = args.GetString("source") == "renderer" ? SourceMeshFromGameObject(*go, path, index) : MeshFromModelPath(path, index);
                bool built = false;
                using T = std::remove_pointer_t<decltype(collider)>;
                if constexpr (std::is_same_v<T, scene::MeshColliderComponent>) built = BuildMeshCollider(*collider, mesh);
                else built = BuildConvexHullCollider(*collider, mesh);
                if (!built) return OpResult::Err("NO_MESH_DATA", "CPUメッシュがありません。変換結果とmeshIndexを確認してください。");
                collider->meshPath = path; collider->meshIndex = index;
                scene::UpdateColliderPose(*go, *collider, collider->useTransformScale);
                if (context.ctx.markSceneDirty) context.ctx.markSceneDirty();
                OpData data = OpData::MakeObject(); data.Set("meshPath", OpData(path)); data.Set("meshIndex", OpData(index)); data.Set("rebuilt", OpData(true));
                return OpResult::Data(std::move(data));
            };
            if (args.GetString("component") == "MeshCollider") return run(go->GetComponent<scene::MeshColliderComponent>());
            return run(go->GetComponent<scene::ConvexHullColliderComponent>());
        }; registry.Register(std::move(op));
    }
    for (const bool bake : {false, true}) {
        EditorOperator op; op.id = bake ? "component.light_probe.bake" : "component.light_probe.inspect"; op.label = op.id; op.category = "Lighting";
        op.kind = bake ? OpKind::Action : OpKind::Query; op.params.push_back(Node());
        op.desc = "LightProbeVolumeのベイク要求と進捗。bakeは要求を積み、inspectのqueued/baking/readyで描画後の状態を確認する。";
        op.exec = [bake](OpContext& context, const OpArgs& args) {
            auto* go = Resolve(context, args); auto* probes = go ? go->GetComponent<scene::LightProbeVolumeComponent>() : nullptr;
            if (!probes) return OpResult::Err("NOT_PRESENT", "LightProbeVolumeがありません。");
            if (bake) { if (!context.ctx.renderer) return OpResult::Err("NO_RENDERER", "描画デバイスがありません。"); probes->bakeRequested = true; }
            OpData data = OpData::MakeObject(); data.Set("queued", OpData(probes->bakeRequested)); data.Set("baking", OpData(probes->runtimeBaking));
            data.Set("ready", OpData(probes->runtimeReady)); data.Set("cursor", OpData(probes->runtimeCursor)); data.Set("pass", OpData(probes->runtimePass)); data.Set("probeCount", OpData(probes->ProbeCount()));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "component.animator.create_controller"; op.label = op.id; op.category = "Animation"; op.kind = OpKind::Mutation;
        op.desc = "現在のAnimatorからControllerをAssets内に生成して接続する。既存ファイルは上書きしない。Undoは接続を戻す。";
        op.params.push_back(Node()); OpParam path; path.name = "path"; op.params.push_back(path);
        op.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; auto* go = Resolve(context, args); auto* animator = go ? go->GetComponent<scene::AnimatorComponent>() : nullptr;
            if (!animator) return OpResult::Err("NOT_PRESENT", "Animatorがありません。");
            if (ctx.projectRoot.empty()) return OpResult::Err("NO_PROJECT", "プロジェクトがありません。");
            const auto root = util::FileSystem::PathFromUtf8(ctx.projectRoot); auto file = util::FileSystem::PathFromUtf8(args.GetString("path"));
            if (file.is_relative()) file = root / file;
            std::error_code error; file = std::filesystem::weakly_canonical(file, error); const auto assets = std::filesystem::weakly_canonical(root / "Assets", error);
            const auto relative = file.lexically_relative(assets);
            if (error || file.extension() != ".animcontroller" || relative.empty() || relative.is_absolute() || *relative.begin() == ".."
                || util::FileSystem::Exists(file) || util::FileSystem::Exists(file.string() + ".meta")) return OpResult::Err("BAD_PATH", "Assets内の未使用.animcontrollerを指定してください。");
            if (!util::FileSystem::EnsureDirectory(file.parent_path())) return OpResult::Err("SAVE_FAILED", "フォルダを作成できません。");
            const auto output = util::FileSystem::PathToUtf8(file); const auto controller = asset::MakeAnimatorControllerAsset(*animator);
            if (!asset::SaveAnimatorControllerAsset(output, controller)) return OpResult::Err("SAVE_FAILED", "Controllerを保存できません。");
            const std::string previous = animator->controllerPath, next = "guid:" + asset::AssetDatabase::GuidFromPath(output), id = go->instanceId, project = ctx.projectRoot;
            const auto apply = [&ctx, id, project](const std::string& expected, const std::string& value, std::string& reason) {
                auto* target = ctx.projectRoot == project && ctx.activeScene ? ctx.activeScene->FindByGuid(id) : nullptr;
                auto* current = target ? target->GetComponent<scene::AnimatorComponent>() : nullptr;
                if (!current || current->controllerPath != expected) { reason = "Animator changed."; return false; }
                current->controllerPath = value; current->loadedControllerPath.clear(); if (ctx.markSceneDirty) ctx.markSceneDirty(); return true;
            };
            animator->controllerPath = next; animator->loadedControllerPath = next;
            if (ctx.markSceneDirty) ctx.markSceneDirty(); ctx.requestAssetBrowserRefresh = true;
            OpResult result = OpResult::Ok(); result.command = std::make_unique<CheckedLambdaCommand>("Create Animator Controller",
                [apply, previous, next](std::string& reason) { return apply(previous, next, reason); },
                [apply, previous, next](std::string& reason) { return apply(next, previous, reason); });
            result.data = OpData::MakeObject(); result.data.Set("path", OpData(output)); result.data.Set("reference", OpData(next)); return result;
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
