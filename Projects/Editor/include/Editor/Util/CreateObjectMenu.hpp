/// @file    CreateObjectMenu.hpp
/// @brief   GameObject の Create メニュー。Hierarchy とメインメニューの GameObject が共有する投影。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// 項目は node.create_preset / prefab.instantiate / node.create_script_object の各 Operator を
/// 呼ぶだけで、生成の中身は持たない。
/// @see Docs/design/editor-operator-model.md
#pragma once

#include <Editor/Op/EditorOperator.hpp>

#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

/// @brief メニューで選ばれた生成。描画中にシーンを変えないよう、呼び出し側が後で実行する。
struct PendingObjectCreate {
    std::string operatorId;
    OpArgs      args;
    /// @brief 同じ Operator を続けて呼ぶ残りの引数 (複数アセットのドロップ)。生成物はまとめて選択する。
    std::vector<OpArgs> moreArgs;
    /// @brief 1 つだけ作れたら Hierarchy のインラインリネームを始める。メニューからの生成だけ true。
    bool        beginRename = true;

    [[nodiscard]] bool IsSet() const { return !operatorId.empty(); }
};

/// @brief Create の中身 (プリセット・Prefab・Script Object) を描く。
/// @param parentGuid 空ならルートに置く。
/// @param out 選ばれた項目。何も選ばれなければ変更しない。
void DrawCreateObjectMenuItems(EditorContext& ctx, const std::string& parentGuid, PendingObjectCreate& out);

/// @brief 選択 (最後に選んだもの) を既定の親にした Create メニュー一式。
/// @note 親があるときは生成先の名前と "Create at Root" を併せて出す。
void DrawCreateObjectMenu(EditorContext& ctx, PendingObjectCreate& out);

/// @brief pending を実行して空に戻す。
/// @return Operator の結果。pending が空なら ok かつ noChange。
OpResult InvokePendingObjectCreate(EditorContext& ctx, PendingObjectCreate& pending);

} // namespace fbzz::editor
