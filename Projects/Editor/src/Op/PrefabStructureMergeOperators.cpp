/// @file    PrefabStructureMergeOperators.cpp
/// @brief   Shared operator entry points for reviewed Prefab hierarchy updates.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Editor/Util/PrefabStructureMerge.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/Uuid.hpp>
#include <memory>
#include <utility>

namespace fbzz::editor {
namespace {
struct Review {
    PrefabStructureMergePlan plan;
    std::string receipt;
    scene::Scene* scene = nullptr;
    std::uint64_t generation = 0;
};

bool CanReview(const OpContext& context, const OpArgs&)
{
    return context.ctx.activeScene && (!context.ctx.playMode || context.ctx.playMode->IsInEditor())
        && !context.ctx.InPrefabEditMode();
}

OpData ReviewData(const Review& review)
{
    OpData data = OpData::MakeObject();
    data.Set("receipt", review.receipt);
    data.Set("rootGuid", review.plan.rootGuid);
    data.Set("canApply", review.plan.merge.CanApply());
    OpData changes = OpData::MakeArray(), conflicts = OpData::MakeArray();
    for (const auto& change : review.plan.merge.changes) {
        OpData row = OpData::MakeObject();
        row.Set("sourceId", change.sourceId);
        row.Set("action", change.action);
        row.Set("beforeParent", change.beforeParent);
        row.Set("afterParent", change.afterParent);
        changes.Push(std::move(row));
    }
    for (const auto& conflict : review.plan.merge.conflicts) {
        OpData row = OpData::MakeObject();
        row.Set("sourceId", conflict.sourceId);
        row.Set("path", conflict.path);
        row.Set("reason", conflict.reason);
        conflicts.Push(std::move(row));
    }
    data.Set("changes", std::move(changes));
    data.Set("conflicts", std::move(conflicts));
    return data;
}
} /// @note namespace

void RegisterPrefabStructureMergeOperators(OperatorRegistry& registry)
{
    auto review = std::make_shared<Review>();
    EditorOperator preview;
    preview.id = "prefab.structure.preview";
    preview.label = "Preview Prefab Structure Update";
    preview.category = "Prefab";
    preview.desc = "Compare this instance, its saved baseline and current template; report complete structural changes and conflicts.";
    preview.kind = OpKind::Query;
    OpParam root;
    root.name = "root";
    root.type = OpParamType::NodeId;
    root.desc = "Connected Prefab root instance GUID";
    preview.params = {root};
    preview.poll = CanReview;
    preview.exec = [review](OpContext& context, const OpArgs& args) -> OpResult {
        *review = {};
        auto* scene = context.ctx.activeScene;
        auto* object = scene ? scene->FindByGuid(args.GetString("root")) : nullptr;
        if (!object) return OpResult::Err("NO_ROOT", "The requested Prefab root does not exist");
        review->scene = scene;
        review->generation = scene->GetRenderSceneGeneration();
        PreviewPrefabStructureMerge(*scene, object->GetID(), context.ctx.projectRoot, review->plan);
        review->receipt = util::GenerateUUID();
        return OpResult::Data(ReviewData(*review));
    };
    registry.Register(std::move(preview));
    EditorOperator apply;
    apply.id = "prefab.structure.apply";
    apply.label = "Apply Prefab Structure Update";
    apply.category = "Prefab";
    apply.desc = "Apply the exact reviewed update to one instance with checked Undo/Redo; source assets are not written.";
    apply.kind = OpKind::Mutation;
    apply.undoLabel = "Merge Prefab Structure";
    OpParam receipt;
    receipt.name = "receipt";
    receipt.type = OpParamType::String;
    receipt.desc = "Receipt returned by the most recent prefab.structure.preview";
    apply.params = {receipt};
    apply.poll = CanReview;
    apply.exec = [review](OpContext& context, const OpArgs& args) -> OpResult {
        if (!context.undo.IsRecordingEnabled()) return OpResult::Err("NO_HISTORY", "Enable Edit Undo before applying a reviewed Prefab update");
        if (review->receipt.empty() || args.GetString("receipt") != review->receipt
            || context.ctx.activeScene != review->scene
            || review->scene->GetRenderSceneGeneration() != review->generation)
            return OpResult::Err("STALE_REVIEW", "Preview this Prefab instance again");
        if (!review->plan.merge.CanApply()) return OpResult::Err("PREFAB_CONFLICT", "The review has unresolved conflicts");
        auto command = MakePrefabStructureMergeCommand(context.ctx, review->plan);
        std::string reason;
        if (!command->TryExecute(reason)) return OpResult::Err("PREFAB_NOT_APPLIED", reason);
        OpResult result;
        result.command = std::move(command);
        result.data.Set("rootGuid", review->plan.rootGuid);
        *review = {};
        return result;
    };
    registry.Register(std::move(apply));
}
} /// @note namespace fbzz::editor
