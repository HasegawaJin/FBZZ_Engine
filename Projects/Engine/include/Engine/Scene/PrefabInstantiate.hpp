/// @file    PrefabInstantiate.hpp
/// @brief   .prefab / .vfx を GameObject 階層として展開する読み取り専用の生成経路。
/// @author  Hasegawa Jin
/// @date    2026-08-26
/// @note 生成経路はもと Editor (PrefabSerializer) にしか無く、スタンドアロンでは
/// @note Script::InstantiatePrefab のコールバック未設定で PrefabPool::Spawn が必ず失敗していた。
/// @note 保存・Apply・Revert・差分算出はオーサリング操作なので Editor に残る。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief GUID と旧パスを共通のアセット解決口でディスク上のパスへ戻す。
/// @return GUID が未登録なら空文字列。
[[nodiscard]] std::string ResolvePrefabAssetPath(const std::string& reference,
                                                const std::string& projectRoot = {});

/// @brief 解決できる旧パスを GUID 参照へ正規化する。
/// @return .meta を確保できなければ元のパス。
[[nodiscard]] std::string CanonicalPrefabAssetRef(const std::string& reference);

/// @brief パスヒントに依らない比較キー。解決できるアセットは GUID のみを返す。
[[nodiscard]] std::string PrefabAssetKey(const std::string& reference);

/// @brief コンポーネント内の内部参照だけを指定の識別子へ振り替える。
/// @note トップレベルの instanceId / parentInstanceId は呼び出し側が扱う。
void RemapPrefabObjectReferences(toml::table& object,
                                  const std::unordered_map<std::string, std::string>& guidMap);

/// @brief プレファブ定義とインスタンスの差分 1 件。
/// @note path は GO テーブル内のドット区切りパス (例 `transform.position`, `LightComponent.intensity`)。
struct PrefabOverride {
    std::string prefabSourceId; ///< @note プレファブ側 GO の instanceId (対応キー)
    std::string instanceGuid;   ///< @note インスタンス側 GO の instanceId
    std::string objectName;     ///< @note 表示用 (インスタンス側の名前)
    std::string path;           ///< @note 差分のあったプロパティパス
    std::string prefabValue;    ///< @note 表示用の元値
    std::string instanceValue;  ///< @note 表示用の現在値
};

/// @brief 1 インスタンス階層ぶんの差分と、パッチ適用に必要な現在値のスナップショット。
struct PrefabOverrideSet {
    std::string prefabAssetPath; ///< @note GUID 参照。旧パスも読み取る。
    std::vector<PrefabOverride> entries;
    bool hasStructuralOverrides = false; ///< @note 子の追加・削除・親変更は自動更新で失わないよう拒否する。
    /// @brief prefabSourceId → そのインスタンス GO の現在のシリアライズ表現。
    /// @note entries は「どのパスが違うか」しか持たないため、値の書き戻しはここから引く。
    std::unordered_map<std::string, toml::table> instanceTables;

    [[nodiscard]] bool Empty() const { return entries.empty() && !hasStructuralOverrides; }
};

/// @brief ドット区切りパスで node を引く。
/// @return 見つからなければ nullptr。
[[nodiscard]] const toml::node* FindNodeAtPath(const toml::table& table, const std::string& path);

/// @brief ドット区切りパスへ node を書き込む。
/// @return 途中のテーブルが無い場合は作らず false。
bool SetNodeAtPath(toml::table& table, const std::string& path, const toml::node& value);

/// @brief 差分で取り除いたコンポーネントまたはプロパティを削除する。
bool RemoveNodeAtPath(toml::table& table, const std::string& path);

/// @brief 差し替え後の実体へ、シーンに残る非所有参照を振り替える。
/// @note 削除された対象は EntityID::INVALID へ振り替える。
void RemapScenePrefabEntityReferences(Scene& scene,
                                       const std::vector<std::pair<EntityID, EntityID>>& replacements);

/// @brief 通常の追記で復元されないコンポーネントだけを補完し、一時 Scene の参照を振り替える。
void CopyPreparedPrefabComponents(Scene& source, Scene& destination,
                                    const std::vector<std::pair<EntityID, EntityID>>& replacements);

/// @brief プレファブアセットを展開してシーンへ追加する。
/// @param path 絶対パスまたはプロジェクトルートからのパス。拡張子が無ければ .prefab を補う。
/// @param overrides 渡すと展開後に該当プロパティだけをその値で上書きする (個別調整を保ったまま作り直す用)。
/// @param preserveGuids プレファブ側 instanceId → 生成後に名乗らせたい instanceId。Revert/Apply の作り直し経路で使う。
/// @note GPU が無い場合はデータだけを復元する。既存実体の差し替えは ReplacePrefabInstanceAsset を使う。
/// @note 渡した guid が既にシーンで使われていないことは呼び出し側の責任。表に無い分は新規採番する。
bool InstantiatePrefabAsset(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides = nullptr,
                            const std::unordered_map<std::string, std::string>* preserveGuids = nullptr);

/// @brief 新しい階層を検証・追加した後に旧階層を差し替える。
/// @return 読み込み・構造・容量の不備なら false。旧階層は変更しない。
/// @note 単一ルートのアセットと両階層を同時に保持できる空き容量が必要。旧 EntityID は無効になり、保存対象のシーン内参照は新 EntityID へ移す。
/// @see Docs/design/prefab-safety.md
bool ReplacePrefabInstanceAsset(Scene& scene,
                                 EntityID rootEntity,
                                 const std::string& reference,
                                 std::vector<EntityID>& outRootEntities,
                                 const PrefabOverrideSet* overrides = nullptr);

} /// @note namespace fbzz::scene
