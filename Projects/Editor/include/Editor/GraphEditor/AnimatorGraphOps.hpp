/// @file    AnimatorGraphOps.hpp
/// @brief   WHAT: Animation Graph の「パネルの描画とは独立した操作」を共有実装として切り出す。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY:  自動整列・保存・dirty 登録は AnimationGraphPanel の private メンバーの中にあり、
/// パネルを描画していないと呼べなかった。そのため AI 側は Animator の構造
/// (state / transition / motion / parameter) を編集できるのに、
/// - **保存する手段が無く**、編集は次回起動で消える
/// - キャンバス整列ができず、AI が足したステートが既存ノードと重なったまま残る
/// という状態だった。
///
/// 別実装を書いてはならない — 整列の間隔が違えば「AI が整列したグラフを
/// Editor で整列し直すと座標が動く」ことになり、差分に意味のない座標変更が
/// 毎回混ざる (BehaviorTreePanel::AutoLayout で既に踏んだのと同じ問題)。
#pragma once
#include <string>

namespace fbzz::asset { struct AnimatorGraphLayout; }
namespace fbzz::scene { struct AnimatorComponent; }

namespace fbzz::editor {

struct EditorContext;
struct GraphLayout;

// Editor 側のレイアウトをアセット保存形式へ変換する。
asset::AnimatorGraphLayout ToAssetGraphLayout(const GraphLayout& source);

// ステートを遷移の深さで列に並べる。
// 既定ステートを根にするのは、入次数 0 に任せるとどこからも遷移して来ない
// 孤立ステートまで 1 列目へ並び、開始点が読めなくなるため。
void AutoLayoutAnimatorStates(GraphLayout& layout, const scene::AnimatorComponent& animator);

// Controller をレイアウトごと保存する。
bool SaveAnimatorControllerWithLayout(EditorContext& ctx,
                                      const std::string& path,
                                      const scene::AnimatorComponent& animator);

// 開いている Controller ドキュメントを dirty として AssetDirtyRegistry へ登録する。
// 登録できた (= 対象が .animcontroller だった) とき true。
// WHY: 登録が無いと Save All にも終了時確認にも乗らない。AI が編集したときだけ
//      登録されない、という状態を作らないため、パネルと同じ関数を通す。
bool MarkAnimatorControllerDirty(EditorContext& ctx);

} // namespace fbzz::editor
