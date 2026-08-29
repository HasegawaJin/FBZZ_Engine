/// @file    PrefabInstantiate.hpp
/// @brief   .prefab / .vfx を GameObject 階層として展開する読み取り専用の生成経路。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY Engine に置くか:
///   生成経路は長らく Editor (PrefabSerializer) にしか無く、スタンドアロン実行では
///   Script::InstantiatePrefab のコールバックが誰にも設定されないため、
///   PrefabPool::Spawn が必ず失敗していた。プレファブをゲーム本体で使う以上、
///   «読んで展開する» 側は Engine の持ち物でなければならない。
///   保存・Apply・Revert・差分算出はオーサリング操作なので Editor に残る。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

class Scene;

/// プレファブ定義とインスタンスの差分 1 件。
/// path は GO テーブル内のドット区切りパス (例 "transform.position",
/// "LightComponent.intensity")。
struct PrefabOverride {
    std::string prefabSourceId; ///< プレファブ側 GO の instanceId (対応キー)
    std::string instanceGuid;   ///< インスタンス側 GO の instanceId
    std::string objectName;     ///< 表示用 (インスタンス側の名前)
    std::string path;           ///< 差分のあったプロパティパス
    std::string prefabValue;    ///< 表示用の元値
    std::string instanceValue;  ///< 表示用の現在値
};

/// 1 インスタンス階層ぶんの差分と、パッチ適用に必要な現在値のスナップショット。
struct PrefabOverrideSet {
    std::string prefabAssetPath; ///< Assets 起点の相対パス
    std::vector<PrefabOverride> entries;
    /// prefabSourceId → そのインスタンス GO の現在のシリアライズ表現。
    /// entries は「どのパスが違うか」しか持たないため、値の書き戻しはここから引く。
    std::unordered_map<std::string, toml::table> instanceTables;

    [[nodiscard]] bool Empty() const { return entries.empty(); }
};

/// ドット区切りパスで node を引く。見つからなければ nullptr。
[[nodiscard]] const toml::node* FindNodeAtPath(const toml::table& table, const std::string& path);

/// ドット区切りパスへ node を書き込む。途中のテーブルが無い場合は作らず false を返す。
bool SetNodeAtPath(toml::table& table, const std::string& path, const toml::node& value);

/// プレファブアセットを展開してシーンへ追加する。
///
/// overrides を渡すと、定義を展開したあと該当プロパティだけをその値で上書きしてから
/// 生成する (インスタンス側の個別調整を保ったまま作り直す用)。
/// path は絶対パスまたはプロジェクトルートからのパス。拡張子が無ければ .prefab を補う。
bool InstantiatePrefabAsset(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr);

} // namespace fbzz::scene
