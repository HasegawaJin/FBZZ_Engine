/// @file    SceneQueryIndex.hpp
/// @brief   名前検索と Prefab 件数のため、公開 Scene メタデータの変更だけを索引へ反映する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/Entity.hpp>
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
        uint64_t revision = 0;
        int generation = 0;
        std::string projectRoot;
        bool operator==(const AssetState&) const = default;
    };
    [[nodiscard]] static AssetState ReadAssetState();
    struct Entry {
        scene::EntityID id;
        scene::EntityID parent;
        std::string name;
        std::string lowerName;
        std::string prefabPath;
        std::string prefabKey;
        bool live = false;
        uint64_t seenRefresh = 0;
    };
    void RebuildPrefabCounts();
    void RefreshPrefabKeys();
    const scene::Scene* m_scene = nullptr;
    uint64_t m_generation = 0;
    uint64_t m_frameSerial = 0;
    uint64_t m_nameRevision = 0;
    uint64_t m_refreshSerial = 0;
    AssetState m_assets;
    bool m_refreshed = false;
    std::vector<Entry> m_entries;
    std::vector<scene::EntityID> m_order;
    std::unordered_map<std::string, int> m_prefabCounts;
};

/// @note 1 個の active Scene の索引を共有し、同じ UI フレームではメタデータを1回だけ確認する。
SceneQueryIndex& SceneQueries(scene::Scene& scene, uint64_t frameSerial);

} /// @note namespace fbzz::editor
