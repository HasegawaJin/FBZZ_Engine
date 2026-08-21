// FBZZ Engine
// NodeOperators.cpp | fbzz::editor
// GameObject そのものを対象にする Operator
//
// WHY: リネームは Hierarchy パネルと AI (node.rename) の両方が実装を持っていた。
//      しかも中身が違い、パネル側は**名前を 1 つ変えるためにシーン全体を 2 回
//      TOML シリアライズ**していた (ExecuteSceneEditWithUndo 経由)。
//      同じ操作なのに Undo の重さと履歴ラベルが経路で違う状態で、
//      大きなシーンではパネルからのリネームだけが目に見えて重かった。
//      Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

scene::GameObject* ResolveNode(const OpContext& c, const std::string& nodeId)
{
    if (c.ctx.activeScene == nullptr) return nullptr;
    if (nodeId.empty()) return c.ctx.GetSelectedGO();
    return c.ctx.activeScene->FindByGuid(nodeId);
}

} // namespace

void RegisterNodeOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id        = "node.rename";
    op.label     = "Rename Node";
    op.category  = "Edit";
    op.desc      = "GameObject の名前を変更する。node を省略すると選択中のものが対象。"
                   "Hierarchy のインライン編集と同じ実体を通る。";
    op.kind      = OpKind::Mutation;
    op.undoLabel = "Rename GameObject";

    OpParam nodeParam;
    nodeParam.name     = "node";
    nodeParam.type     = OpParamType::NodeId;
    nodeParam.desc     = "対象の GameObject。省略すると選択中のもの";
    nodeParam.required = false;

    OpParam nameParam;
    nameParam.name = "name";
    nameParam.type = OpParamType::String;
    nameParam.desc = "新しい名前 (空は不可)";
    op.params = { nodeParam, nameParam };

    // 対象が引けること。名前が同じかどうかは exec 側で見る
    // (メニューから引数なしで問い合わせたときに常に無効化されてしまうため)。
    op.poll = [](const OpContext& c, const OpArgs& args) {
        return ResolveNode(c, args.GetString("node")) != nullptr;
    };

    op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        scene::GameObject* go = ResolveNode(c, args.GetString("node"));
        if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

        const std::string newName = args.GetString("name");
        if (newName.empty()) return OpResult::Err("BAD_ARG", "name が空です");

        OpResult result;
        // UI へは即座に反映してから Push する (レジストリは Execute を呼ばない契約)。
        result.command = MakeRenameNodeCommand(c.ctx, go->GetID(), newName,
                                               "Rename GameObject", /*applyNow=*/true);
        // 名前が変わらなかった場合は履歴を汚さない。エラーでもない。
        if (!result.command) {
            result.noChange = true;
            result.message  = "名前は変わりませんでした";
        }
        return result;
    };

    registry.Register(std::move(op));
}

} // namespace fbzz::editor
