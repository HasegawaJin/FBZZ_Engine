/// @file    PrefabOverrides.hpp
/// @brief   プレファブインスタンスの「プロパティ単位の差分 (override)」の算出・保持・適用。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note 対応は GameObject::prefabSourceId が持つ。差分はランタイム構造体でなく「シリアライズ済み
/// @note TOML の値比較」で取り、一時状態 (GPU ハンドル等) を自動的に無視しつつコンポーネント増加にも
/// @note 差分ロジックの追加なしで対応する。Reflect() の無い手書きシリアライズにも等しく効く。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

/// @note 差分の表現とパス操作は Engine が持つ (生成経路が差分を当てる側なので、
/// @note 型を Editor に置くと Engine から参照できない)。ここでは名前だけ引き継ぐ。
using PrefabOverride = scene::PrefabOverride;
using PrefabOverrideSet = scene::PrefabOverrideSet;
using scene::FindNodeAtPath;
using scene::SetNodeAtPath;

/// @brief 最後に適用した基準定義とインスタンスを比較して差分を採る。
/// @return 出所・基準定義・シーンを読めなければ false。
/// @note 基準定義の無い旧シーンだけ現在のアセットを使い、既存の個別変更を保護する。
/// @note 子の追加・削除・親変更は hasStructuralOverrides に記録し、個別調整を保持する更新を拒否する。
/// @see Docs/design/prefab-safety.md
bool ComputePrefabOverrides(scene::Scene& scene,
                            scene::EntityID rootEntity,
                            const std::string& projectRoot,
                            PrefabOverrideSet& out);

/// @note 指定パスの差分だけを除外した override 集合を返す (「この 1 件だけ元に戻す」用)。
[[nodiscard]] PrefabOverrideSet WithoutEntry(const PrefabOverrideSet& set,
                                             const PrefabOverride& removed);

/// @note UI 表示用に node を 1 行の文字列へ整形する。
[[nodiscard]] std::string FormatNodeForDisplay(const toml::node* node);

} /// @note namespace fbzz::editor
