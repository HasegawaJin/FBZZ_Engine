/// @file    AnimatorGraphOps.hpp
/// @brief   Animation Graph の「パネルの描画とは独立した操作」を共有実装として切り出す。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 自動整列・保存・dirty 登録は元は AnimationGraphPanel の private メンバーで、パネル描画中しか呼べなかった。整列は共有必須: 間隔が違うと AI が整列したグラフを Editor で整列し直すたび座標だけの無意味な差分が混ざる (BehaviorTreePanel::AutoLayout で既出の問題)。
#pragma once
#include <string>

namespace fbzz::asset { struct AnimatorGraphLayout; }
namespace fbzz::scene { struct AnimatorComponent; }

namespace fbzz::editor {

struct EditorContext;
struct GraphLayout;

/// Editor 側のレイアウトをアセット保存形式へ変換する。
asset::AnimatorGraphLayout ToAssetGraphLayout(const GraphLayout& source);

/// ステートを遷移の深さで列に並べる。
/// 既定ステートを根にするのは、入次数 0 に任せるとどこからも遷移して来ない
/// 孤立ステートまで 1 列目へ並び、開始点が読めなくなるため。
void AutoLayoutAnimatorStates(GraphLayout& layout, const scene::AnimatorComponent& animator);

/// Controller をレイアウトごと保存する。
bool SaveAnimatorControllerWithLayout(EditorContext& ctx,
                                      const std::string& path,
                                      const scene::AnimatorComponent& animator);

/// 開いている Controller ドキュメントを dirty として AssetDirtyRegistry へ登録する。
/// 登録できた (= 対象が .animcontroller だった) とき true。
/// @note 登録が無いと Save All にも終了時確認にも乗らない。AI が編集したときだけ登録されない状態を作らないため、パネルと同じ関数を通す。
bool MarkAnimatorControllerDirty(EditorContext& ctx);

} // namespace fbzz::editor
