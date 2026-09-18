/// @file    FluidRecipeWidgets.hpp
/// @brief   .fluid レシピの編集欄 (ImGui)。Fluid Editor の Properties と Outliner の追加メニューが使う
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @brief どれも «レシピを直接書き換えて、書き換えたら true» を返す即時モードの部品。Undo と保存は
/// @brief 呼び手 (FluidDocument::Commit / Edit) の仕事なので、ここはファイルにも UndoStack にも触らない。
/// @brief ID の衝突を避けるため、各関数は自分の中で PushID する (呼び手は部品ごとに PushID(index) しなくてよい)。
#pragma once

#include <Editor/Util/FluidDocument.hpp>
#include <Fluid/FluidRecipe.hpp>

#include <cstddef>
#include <string>

namespace fbzz::editor::fluidui {

struct FluidWidgetContext {
    /// @brief AssetPathField (テクスチャ発生源の画像) が使う。
    std::string projectRoot;
};

/// @brief 種類 (気体 / 液体)・seed・格子やソルバーの設定。
bool EditSimulation(FluidWidgetContext& wc, fluid::FluidRecipe& recipe);
/// @brief 部品 1 つの中身 (見出し・有効チェック・並べ替えボタンは含まない — Outliner が持つ)。
bool EditSource(FluidWidgetContext& wc, fluid::FluidRecipe& recipe, int index);
bool EditForce(FluidWidgetContext& wc, fluid::FluidRecipe& recipe, int index);
bool EditCollider(FluidWidgetContext& wc, fluid::FluidRecipe& recipe, int index);
/// @brief 見た目 (Shading・色・Ramp・細部・炎・液面)。
bool EditLook(FluidWidgetContext& wc, fluid::FluidRecipe& recipe);
/// @brief 出力 (コマ・長さ・ループ・MV・速度場 PNG)。
bool EditOutput(FluidWidgetContext& wc, fluid::FluidRecipe& recipe);
/// @brief 焼き方 ([bake])。
bool EditBake(FluidWidgetContext& wc, fluid::FluidRecipe& recipe);

/// @brief 「Add ...」のポップアップの中身 (BeginPopup の内側で呼ぶ)。項目が選ばれたら部品を末尾に足し、
/// @brief outNewIndex に添字を入れて true。上限に達していれば項目を無効で描く。
bool AddSourceMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex);
bool AddForceMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex);
bool AddColliderMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex);

/// @brief Outliner に出す部品の名前 (name が空なら "Source 2 (Cone)" の形)。
[[nodiscard]] std::string PartDisplayName(const fluid::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// @brief 部品の enabled への参照 (Outliner の有効チェック)。list / index が不正なら nullptr。
[[nodiscard]] bool* PartEnabled(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// @brief 部品の数 (Source / Force / Collider)。
[[nodiscard]] int PartCount(const fluid::FluidRecipe& recipe, FluidSelectionKind list);
/// @brief 部品の複製・削除・移動 (添字は呼び手が選択へ反映する)。成功したら true。
bool DuplicatePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index);
bool RemovePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index);
bool MovePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int from, int to);

} // namespace fbzz::editor::fluidui
