/// @file    PrefabOverrides.hpp
/// @brief   プレファブインスタンスの「プロパティ単位の差分 (override)」の算出・保持・適用。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: これまでプレファブは「インスタンス化した瞬間の複製」でしかなく、
/// インスタンス側で何を変えたかを system が把握していなかった。そのため
/// - Inspector で「このインスタンスのどこが元と違うか」が分からない
/// - Apply でアセットを更新すると、他インスタンスの個別調整が巻き戻る
/// という 2 つの問題があった。
///
/// インスタンスとプレファブの対応は GameObject::prefabSourceId が持つ。
/// 差分は「シリアライズ済み TOML の値比較」で取る。ランタイム構造体を型ごとに
/// 比較するのではなくシリアライズ表現で比べるのは、
/// 1. 保存されない一時状態 (GPU ハンドル等) を自動的に無視できる
/// 2. コンポーネントが増えても差分ロジックを書き足す必要がない
/// ため。Reflect() を持たない手書きシリアライズのコンポーネントにも等しく効く。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

// 差分の表現とパス操作は Engine が持つ (生成経路が差分を当てる側なので、
// 型を Editor に置くと Engine から参照できない)。ここでは名前だけ引き継ぐ。
using PrefabOverride = scene::PrefabOverride;
using PrefabOverrideSet = scene::PrefabOverrideSet;
using scene::FindNodeAtPath;
using scene::SetNodeAtPath;

// rootEntity のプレファブインスタンス階層について、アセット定義との差分を算出する。
// rootEntity が prefabAssetPath を持たない、またはアセットを読めない場合は false。
bool ComputePrefabOverrides(scene::Scene& scene,
                            scene::EntityID rootEntity,
                            const std::string& projectRoot,
                            PrefabOverrideSet& out);

// 指定パスの差分だけを除外した override 集合を返す (「この 1 件だけ元に戻す」用)。
[[nodiscard]] PrefabOverrideSet WithoutEntry(const PrefabOverrideSet& set,
                                             const PrefabOverride& removed);

// UI 表示用に node を 1 行の文字列へ整形する。
[[nodiscard]] std::string FormatNodeForDisplay(const toml::node* node);

} // namespace fbzz::editor
