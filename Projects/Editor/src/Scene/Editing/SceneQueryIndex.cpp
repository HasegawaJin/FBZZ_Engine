/// @file    SceneQueryIndex.cpp
/// @brief   Scene の軽い一致確認と、変更時のみの名前正規化・Prefab ルート集計。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Scene/Editing/SceneQueryIndex.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Prefab/PrefabInstantiate.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <utility>

namespace fbzz::editor {
namespace {
std::string Lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

std::string PrefabKey(const std::string& path)
{
    return Lower(util::FileSystem::NormalizePathSeparators(scene::PrefabAssetKey(path)));
}
} // namespace

SceneQueryIndex::AssetState SceneQueryIndex::ReadAssetState()
{
    return {asset::AssetDatabase::Revision(), asset::AssetManager::GetAssetGeneration(),
        asset::AssetDatabase::ProjectRoot()};
}

void SceneQueryIndex::Refresh(scene::Scene& source, uint64_t frameSerial)
{
    const uint64_t generation = source.GetRenderSceneGeneration();
    const bool assetsChanged = m_assets != ReadAssetState();
    const bool sceneChanged = m_scene != &source || m_generation != generation;
    const bool refreshedInThisFrame = m_refreshed && m_frameSerial == frameSerial;
    if (!sceneChanged && !assetsChanged && refreshedInThisFrame) {
        return;
    }

    // シーンが変わったときは、別シーンで再利用される EntityID と名前が混ざらないようにする。
    if (sceneChanged) {
        m_entries.clear();
        m_order.clear();
    }
    m_scene = &source;
    m_generation = generation;
    m_frameSerial = frameSerial;
    m_refreshed = true;

    // 表示順・名前と Prefab の所属を別々に照合し、関係するキャッシュだけを更新する。
    bool namesChanged = sceneChanged;
    bool prefabsChanged = sceneChanged || assetsChanged;
    const size_t slotCount = source.EntitySlotCount();
    if (m_entries.size() != slotCount) {
        m_entries.resize(slotCount);
        namesChanged = prefabsChanged = true;
    }
    ++m_refreshSerial;
    size_t ordinal = 0;
    for (const auto& object : source.GameObjects()) {
        const auto id = object.GetID();
        if (ordinal == m_order.size()) {
            m_order.push_back(id);
            namesChanged = true;
        } else if (m_order[ordinal] != id) {
            m_order[ordinal] = id;
            namesChanged = true;
        }
        ++ordinal;
        auto& cached = m_entries[id.index];
        const bool identityChanged = !cached.live || cached.id != id;
        const auto* parent = object.GetParent();
        const auto parentId = parent ? parent->GetID() : scene::EntityID{};
        if (identityChanged || cached.name != object.name) {
            cached.name = object.name;
            cached.lowerName = Lower(object.name);
            namesChanged = true;
        }
        if (identityChanged || cached.prefabPath != object.prefabAssetPath) {
            cached.prefabPath = object.prefabAssetPath;
            prefabsChanged = true;
        }
        if (identityChanged || cached.parent != parentId) {
            prefabsChanged = true;
        }
        cached.id = id;
        cached.parent = parentId;
        cached.live = true;
        cached.seenRefresh = m_refreshSerial;
    }
    if (m_order.size() != ordinal) {
        m_order.resize(ordinal);
        namesChanged = true;
    }

    // この巡回で見つからなかったスロットを無効にし、削除済みのオブジェクトが検索に残らないようにする。
    for (auto& cached : m_entries) {
        if (cached.live && cached.seenRefresh != m_refreshSerial) {
            cached.live = false;
            namesChanged = prefabsChanged = true;
        }
    }
    if (namesChanged) {
        ++m_nameRevision;
    }
    if (prefabsChanged) {
        RebuildPrefabCounts();
    }
}

void SceneQueryIndex::RefreshPrefabKeys()
{
    AssetState observed;
    constexpr int MAX_KEY_PASSES = 2;
    // 遅延 GUID 登録の整合確認は2巡まで。並行登録が続けば巡回開始版を残して次の照会で失効する。
    for (int pass = 0; pass < MAX_KEY_PASSES; ++pass) {
        observed = ReadAssetState();
        for (auto& entry : m_entries) {
            if (!entry.live) {
                continue;
            }
            entry.prefabKey = PrefabKey(entry.prefabPath);
        }
        if (observed == ReadAssetState()) {
            break;
        }
    }
    m_assets = std::move(observed);
}

void SceneQueryIndex::RebuildPrefabCounts()
{
    RefreshPrefabKeys();
    m_prefabCounts.clear();
    for (const auto& entry : m_entries) {
        if (!entry.live) {
            continue;
        }
        if (entry.prefabKey.empty()) {
            continue;
        }

        // 同じ Prefab を指す祖先がいるノードはインスタンスの一部なので、ルートの件数には加えない。
        bool sameAncestor = false;
        auto parent = entry.parent;
        for (size_t depth = 0; parent.IsValid() && depth < m_entries.size(); ++depth) {
            if (parent.index >= m_entries.size()) {
                break;
            }
            const auto& ancestor = m_entries[parent.index];
            if (!ancestor.live || ancestor.id != parent) {
                break;
            }
            if (ancestor.prefabKey == entry.prefabKey) {
                sameAncestor = true;
                break;
            }
            parent = ancestor.parent;
        }
        if (!sameAncestor) {
            ++m_prefabCounts[entry.prefabKey];
        }
    }
}

std::vector<scene::EntityID> SceneQueryIndex::FindNames(std::string_view query, size_t limit) const
{
    std::vector<scene::EntityID> result;
    if (limit == 0) {
        return result;
    }
    const auto lowerQuery = Lower(query);
    for (const auto id : m_order) {
        const auto& entry = m_entries[id.index];
        if (!entry.live || entry.lowerName.find(lowerQuery) == std::string::npos) {
            continue;
        }
        result.push_back(entry.id);
        if (result.size() == limit) {
            break;
        }
    }
    return result;
}

int SceneQueryIndex::CountPrefabInstances(const std::string& path)
{
    if (path.empty()) {
        return 0;
    }
    auto key = PrefabKey(path);
    if (m_assets != ReadAssetState()) {
        RebuildPrefabCounts();
        key = PrefabKey(path);
    }
    const auto found = m_prefabCounts.find(key);
    return found == m_prefabCounts.end() ? 0 : found->second;
}

SceneQueryIndex& SceneQueries(scene::Scene& source, uint64_t frameSerial)
{
    static SceneQueryIndex index;
    index.Refresh(source, frameSerial);
    return index;
}

} // namespace fbzz::editor
