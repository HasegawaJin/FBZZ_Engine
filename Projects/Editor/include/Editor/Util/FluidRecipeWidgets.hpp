/// @file    FluidRecipeWidgets.hpp
/// @brief   .fluid レシピの編集欄 (ImGui)。Fluid Editor の Properties と Outliner の追加メニューが使う
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// どれも «レシピを直接書き換えて、書き換えたら true» を返す即時モードの部品。Undo と保存は
/// 呼び手 (FluidDocument::Commit / Edit) の仕事なので、ここはファイルにも UndoStack にも触らない。
/// ID の衝突を避けるため、各関数は自分の中で PushID する (呼び手は部品ごとに PushID(index) しなくてよい)。
#pragma once

#include <Editor/Util/FluidDocument.hpp>
#include <Engine/Asset/FluidRecipe.hpp>

#include <cstddef>
#include <string>

namespace fbzz::editor::fluidui {

struct FluidWidgetContext {
    /// AssetPathField (テクスチャ発生源の画像) が使う。
    std::string projectRoot;
};

/// 種類 (気体 / 液体)・seed・格子やソルバーの設定。
bool EditSimulation(FluidWidgetContext& wc, asset::FluidRecipe& recipe);
/// 部品 1 つの中身 (見出し・有効チェック・並べ替えボタンは含まない — Outliner が持つ)。
bool EditSource(FluidWidgetContext& wc, asset::FluidRecipe& recipe, int index);
bool EditForce(FluidWidgetContext& wc, asset::FluidRecipe& recipe, int index);
bool EditCollider(FluidWidgetContext& wc, asset::FluidRecipe& recipe, int index);
/// 見た目 (Shading・色・Ramp・細部・炎・液面)。
bool EditLook(FluidWidgetContext& wc, asset::FluidRecipe& recipe);
/// 出力 (コマ・長さ・ループ・MV・.vfield)。
bool EditOutput(FluidWidgetContext& wc, asset::FluidRecipe& recipe);
/// 焼き方 ([bake])。
bool EditBake(FluidWidgetContext& wc, asset::FluidRecipe& recipe);

/// 「Add ...」のポップアップの中身 (BeginPopup の内側で呼ぶ)。項目が選ばれたら部品を末尾に足し、
/// outNewIndex に添字を入れて true。上限に達していれば項目を無効で描く。
bool AddSourceMenuItems(asset::FluidRecipe& recipe, int& outNewIndex);
bool AddForceMenuItems(asset::FluidRecipe& recipe, int& outNewIndex);
bool AddColliderMenuItems(asset::FluidRecipe& recipe, int& outNewIndex);

/// Outliner に出す部品の名前 (name が空なら "Source 2 (Cone)" の形)。
[[nodiscard]] std::string PartDisplayName(const asset::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// 部品の enabled への参照 (Outliner の有効チェック)。list / index が不正なら nullptr。
[[nodiscard]] bool* PartEnabled(asset::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// 部品の数 (Source / Force / Collider)。
[[nodiscard]] int PartCount(const asset::FluidRecipe& recipe, FluidSelectionKind list);
/// 部品の複製・削除・移動 (添字は呼び手が選択へ反映する)。成功したら true。
bool DuplicatePart(asset::FluidRecipe& recipe, FluidSelectionKind list, int index);
bool RemovePart(asset::FluidRecipe& recipe, FluidSelectionKind list, int index);
bool MovePart(asset::FluidRecipe& recipe, FluidSelectionKind list, int from, int to);

} // namespace fbzz::editor::fluidui
