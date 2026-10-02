/// @file    PrefabOverrides.cpp
/// @brief   プレファブインスタンスとアセット定義の TOML 差分。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/PrefabOverrides.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace fbzz::editor {

namespace {

/// @brief 差分の対象外にするキー。
/// @note 識別子と出所メタデータは差分から除き、内部参照はソース ID へ正規化して比較する。
bool IsIdentityKey(std::string_view key)
{
    static constexpr std::string_view kExcluded[] = {
        "instanceId", "parentInstanceId", "name", "parent",
        "prefabAssetPath", "prefabSourceId", "prefabSourceSnapshot"
    };
    for (std::string_view e : kExcluded)
        if (key == e) return true;

    return false;
}

std::string JoinPath(const std::string& prefix, std::string_view key)
{
    if (prefix.empty()) return std::string(key);
    return prefix + "." + std::string(key);
}

std::string NodeToString(const toml::node& node)
{
    std::ostringstream ss;
    ss << toml::toml_formatter{node};
    return ss.str();
}

/// @note 比較用コピーだけの表示ヒントを除き、サブアセットを含む GUID 本体は保持する。
void RemoveAssetReferenceHints(toml::node& node)
{
    if (auto* value = node.as_string()) {
        std::string& ref = value->get();
        if (asset::AssetDatabase::IsGuidRef(ref)) {
            const std::size_t hint = ref.find(asset::AssetDatabase::kRefHintSeparator);
            if (hint != std::string::npos) ref.resize(hint);
        }
    } else if (auto* table = node.as_table()) {
        for (auto& [key, value] : *table) RemoveAssetReferenceHints(value);
    } else if (auto* array = node.as_array()) {
        for (auto& value : *array) RemoveAssetReferenceHints(value);
    }
}

/// @note 旧パスと GUID 表記を同一視する。差分パッチや Inspector の表示値は変更しない。
std::string ComparisonNodeToString(const toml::node& node)
{
    toml::table copy;
    node.visit([&](const auto& value) { copy.insert("value", value); });
    asset::EncodeGuidRefs(copy);
    RemoveAssetReferenceHints(copy);
    return NodeToString(*copy.get("value"));
}

bool NodesEqual(const toml::node& lhs, const toml::node& rhs)
{
    if (lhs.type() != rhs.type()) return false;
    return ComparisonNodeToString(lhs) == ComparisonNodeToString(rhs);
}

/// @note prefabTable と instanceTable を再帰的に比べ、差分を entries へ積む。
void DiffTables(const toml::table& prefabTable,
                const toml::table& instanceTable,
                const std::string& pathPrefix,
                const std::string& prefabSourceId,
                const std::string& instanceGuid,
                const std::string& objectName,
                std::vector<PrefabOverride>& entries)
{
    /// @note インスタンス側にある / 変わったキーを見る。
    for (const auto& [keyView, instanceNode] : instanceTable) {
        const std::string_view key = keyView.str();
        if (IsIdentityKey(key)) continue;

        const std::string path = JoinPath(pathPrefix, key);
        const toml::node* prefabNode = prefabTable.get(key);

        /// @note 入れ子テーブルは 1 段掘る (コンポーネント単位ではなくプロパティ単位で出すため)。
        if (instanceNode.is_table() && prefabNode && prefabNode->is_table()) {
            DiffTables(*prefabNode->as_table(), *instanceNode.as_table(),
                       path, prefabSourceId, instanceGuid, objectName, entries);
            continue;
        }

        if (prefabNode && NodesEqual(*prefabNode, instanceNode)) continue;

        PrefabOverride ov;
        ov.prefabSourceId = prefabSourceId;
        ov.instanceGuid   = instanceGuid;
        ov.objectName     = objectName;
        ov.path           = path;
        ov.prefabValue    = prefabNode ? FormatNodeForDisplay(prefabNode) : "(added)";
        ov.instanceValue  = FormatNodeForDisplay(&instanceNode);
        entries.push_back(std::move(ov));
    }

    /// @note プレファブ側にあってインスタンス側に無いキー (コンポーネントを外した等)。
    for (const auto& [keyView, prefabNode] : prefabTable) {
        const std::string_view key = keyView.str();
        if (IsIdentityKey(key)) continue;
        if (instanceTable.get(key) != nullptr) continue;

        PrefabOverride ov;
        ov.prefabSourceId = prefabSourceId;
        ov.instanceGuid   = instanceGuid;
        ov.objectName     = objectName;
        ov.path           = JoinPath(pathPrefix, key);
        ov.prefabValue    = FormatNodeForDisplay(&prefabNode);
        ov.instanceValue  = "(removed)";
        entries.push_back(std::move(ov));
    }
}

/// @note rootEntity 以下の GO を GUID で集める。
void CollectHierarchyGuids(scene::GameObject& go, std::unordered_set<std::string>& out)
{
    if (!go.instanceId.empty()) out.insert(go.instanceId);
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (auto* child = go.GetChild(i))
            CollectHierarchyGuids(*child, out);
}

/// @note 一意化された表示名を個別変更と誤認しないよう、内部参照の名前ヒントもソースへ戻す。
void NormalizeReferenceNames(toml::table& object,
    const std::unordered_map<std::string, const toml::table*>& sources)
{
    const auto normalize = [&](toml::table& table, const char* guidKey, const char* nameKey) {
        const std::string sourceId = table[guidKey].value_or(std::string{});
        const auto source = sources.find(sourceId);
        if (source != sources.end() && table.contains(nameKey))
            table.insert_or_assign(nameKey, (*source->second)["name"].value_or(std::string{}));
    };
    if (auto* ik = object["IKSolverComponent"].as_table())
        if (auto* chains = (*ik)["chains"].as_array())
            for (auto& item : *chains)
                if (auto* chain = item.as_table()) {
                    normalize(*chain, "targetGuid", "targetName");
                    normalize(*chain, "poleGuid", "poleName");
                }
    if (auto* bone = object["BoneComponent"].as_table())
        normalize(*bone, "skinnedMeshOwnerGuid", "skinnedMeshOwner");
    if (auto* skin = object["SkinnedMeshRenderer"].as_table())
        normalize(*skin, "skeletonRootGuid", "skeletonRootName");
}

} /// @note namespace

std::string FormatNodeForDisplay(const toml::node* node)
{
    if (!node) return "(none)";

    std::string text = NodeToString(*node);
    /// @note 1 行に収める (配列やインラインテーブルの改行を潰す)。
    std::string oneLine;
    oneLine.reserve(text.size());
    bool lastWasSpace = false;
    for (char c : text) {
        const char ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (ch == ' ') {
            if (lastWasSpace) continue;
            lastWasSpace = true;
        } else {
            lastWasSpace = false;
        }
        oneLine.push_back(ch);
    }
    /// @note substr はバイトで切るので、日本語の値が «□» で終わる。文字境界まで戻す。
    constexpr std::size_t kMaxLen = 64;
    return util::StringUtils::TruncateUtf8(oneLine, kMaxLen);
}

bool ComputePrefabOverrides(scene::Scene& scene,
                            scene::EntityID rootEntity,
                            const std::string& projectRoot,
                            PrefabOverrideSet& out)
{
    out = {};

    scene::GameObject* root = scene.GetGameObject(rootEntity);
    if (!root || root->prefabAssetPath.empty()) return false;
    out.prefabAssetPath = root->prefabAssetPath;

    /// @name プレファブ定義を読む
    const std::string diskPath = ToProjectAssetDiskPath(projectRoot, root->prefabAssetPath);
    std::string prefabText = root->prefabSourceSnapshot;
    if (prefabText.empty() && !util::FileSystem::ReadText(diskPath, prefabText)) return false;

    toml::parse_result prefabParsed = toml::parse(prefabText);
    if (!prefabParsed) return false;

    std::unordered_map<std::string, const toml::table*> prefabById;
    if (auto* arr = prefabParsed.table()["gameobjects"].as_array()) {
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) continue;
            const std::string id = (*tbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) prefabById.emplace(id, tbl);
        }
    }
    if (prefabById.empty()) return false;

    std::unordered_map<std::string, std::string> sourceByName;
    for (const auto& [id, table] : prefabById)
        sourceByName.emplace((*table)["name"].value_or(std::string{}), id);
    std::unordered_map<std::string, std::string> sourceParents;
    for (const auto& [id, table] : prefabById) {
        std::string parent = (*table)["parentInstanceId"].value_or(std::string{});
        if (parent.empty()) {
            const std::string parentName = (*table)["parent"].value_or(std::string{});
            const auto byName = sourceByName.find(parentName);
            if (!parentName.empty() && byName != sourceByName.end()) parent = byName->second;
        }
        sourceParents.emplace(id, std::move(parent));
    }
    /// @note 複数ルートのアセットでも、このルート由来の構造だけを比較する。
    std::unordered_set<std::string> expectedSources;
    if (prefabById.contains(root->prefabSourceId))
        expectedSources.insert(root->prefabSourceId);
    else
        out.hasStructuralOverrides = true;
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& [id, parent] : sourceParents)
            if (!expectedSources.contains(id) && expectedSources.contains(parent)) {
                expectedSources.insert(id);
                expanded = true;
            }
    }

    /// @name インスタンス側の現在状態をシリアライズして拾う
    /// @note ランタイム構造体を直接比べず、保存されるのと同じ表現で比べる。
    /// @note Save したら消える差分を override として出さないため。
    std::unordered_set<std::string> hierarchyGuids;
    CollectHierarchyGuids(*root, hierarchyGuids);

    const std::string sceneText = SceneIO::Serialize(scene);
    if (sceneText.empty()) return false;
    toml::parse_result sceneParsed = toml::parse(sceneText);
    if (!sceneParsed) return false;

    auto* sceneObjects = sceneParsed.table()["gameobjects"].as_array();
    if (!sceneObjects) return false;

    std::unordered_map<std::string, std::string> instanceToSource;
    for (const auto& item : *sceneObjects) {
        const auto* table = item.as_table();
        if (!table) continue;
        const std::string guid = (*table)["instanceId"].value_or(std::string{});
        const std::string source = (*table)["prefabSourceId"].value_or(std::string{});
        if (hierarchyGuids.contains(guid) && !source.empty()) instanceToSource.emplace(guid, source);
    }
    std::unordered_set<std::string> actualSources;
    for (const auto& item : *sceneObjects) {
        const auto* tbl = item.as_table();
        if (!tbl) continue;

        const std::string instanceGuid = (*tbl)["instanceId"].value_or(std::string{});
        if (instanceGuid.empty() || !hierarchyGuids.contains(instanceGuid)) continue;

        const std::string sourceId = (*tbl)["prefabSourceId"].value_or(std::string{});
        if (sourceId.empty() || !expectedSources.contains(sourceId) || !actualSources.insert(sourceId).second)
            out.hasStructuralOverrides = true;
        if (sourceId.empty()) continue;
        const auto found = prefabById.find(sourceId);
        if (found == prefabById.end()) continue;
        /// @note ルートの配置先はインスタンス固有のため、子の親だけを比較する。
        if (instanceGuid != root->instanceId) {
            const std::string parentGuid = (*tbl)["parentInstanceId"].value_or(std::string{});
            const auto parent = instanceToSource.find(parentGuid);
            const std::string parentSource = parent == instanceToSource.end() ? std::string{} : parent->second;
            if (parentSource != sourceParents.at(sourceId)) out.hasStructuralOverrides = true;
        }

        const std::string objectName = (*tbl)["name"].value_or(std::string{"GameObject"});
        toml::table normalized = *tbl;
        scene::RemapPrefabObjectReferences(normalized, instanceToSource);
        NormalizeReferenceNames(normalized, prefabById);
        out.instanceTables.emplace(sourceId, normalized);
        DiffTables(*found->second, normalized, {}, sourceId, instanceGuid, objectName, out.entries);
    }

    if (actualSources != expectedSources) out.hasStructuralOverrides = true;
    return true;
}

PrefabOverrideSet WithoutEntry(const PrefabOverrideSet& set, const PrefabOverride& removed)
{
    PrefabOverrideSet result;
    result.prefabAssetPath = set.prefabAssetPath;
    result.instanceTables  = set.instanceTables;
    result.hasStructuralOverrides = set.hasStructuralOverrides;
    for (const PrefabOverride& e : set.entries) {
        if (e.prefabSourceId == removed.prefabSourceId && e.path == removed.path) continue;
        result.entries.push_back(e);
    }
    return result;
}

} /// @note namespace fbzz::editor
