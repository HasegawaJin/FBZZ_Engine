/// @file    PrefabInstantiate.hpp
/// @brief   .prefab / .vfx を GameObject 階層として展開する読み取り専用の生成経路。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 生成経路はもと Editor (PrefabSerializer) にしか無く、スタンドアロンでは
///       Script::InstantiatePrefab のコールバック未設定で PrefabPool::Spawn が必ず失敗していた。
/// @note 保存・Apply・Revert・差分算出はオーサリング操作なので Editor に残る。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief プレファブ定義とインスタンスの差分 1 件。
/// @note path は GO テーブル内のドット区切りパス (例 `transform.position`, `LightComponent.intensity`)。
struct PrefabOverride {
    std::string prefabSourceId; ///< プレファブ側 GO の instanceId (対応キー)
    std::string instanceGuid;   ///< インスタンス側 GO の instanceId
    std::string objectName;     ///< 表示用 (インスタンス側の名前)
    std::string path;           ///< 差分のあったプロパティパス
    std::string prefabValue;    ///< 表示用の元値
    std::string instanceValue;  ///< 表示用の現在値
};

/// @brief 1 インスタンス階層ぶんの差分と、パッチ適用に必要な現在値のスナップショット。
struct PrefabOverrideSet {
    std::string prefabAssetPath; ///< Assets 起点の相対パス
    std::vector<PrefabOverride> entries;
    /// @brief prefabSourceId → そのインスタンス GO の現在のシリアライズ表現。
    /// @note entries は「どのパスが違うか」しか持たないため、値の書き戻しはここから引く。
    std::unordered_map<std::string, toml::table> instanceTables;

    [[nodiscard]] bool Empty() const { return entries.empty(); }
};

/// @brief ドット区切りパスで node を引く。
/// @return 見つからなければ nullptr。
[[nodiscard]] const toml::node* FindNodeAtPath(const toml::table& table, const std::string& path);

/// @brief ドット区切りパスへ node を書き込む。
/// @return 途中のテーブルが無い場合は作らず false。
bool SetNodeAtPath(toml::table& table, const std::string& path, const toml::node& value);

/// @brief プレファブアセットを展開してシーンへ追加する。
/// @param path 絶対パスまたはプロジェクトルートからのパス。拡張子が無ければ .prefab を補う。
/// @param overrides 渡すと展開後に該当プロパティだけをその値で上書きする (個別調整を保ったまま作り直す用)。
/// @param preserveGuids プレファブ側 instanceId → 生成後に名乗らせたい instanceId。Revert/Apply の作り直し経路で使う。
/// @note guid を維持しないと、そのインスタンスを指す参照 (追従先・IK ターゲット・FBZZ_REF) が作り直しのたびに行き先を失う。
/// @note 渡した guid が既にシーンで使われていないことは呼び出し側の責任。表に無い分は新規採番する。
bool InstantiatePrefabAsset(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr,
                            const std::unordered_map<std::string, std::string>* preserveGuids = nullptr);

} // namespace fbzz::scene
