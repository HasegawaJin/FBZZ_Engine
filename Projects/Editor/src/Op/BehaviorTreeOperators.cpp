/// @file    BehaviorTreeOperators.cpp
/// @brief   Behavior Tree の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY ここに全 bt_* を並べないか:
/// bt_* の AI ツールは 29 個あるが、その大半は
/// bt.node.setField(path, nodeId, field, value) のように
/// 「パスと id と名前を指定して値を書く」API で、コマンドパレットから
/// 呼びたい場面が存在しない。Operator にすると引数宣言が増えるだけで、
/// 人が使う面には現れないまま層が 1 つ深くなる。
///
/// Operator にする価値があるのは
/// - 引数が無い / ほとんど無い
/// - 「今開いているドキュメント」に対して人も実行したい
/// もの。ここではそれだけを登録する。実装の共有 (本題) は
/// Editor/GraphEditor/BehaviorTreeOps.hpp が担っており、
/// パネルと AI ハンドラの両方がそこを通る。
/// Docs/design/editor-operator-model.md §6 Step 5
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>

#include <memory>
#include <string>
#include <utility>

namespace fbzz::editor {

void RegisterBehaviorTreeOperators(OperatorRegistry& registry)
{
    // Behavior Tree パネルが開いているドキュメントに対して働く。
    // WHY パスを引数に取らないか: レイアウトも dirty 状態も「今開いている木」を
    //     単位にしており、閉じたファイルへ直接書くとパネルが保持している
    //     編集中モデルと食い違う 2 つの真実ができる (Animation と同じ理由)。
    const auto hasOpenTree = [](const OpContext& c, const OpArgs&) {
        return !c.ctx.behaviorTreeEditorPath.empty();
    };

    EditorOperator op;
    op.id        = "bt.auto_layout";
    op.label     = "Auto Layout Behavior Tree";
    op.category  = "Behavior Tree";
    op.desc      = "開いている Behavior Tree のノードを、深さ順の列に並べ直す。"
                   "AI の bt_auto_layout と同一実装なので、どちらで整列しても座標は一致する。";
    op.caution   = "既存のノード配置は失われる。";
    op.kind      = OpKind::Action;   // 座標は .behaviortree 側の情報で、Undo はパネルが持つ
    op.poll      = hasOpenTree;
    op.exec      = [](OpContext& c, const OpArgs&) -> OpResult {
        // 実体はパネルが持つドキュメントなので、パネルへ要求を渡す。
        // WHY 直接 asset を触らないか: パネルは Undo スタックとキャンバスの
        //     選択状態を自分で持っており、外から中身だけ書き換えると
        //     整列前へ戻せなくなる (PushUndo を通らない)。
        c.ctx.requestBehaviorTreeAutoLayout = true;
        return OpResult::Ok();
    };
    registry.Register(std::move(op));
}

} // namespace fbzz::editor
