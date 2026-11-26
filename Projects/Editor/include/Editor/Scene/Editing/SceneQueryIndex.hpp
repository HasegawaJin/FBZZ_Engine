/// @file    SceneQueryIndex.hpp
/// @brief   名前検索と Prefab 件数のため、公開 Scene メタデータの変更だけを索引へ反映する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/References/Entity.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }
namespace fbzz::editor {

/// @note UI スレッド専用。名前・Prefab パスの直接書き込みも次の UI フレームで検出する。
class SceneQueryIndex {
public:
    void Refresh(scene::Scene& scene, uint64_t frameSerial);
    [[nodiscard]] uint64_t NameRevision() const { return m_nameRevision; }
    [[nodiscard]] std::vector<scene::EntityID> FindNames(std::string_view query, size_t limit) const;
    /// @note 照会時の遅延 GUID 登録でも Prefab 件数だけを同期する。
    [[nodiscard]] int CountPrefabInstances(const std::string& path);
private:
    struct AssetState {
        uint64_t revision = 0; ///< GUID とアセットパスを引く AssetDatabase の更新番号。
        int generation = 0; ///< アセットの再読み込みを識別する AssetManager の世代番号。
        std::string projectRoot; ///< パスと GUID を解決するときのプロジェクトルート。
        bool operator==(const AssetState&) const = default;
    };
    [[nodiscard]] static AssetState ReadAssetState();
    struct Entry {
        scene::EntityID id; ///< 現在スロットにいるエンティティの ID。世代を含む。
        scene::EntityID parent; ///< Prefab の祖先判定で使う親エンティティの ID。
        std::string name; ///< シーンが保持する元の名前。直接変更を検知するために控える。
        std::string lowerName; ///< 名前検索用に大文字小文字を正規化した文字列。
        std::string prefabPath; ///< シーンに記録された Prefab アセットパス。参照先が変わったか照合する。
        std::string prefabKey; ///< Prefab 件数を集計するために GUID とパスの表記を揃えたキー。
        bool live = false; ///< スロットに有効なオブジェクトがいるか。
        uint64_t seenRefresh = 0; ///< このスロットを最後に見つけた m_refreshSerial の値。
    };
    void RebuildPrefabCounts();
    void RefreshPrefabKeys();
    const scene::Scene* m_scene = nullptr; ///< 最後に索引を更新した借用シーン。同じポインターか確認するために保持する。
    uint64_t m_generation = 0; ///< m_scene の中身が入れ替わったかを判定するシーンの世代番号。
    uint64_t m_frameSerial = 0; ///< 重複更新を避けるために控えた直近の UI フレーム番号。
    uint64_t m_nameRevision = 0; ///< 検索結果の再計算が必要になる名前や並び順の更新番号。
    uint64_t m_refreshSerial = 0; ///< オブジェクトの削除を検知する索引巡回の通し番号。
    AssetState m_assets; ///< m_prefabCounts を集計したときのアセット索引の状態。
    bool m_refreshed = false; ///< 現在のフレーム番号と比較できる直前の更新があるか。
    std::vector<Entry> m_entries; ///< EntityID の index で参照するオブジェクトのメタデータ。
    std::vector<scene::EntityID> m_order; ///< Scene::GameObjects() の列挙順に並ぶ有効なエンティティ。
    std::unordered_map<std::string, int> m_prefabCounts; ///< 正規化した Prefab キーごとのインスタンスルート数。
};

/// @note 1 個の active Scene の索引を共有し、同じ UI フレームではメタデータを1回だけ確認する。
SceneQueryIndex& SceneQueries(scene::Scene& scene, uint64_t frameSerial);

} // namespace fbzz::editor
