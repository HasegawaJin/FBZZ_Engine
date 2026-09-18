/// @file    BehaviorTreeOperators.cpp
/// @brief   Behavior Tree の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// bt_* の AI ツール (29 個) は id 指定で値を書く API が大半で、コマンドパレットから呼ぶ場面が無い。
/// 引数がほとんど無く「今開いているドキュメント」に人も実行したいものだけを Operator として登録する。
/// 実装は Editor/GraphEditor/BehaviorTreeOps.hpp (パネルと AI ハンドラ共通) が担う。
/// @see Docs/design/editor-operator-model.md §6 Step 5
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>

#include <memory>
#include <string>
#include <utility>

namespace fbzz::editor {

void RegisterBehaviorTreeOperators(OperatorRegistry& registry)
{
    /// @note Behavior Tree パネルが開いているドキュメントに対して働く。レイアウトも dirty 状態も
    ///       「今開いている木」単位で、閉じたファイルへ直接書くとパネルの編集中モデルと食い違う
    ///       2 つの真実ができる (Animation と同じ理由)。
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
    /// @note 座標は .behaviortree 側の情報で、Undo はパネルが持つ
    op.kind      = OpKind::Action;
    op.poll      = hasOpenTree;
    op.exec      = [](OpContext& c, const OpArgs&) -> OpResult {
        /// @note 実体はパネルが持つドキュメントなので、要求を渡すだけにする。パネルは Undo スタックと
        ///       選択状態を自分で持ち、外から中身だけ書き換えると整列前へ戻せなくなる (PushUndo を通らない)。
        c.ctx.requestBehaviorTreeAutoLayout = true;
        return OpResult::Ok();
    };
    registry.Register(std::move(op));
}

} // namespace fbzz::editor
