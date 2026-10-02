/// @file    PrefabSerializer.cpp
/// @brief   TOML-based prefab save and instantiate helpers.
/// @author  Hasegawa Jin
/// @date    2026-05-26
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::editor {

namespace {

std::string WithPrefabExtension(const std::string& path)
{
    if (path.empty()) return {};
    if (!util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".prefab";
}

bool ContainsEntity(const std::vector<scene::EntityID>& ids, scene::EntityID id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool HasSelectedAncestor(const scene::GameObject& go, const std::vector<scene::EntityID>& selected)
{
    const scene::GameObject* parent = go.GetParent();
    while (parent) {
        if (ContainsEntity(selected, parent->GetID())) return true;
        parent = parent->GetParent();
    }
    return false;
}

void CollectHierarchyIds(scene::GameObject& go, std::unordered_set<std::string>& ids)
{
    if (!go.instanceId.empty())
        ids.insert(go.instanceId);
    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (auto* child = go.GetChild(i))
            CollectHierarchyIds(*child, ids);
    }
}

bool SamePrefabPath(const std::string& lhs, const std::string& rhs)
{
    return util::FileSystem::SamePathText(scene::PrefabAssetKey(lhs), scene::PrefabAssetKey(rhs));
}

/// @brief 同じプレファブのインスタンスが入れ子になっている場合、外側だけを対象にする。
/// @note 親インスタンスを Revert すると子孫ごと作り直されるため、内側を処理すると
/// @note 既に破棄された GO を触ることになる。
bool HasSamePrefabAncestor(const scene::GameObject& go, const std::string& prefabAssetPath)
{
    for (const scene::GameObject* p = go.GetParent(); p; p = p->GetParent())
        if (SamePrefabPath(p->prefabAssetPath, prefabAssetPath)) return true;
    return false;
}

std::unordered_map<std::string, std::string> BuildAssetIdMap(
    const toml::array& objects, const std::unordered_set<std::string>& includedIds)
{
    /// @note 保存と接続の両方で同じ全選択範囲の重複解消順を使う。
    std::unordered_map<std::string, std::string> assetIdMap;
    std::unordered_set<std::string> usedAssetIds;
    for (const auto& item : objects) {
        const auto* source = item.as_table();
        if (!source) continue;
        const std::string instanceId = (*source)["instanceId"].value_or(std::string{});
        if (instanceId.empty() || !includedIds.contains(instanceId)) continue;
        const std::string sourceId = (*source)["prefabSourceId"].value_or(std::string{});
        const std::string assetId = !sourceId.empty() && !usedAssetIds.contains(sourceId) ? sourceId : instanceId;
        usedAssetIds.insert(assetId);
        assetIdMap.emplace(instanceId, assetId);
    }
    return assetIdMap;
}

bool UpdatePrefabSourceTracking(scene::Scene& scene,
                                 const std::vector<scene::EntityID>& roots,
                                 const std::string& diskPath)
{
    std::string sourceSnapshot;
    if (!util::FileSystem::ReadText(diskPath, sourceSnapshot)) return false;
    std::unordered_set<std::string> hierarchyIds;
    for (const auto id : roots)
        if (auto* root = scene.GetGameObject(id)) CollectHierarchyIds(*root, hierarchyIds);
    const auto parsed = toml::parse(SceneIO::Serialize(scene));
    if (!parsed) return false;
    const auto* objects = parsed.table()["gameobjects"].as_array();
    if (!objects) return false;
    const auto assetIdMap = BuildAssetIdMap(*objects, hierarchyIds);
    for (auto& object : scene.GameObjects())
        if (const auto found = assetIdMap.find(object.instanceId); found != assetIdMap.end())
            object.prefabSourceId = found->second;
    const std::string sourceReference = scene::CanonicalPrefabAssetRef(diskPath);
    for (const auto id : roots)
        if (auto* root = scene.GetGameObject(id)) {
            root->prefabAssetPath = sourceReference;
            root->prefabSourceSnapshot = sourceSnapshot;
        }
    return true;
}

/// @note 自己書き込み記録 (ディスク監視の自己反応を弾くため)。
/// @note キーは比較しやすいよう小文字 + '/' 区切りに正規化する。
std::string NormalizeForCompare(const std::string& path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        const char ch = (c == '\\') ? '/' : c;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::unordered_map<std::string, std::chrono::steady_clock::time_point>& SelfWriteLog()
{
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> log;
    return log;
}

} /// @note namespace

bool PrefabSerializer::SaveSelection(const scene::Scene& scene,
                                     const std::vector<scene::EntityID>& selectedEntities,
                                     const std::string& path)
{
    if (selectedEntities.empty() || path.empty()) return false;

    std::vector<scene::EntityID> rootSelection;
    for (scene::EntityID id : selectedEntities) {
        auto* go = scene.GetGameObject(id);
        if (!go || HasSelectedAncestor(*go, selectedEntities)) continue;
        rootSelection.push_back(id);
    }
    if (rootSelection.empty()) return false;

    std::unordered_set<std::string> includedIds;
    for (scene::EntityID id : rootSelection) {
        if (auto* go = scene.GetGameObject(id))
            CollectHierarchyIds(*go, includedIds);
    }

    const std::string sceneText = SceneIO::Serialize(scene);
    if (sceneText.empty()) return false;

    toml::parse_result sceneResult = toml::parse(sceneText);
    if (!sceneResult) return false;

    toml::table doc;
    toml::table sceneInfo;
    sceneInfo.insert("format_version", 1);
    doc.insert("scene", std::move(sceneInfo));

    toml::table prefabInfo;
    prefabInfo.insert("format_version", 1);
    prefabInfo.insert("root_count", static_cast<int64_t>(rootSelection.size()));
    doc.insert("prefab", std::move(prefabInfo));

    /// @note インスタンス側の instanceId → アセットへ書く id の対応を先に決める。素直に instanceId を
    /// @note 書くとアセット側 id が Apply のたびに入れ替わり、他インスタンスの prefabSourceId (旧アセット id)
    /// @note が行き先を失う。prefabSourceId を持つ GO は元のアセット id へ書き戻し、重複時は先着のみ使う。
    const auto* serializedObjects = sceneResult.table()["gameobjects"].as_array();
    if (!serializedObjects) return false;
    const auto assetIdMap = BuildAssetIdMap(*serializedObjects, includedIds);

    auto mapAssetId = [&assetIdMap](const std::string& id) {
        const auto it = assetIdMap.find(id);
        return it != assetIdMap.end() ? it->second : id;
    };

    toml::array prefabObjects;
    if (auto* gameObjects = sceneResult.table()["gameobjects"].as_array()) {
        for (const auto& item : *gameObjects) {
            const auto* source = item.as_table();
            if (!source) continue;

            const std::string instanceId = (*source)["instanceId"].value_or(std::string{});
            if (instanceId.empty() || !includedIds.contains(instanceId)) continue;

            toml::table copied = *source;
            scene::RemapPrefabObjectReferences(copied, assetIdMap);

            copied.erase("instanceId");
            copied.insert("instanceId", mapAssetId(instanceId));

            const std::string parentId = (*source)["parentInstanceId"].value_or(std::string{});
            copied.erase("parent");
            copied.erase("parentInstanceId");
            if (parentId.empty() || !includedIds.contains(parentId)) {
                copied.insert("parent", std::string{});
                copied.insert("parentInstanceId", std::string{});
            } else {
                copied.insert("parent", (*source)["parent"].value_or(std::string{}));
                copied.insert("parentInstanceId", mapAssetId(parentId));
            }

            /// @note アセット側にはリンク情報を残さない。prefabAssetPath は自分自身を指すインスタンスに
            /// @note 見え、prefabSourceId は「1 世代前のアセット id」でしかなく意味を持たない。
            /// @note どちらも Instantiate が生成時に正しい値を入れ直す。
            copied.erase("prefabAssetPath");
            copied.insert("prefabAssetPath", std::string{});
            copied.erase("prefabSourceId");
            copied.insert("prefabSourceId", std::string{});
            copied.erase("prefabSourceSnapshot");

            prefabObjects.push_back(std::move(copied));
        }
    }

    if (prefabObjects.empty()) return false;
    doc.insert("gameobjects", std::move(prefabObjects));

    std::ostringstream ss;
    ss << doc;
    const std::string outputPath = WithPrefabExtension(path);
    if (!util::FileSystem::WriteText(outputPath, ss.str())) {
        FBZZ_LOG_ERROR("Prefab save failed: %s", outputPath.c_str());
        return false;
    }

    SelfWriteLog()[NormalizeForCompare(outputPath)] = std::chrono::steady_clock::now();
    FBZZ_LOG_INFO("Saved prefab: %s", outputPath.c_str());
    return true;
}

bool PrefabSerializer::WasSelfWrittenRecently(const std::string& diskPath, double withinSeconds)
{
    auto& log = SelfWriteLog();
    const auto it = log.find(NormalizeForCompare(diskPath));
    if (it == log.end()) return false;

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - it->second).count();
    return elapsed <= withinSeconds;
}

bool PrefabSerializer::SaveSelectionAndConnect(scene::Scene& scene,
                                               const std::vector<scene::EntityID>& selectedEntities,
                                               const std::string& path,
                                               std::vector<scene::EntityID>& outRoots)
{
    outRoots.clear();
    if (selectedEntities.empty()) return false;

    const std::string outputPath = WithPrefabExtension(path);
    if (!SaveSelection(scene, selectedEntities, outputPath)) return false;

    /// @note 保存後に出所と基準定義を接続し、アセット自身への参照を保存しない。
    for (scene::EntityID id : selectedEntities) {
        auto* go = scene.GetGameObject(id);
        /// @note 選択された祖先を持つものは子。ルートのみ接続する (SaveSelection のルート判定に合わせる)。
        if (!go || HasSelectedAncestor(*go, selectedEntities)) continue;
        outRoots.push_back(id);
    }
    if (!UpdatePrefabSourceTracking(scene, outRoots, outputPath)) {
        outRoots.clear();
        return false;
    }
    return true;
}

bool PrefabSerializer::Instantiate(scene::Scene& scene,
                                   const std::string& path,
                                   std::vector<scene::EntityID>& outRootEntities,
                                   const PrefabOverrideSet* overrides,
                                   const std::unordered_map<std::string, std::string>* preserveGuids)
{
    /// @note 展開そのものは Engine が持つ (Engine/Scene/PrefabInstantiate.hpp)。Editor はオーサリング
    /// @note 操作 (保存/Apply/Revert/差分算出) だけを持ち、「読んで展開する」側は Engine と共有する。
    return scene::InstantiatePrefabAsset(scene, path, outRootEntities, overrides, preserveGuids);
}

bool PrefabSerializer::Apply(scene::Scene& scene, scene::EntityID rootEntity,
                             const std::string& projectRoot)
{
    /// @note 出所の GUID 参照または旧相対パスを解決し、現在のアセットへ書き戻す。
    const scene::GameObject* go = scene.GetGameObject(rootEntity);
    if (!go || go->prefabAssetPath.empty()) {
        FBZZ_LOG_WARN("PrefabSerializer::Apply: entity is not a prefab instance");
        return false;
    }

    const std::string diskPath = ToProjectAssetDiskPath(projectRoot, go->prefabAssetPath);
    if (diskPath.empty()) return false;
    std::string sourceText;
    if (!util::FileSystem::ReadText(diskPath, sourceText)) return false;
    const auto sourceParsed = toml::parse(sourceText);
    if (!sourceParsed) return false;
    const auto* objects = sourceParsed.table()["gameobjects"].as_array();
    if (!objects) return false;
    std::size_t rootCount = 0;
    for (const auto& item : *objects) {
        const auto* object = item.as_table();
        if (!object) return false;
        if ((*object)["parentInstanceId"].value_or(std::string{}).empty() &&
            (*object)["parent"].value_or(std::string{}).empty()) ++rootCount;
    }
    if (rootCount != 1) {
        FBZZ_LOG_WARN("Prefab Apply needs a single-root source asset: %s", diskPath.c_str());
        return false;
    }
    if (!SaveSelection(scene, { rootEntity }, diskPath)) return false;
    return UpdatePrefabSourceTracking(scene, {rootEntity}, diskPath);
}

bool PrefabSerializer::RefreshInstanceKeepingOverrides(scene::Scene& scene,
                                                       scene::EntityID rootEntity,
                                                       std::vector<scene::EntityID>& outNewRoots,
                                                       const std::string& projectRoot)
{
    /// @note 作り直す前に現在の差分を採取する。GO は Revert で破棄されるため、
    /// @note PrefabOverrideSet 側が値のコピーを持っている点が要 (ポインタでは追えない)。
    PrefabOverrideSet overrides;
    if (!ComputePrefabOverrides(scene, rootEntity, projectRoot, overrides)) return false;
    return Revert(scene, rootEntity, outNewRoots, projectRoot, &overrides);
}

bool PrefabSerializer::Revert(scene::Scene& scene,
                              scene::EntityID rootEntity,
                              std::vector<scene::EntityID>& outNewRoots,
                              const std::string& projectRoot,
                              const PrefabOverrideSet* keepOverrides)
{
    outNewRoots.clear();
    if (keepOverrides && keepOverrides->hasStructuralOverrides) {
        FBZZ_LOG_WARN("Prefab refresh skipped: preserve mode cannot merge local child additions, deletions, or parent changes");
        return false;
    }
    /// @note Revert: インスタンスをプレファブアセットの定義に戻す。「コンポーネント・子構成」は
    /// @note リセットするが、ワールド上での配置 (Transform) は保持するのが自然な挙動のため保存し直す。
    scene::GameObject* go = scene.GetGameObject(rootEntity);
    if (!go || go->prefabAssetPath.empty()) {
        FBZZ_LOG_WARN("PrefabSerializer::Revert: entity is not a prefab instance");
        return false;
    }

    /// @note GUID 参照と旧相対パスを現在のアセットへ解決する。
    const std::string diskPath      = ToProjectAssetDiskPath(projectRoot, go->prefabAssetPath);
    const scene::Transform savedTransform = go->transform;
    scene::GameObject* savedParent  = go->GetParent();
    const scene::EntityID savedParentId =
        savedParent ? savedParent->GetID() : scene::EntityID{};

    if (!scene::ReplacePrefabInstanceAsset(scene, rootEntity, diskPath, outNewRoots, keepOverrides))
        return false;

    /// @note 先頭ルートに旧 Transform / 親を復元する。複数ルートを持つプレファブは稀なため、
    /// @note 先頭のみ位置を合わせる。
    if (auto* newGo = scene.GetGameObject(outNewRoots.front())) {
        newGo->transform = savedTransform;
        if (savedParentId.IsValid()) {
            if (auto* parent = scene.GetGameObject(savedParentId))
                newGo->SetParent(*parent);
        }
        scene::FlushWorldTransforms(*newGo);
    }

    return true;
}

int PrefabSerializer::CountInstances(scene::Scene& scene,
                                     const std::string& prefabAssetPath)
{
    if (prefabAssetPath.empty()) return 0;

    int count = 0;
    for (const auto& go : scene.GameObjects()) {
        if (!SamePrefabPath(go.prefabAssetPath, prefabAssetPath)) continue;
        if (HasSamePrefabAncestor(go, prefabAssetPath)) continue;
        ++count;
    }
    return count;
}

int PrefabSerializer::ApplyAndPropagate(scene::Scene& scene,
                                        scene::EntityID rootEntity,
                                        const std::string& projectRoot,
                                        bool preserveOverrides)
{
    /// @note 伝播先を決めるためにアセットパスを先に控える。
    /// @note Apply の途中でインスタンスが差し替わることは無いが、Propagate は
    /// @note 「パス」で対象を探すので、GameObject を跨いで参照を持ち回らない。
    const scene::GameObject* root = scene.GetGameObject(rootEntity);
    if (root == nullptr || root->prefabAssetPath.empty()) return -1;
    const std::string assetPath = root->prefabAssetPath;

    std::vector<std::pair<std::string, PrefabOverrideSet>> capturedOverrides;
    for (auto& object : scene.GameObjects()) {
        if (object.GetID() == rootEntity || !SamePrefabPath(object.prefabAssetPath, assetPath) ||
            HasSamePrefabAncestor(object, assetPath)) continue;
        PrefabOverrideSet overrides;
        if (preserveOverrides && !ComputePrefabOverrides(scene, object.GetID(), projectRoot, overrides))
            return -1;
        capturedOverrides.emplace_back(object.instanceId, std::move(overrides));
    }
    if (!Apply(scene, rootEntity, projectRoot)) return -1;
    int updated = 0;
    for (const auto& [guid, overrides] : capturedOverrides) {
        auto* instance = scene.FindByGuid(guid);
        if (!instance) continue;
        std::vector<scene::EntityID> newRoots;
        if (Revert(scene, instance->GetID(), newRoots, projectRoot,
                   preserveOverrides ? &overrides : nullptr)) ++updated;
    }
    return updated;
}

int PrefabSerializer::PropagateToInstances(scene::Scene& scene,
                                           const std::string& prefabAssetPath,
                                           scene::EntityID exceptRoot,
                                           const std::string& projectRoot,
                                           bool preserveOverrides)
{
    if (prefabAssetPath.empty()) return 0;

    /// @note Revert は GO を破棄して作り直すため EntityID が無効になる。走査中に作り直すと
    /// @note イテレータも壊れるので、GUID (Revert で変化しない) だけを先に集めてから処理する。
    std::vector<std::string> targetGuids;
    for (const auto& go : scene.GameObjects()) {
        if (!SamePrefabPath(go.prefabAssetPath, prefabAssetPath)) continue;
        if (HasSamePrefabAncestor(go, prefabAssetPath)) continue;
        if (exceptRoot.IsValid() && go.GetID() == exceptRoot) continue;
        if (go.instanceId.empty()) continue;
        targetGuids.push_back(go.instanceId);
    }

    int updated = 0;
    for (const std::string& guid : targetGuids) {
        scene::GameObject* instance = scene.FindByGuid(guid);
        /// @note 先行する Revert で入れ子ごと作り直された等
        if (!instance) continue;

        std::vector<scene::EntityID> newRoots;
        const bool ok = preserveOverrides
            ? RefreshInstanceKeepingOverrides(scene, instance->GetID(), newRoots, projectRoot)
            : Revert(scene, instance->GetID(), newRoots, projectRoot);
        if (ok) ++updated;
    }
    return updated;
}

} /// @note namespace fbzz::editor
