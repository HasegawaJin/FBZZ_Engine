/// @file    TerrainOperators.cpp
/// @brief   Terrain データ編集の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: Terrain の columns / rows は heightData と splatData のサイズと一体であり、
/// 数値だけを変更すると TerrainRenderPass と PhysicsSystem が不整合な配列を読む。
/// Inspector は TerrainComponent::Resize() を通して安全に変更しているため、
/// AI からも同じ入口を使い、変更前後の TerrainComponent を丸ごと Undo へ保持する。
/// Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <memory>
#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

scene::GameObject* ResolveTerrainNode(const OpContext& context, const std::string& nodeId)
{
    if (context.ctx.activeScene == nullptr) return nullptr;
    if (nodeId.empty()) return context.ctx.GetSelectedGO();
    return context.ctx.activeScene->FindByGuid(nodeId);
}

bool IsValidTerrainDimension(int value)
{
    // Inspector と同じ上限に揃える。極端な値は Undo スナップショットと GPU 再構築の
    // 両方で巨大な一時確保を起こすため、Operator 側でも先に拒否する。
    return value >= 2 && value <= 4097;
}

void MarkTerrainDirty(scene::TerrainComponent& terrain)
{
    terrain.heightDirty = true;
    terrain.splatDirty = true;
    terrain.colliderDirty = true;
}

std::unique_ptr<ICommand> MakeTerrainResizeCommand(
    EditorContext& context,
    std::string    nodeId,
    scene::TerrainComponent before,
    scene::TerrainComponent after)
{
    EditorContext* editorContext = &context;
    const auto apply = [editorContext, nodeId](const scene::TerrainComponent& snapshot) {
        if (editorContext->activeScene == nullptr) return;
        scene::GameObject* go = editorContext->activeScene->FindByGuid(nodeId);
        if (go == nullptr) return;
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr) return;
        *terrain = snapshot;
        MarkTerrainDirty(*terrain);
        if (editorContext->markSceneDirty) editorContext->markSceneDirty();
    };

    return std::make_unique<LambdaCommand>(
        "AI: Resize Terrain",
        [apply, after = std::move(after)]() { apply(after); },
        [apply, before = std::move(before)]() { apply(before); });
}

} // namespace

void RegisterTerrainOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id        = "terrain.resize";
    op.label     = "Resize Terrain";
    op.category  = "Terrain";
    op.desc      = "Terrain の columns / rows を変更する。既存データは重なる範囲を保持し、"
                   "拡張領域は平坦な layer 0 として初期化する。Inspector と同じ Resize() を通る。";
    op.caution   = "縮小すると範囲外の height / splat データは破棄される (Undo で復元できる)。";
    op.kind      = OpKind::Mutation;
    op.undoLabel = "AI: Resize Terrain";

    OpParam nodeParam;
    nodeParam.name = "node";
    nodeParam.type = OpParamType::NodeId;
    nodeParam.desc = "対象 Terrain を持つ GameObject。省略時は選択中のもの";
    nodeParam.required = false;

    OpParam columnsParam;
    columnsParam.name = "columns";
    columnsParam.type = OpParamType::Int;
    columnsParam.desc = "X 方向の頂点数 (2〜4097)";

    OpParam rowsParam;
    rowsParam.name = "rows";
    rowsParam.type = OpParamType::Int;
    rowsParam.desc = "Z 方向の頂点数 (2〜4097)";

    op.params = { nodeParam, columnsParam, rowsParam };
    op.poll = [](const OpContext& context, const OpArgs& args) {
        scene::GameObject* go = ResolveTerrainNode(context, args.GetString("node"));
        if (go == nullptr || go->GetComponent<scene::TerrainComponent>() == nullptr)
            return false;
        return IsValidTerrainDimension(args.GetInt("columns"))
            && IsValidTerrainDimension(args.GetInt("rows"));
    };

    op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        scene::GameObject* go = ResolveTerrainNode(context, args.GetString("node"));
        if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr)
            return OpResult::Err("NOT_PRESENT", "TerrainComponent が装着されていません");

        const int columns = args.GetInt("columns");
        const int rows = args.GetInt("rows");
        if (!IsValidTerrainDimension(columns) || !IsValidTerrainDimension(rows))
            return OpResult::Err("BAD_ARG", "columns と rows は 2〜4097 で指定してください");
        if (terrain->columns == columns && terrain->rows == rows) {
            OpResult result;
            result.noChange = true;
            result.message = "Terrain のサイズは変わりませんでした";
            return result;
        }

        const scene::TerrainComponent before = *terrain;
        scene::TerrainComponent after = before;
        after.Resize(columns, rows);
        MarkTerrainDirty(after);

        // OperatorRegistry::Invoke は返却コマンドを再実行せず Push だけ行う契約なので、
        // 画面へはここで反映してから、同じ状態を redo 用スナップショットへ渡す。
        *terrain = after;
        if (context.ctx.markSceneDirty) context.ctx.markSceneDirty();

        OpResult result;
        result.command = MakeTerrainResizeCommand(
            context.ctx, go->instanceId, before, std::move(after));
        return result;
    };

    registry.Register(std::move(op));
}

} // namespace fbzz::editor
