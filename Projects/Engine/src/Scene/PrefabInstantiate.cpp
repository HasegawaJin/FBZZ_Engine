/// @file    PrefabInstantiate.cpp
/// @brief   .prefab / .vfx の TOML を読み、GUID と名前を振り直してシーンへ展開する。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/PrefabInstantiate.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include <filesystem>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

std::string UniqueName(const std::string& base, std::unordered_set<std::string>& used)
{
    if (!used.contains(base)) {
        used.insert(base);
        return base;
    }
    for (int index = 1; index < 10000; ++index) {
        const std::string candidate = base + " (" + std::to_string(index) + ")";
        if (!used.contains(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
    return base + " (Prefab)";
}

bool ReadToml(const std::string& path, toml::table& outTable)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    toml::parse_result result = toml::parse(text);
    if (!result) return false;

    outTable = result.table();
    return true;
}

/// @note 解析済みプレファブの取り置き。鍵は実ファイルパス、鮮度は最終更新時刻で見る。
/// @note 展開は «読む → TOML 解析 → 木を複製する» の順で、同じファイルなら結果が変わらないため
/// @note 取り置く。更新時刻を見るのは保存し直したら次の生成から反映させるためで、読み直すより
/// @note 時刻の問い合わせの方が桁で安い。
const toml::table* CachedPrefabDoc(const std::string& diskPath)
{
    struct Entry {
        toml::table                    doc;
        std::filesystem::file_time_type stamp{};
    };
    static std::unordered_map<std::string, Entry> cache;

    std::error_code error;
    const std::filesystem::file_time_type stamp =
        std::filesystem::last_write_time(std::filesystem::path(diskPath), error);

    const auto found = cache.find(diskPath);
    /// @note 実体の削除・読込失敗を古い取り置きで隠さず、差し替えを拒否する。
    if (!error && found != cache.end() && found->second.stamp == stamp)
        return &found->second.doc;

    Entry entry;
    if (!ReadToml(diskPath, entry.doc)) return nullptr;
    entry.stamp = error ? std::filesystem::file_time_type{} : stamp;

    return &(cache[diskPath] = std::move(entry)).doc;
}

} /// @note namespace

std::string ResolvePrefabAssetPath(const std::string& reference, const std::string& projectRoot)
{
    if (reference.empty()) return {};
    if (asset::AssetDatabase::IsGuidRef(reference))
        return asset::AssetManager::ResolveAssetPath(reference);
    std::string path = util::FileSystem::NormalizePathSeparators(reference);
    if (util::FileSystem::GetExtension(path).empty()) path += ".prefab";
    const std::string root = projectRoot.empty() ? asset::AssetDatabase::ProjectRoot() : projectRoot;
    if (!root.empty() && !util::FileSystem::PathFromUtf8(path).is_absolute())
        path = root + "/" + path;
    return asset::AssetManager::ResolveAssetPath(path);
}

std::string CanonicalPrefabAssetRef(const std::string& reference)
{
    if (reference.empty() || asset::AssetDatabase::IsGuidRef(reference)) return reference;
    const std::string diskPath = ResolvePrefabAssetPath(reference);
    const std::string assetsRoot = asset::AssetDatabase::AssetsRoot();
    if (!diskPath.empty() && !assetsRoot.empty() && util::FileSystem::IsChildPathText(diskPath, assetsRoot) &&
        util::FileSystem::Exists(diskPath))
        (void)asset::AssetDatabase::GuidFromPath(diskPath);
    return diskPath.empty() ? reference : asset::EncodeGuidRef(diskPath);
}

std::string PrefabAssetKey(const std::string& reference)
{
    const std::string canonical = CanonicalPrefabAssetRef(reference);
    if (asset::AssetDatabase::IsGuidRef(canonical))
        return std::string(asset::AssetDatabase::kGuidPrefix) + asset::AssetDatabase::GuidFromRef(canonical);
    return util::FileSystem::NormalizePathSeparators(canonical);
}

const toml::node* FindNodeAtPath(const toml::table& table, const std::string& path)
{
    const toml::table* current = &table;
    std::size_t start = 0;

    while (true) {
        const std::size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos
                                                       ? std::string::npos
                                                       : dot - start);
        const toml::node* node = current->get(key);
        if (!node) return nullptr;
        if (dot == std::string::npos) return node;

        current = node->as_table();
        if (!current) return nullptr;
        start = dot + 1;
    }
}

bool SetNodeAtPath(toml::table& table, const std::string& path, const toml::node& value)
{
    toml::table* current = &table;
    std::size_t start = 0;

    while (true) {
        const std::size_t dot = path.find('.', start);
        if (dot == std::string::npos) {
            const std::string key = path.substr(start);
            current->erase(key);
            /// @note toml++ の node は多態なので、visit で具体型へ落としてから複製する。
            value.visit([&](const auto& concrete) {
                current->insert(key, concrete);
            });
            return true;
        }

        const std::string key = path.substr(start, dot - start);
        toml::node* node = current->get(key);
        /// @note 途中のテーブルが無い場合は作らない (想定外の形)
        if (!node) return false;
        current = node->as_table();
        if (!current) return false;
        start = dot + 1;
    }
}

bool RemoveNodeAtPath(toml::table& table, const std::string& path)
{
    toml::table* current = &table;
    std::size_t start = 0;
    while (true) {
        const auto dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? dot : dot - start);
        if (dot == std::string::npos) return current->erase(key) > 0;
        auto* node = current->get(key);
        current = node ? node->as_table() : nullptr;
        if (!current) return false;
        start = dot + 1;
    }
}

namespace {

/// @note node 配下の文字列を辿り、guidMap の鍵と «完全一致» するものを新しい guid へ差し替える。
void RemapGuidsInNode(toml::node& node,
                      const std::unordered_map<std::string, std::string>& guidMap)
{
    const auto remapScalar = [&guidMap](toml::node& target) {
        auto* text = target.as_string();
        if (!text) return;
        const auto found = guidMap.find(text->get());
        if (found != guidMap.end()) text->get() = found->second;
    };

    if (auto* table = node.as_table()) {
        for (auto&& [key, value] : *table) {
            (void)key;
            if (value.is_table() || value.is_array()) RemapGuidsInNode(value, guidMap);
            else                                      remapScalar(value);
        }
        return;
    }
    if (auto* array = node.as_array()) {
        for (auto& element : *array) {
            if (element.is_table() || element.is_array()) RemapGuidsInNode(element, guidMap);
            else                                         remapScalar(element);
        }
    }
}

} /// @note namespace

void RemapPrefabObjectReferences(toml::table& table,
                                  const std::unordered_map<std::string, std::string>& guidMap)
{
    for (auto&& [key, value] : table) {
        (void)key;
        if (value.is_table() || value.is_array()) RemapGuidsInNode(value, guidMap);
    }
}

namespace {

struct SavedPrefabIdentity {
    EntityID entity;
    std::string guid;
    std::string name;
};

void CollectReplacementIdentity(GameObject& root, std::vector<SavedPrefabIdentity>& identities)
{
    identities.push_back({root.GetID(), root.instanceId, root.name});
    for (int i = 0; i < root.GetChildCount(); ++i)
        if (auto* child = root.GetChild(i)) CollectReplacementIdentity(*child, identities);
}

bool PreparePrefabObjects(toml::array& objects)
{
    if (objects.empty() || objects.size() > Scene::MAX_ENTITIES) return false;
    std::unordered_set<std::string> ids;
    for (const auto& item : objects) {
        const auto* object = item.as_table();
        if (!object) return false;
        const std::string id = (*object)["instanceId"].value_or(std::string{});
        if (!id.empty() && !ids.insert(id).second) return false;
    }
    std::unordered_map<std::string, std::string> idsByName;
    std::unordered_set<std::string> ambiguousNames;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        auto& object = *objects[index].as_table();
        std::string id = object["instanceId"].value_or(std::string{});
        if (id.empty()) {
            /// @note 旧形式に永続 ID が無い場合も、同じ配列順の更新でソース対応を失わない。
            id = "legacy-prefab-object:" + std::to_string(index);
            while (ids.contains(id)) id += "_";
            ids.insert(id);
            object.insert_or_assign("instanceId", id);
        }
        const std::string name = object["name"].value_or(std::string{"GameObject"});
        if (!idsByName.emplace(name, id).second) ambiguousNames.insert(name);
    }
    std::unordered_map<std::string, std::string> parents;
    bool hasRoot = false;
    for (auto& item : objects) {
        auto& object = *item.as_table();
        const std::string id = object["instanceId"].value_or(std::string{});
        std::string parent = object["parentInstanceId"].value_or(std::string{});
        const std::string parentName = object["parent"].value_or(std::string{});
        if (parent.empty() && !parentName.empty()) {
            const auto found = idsByName.find(parentName);
            if (found == idsByName.end() || ambiguousNames.contains(parentName)) return false;
            parent = found->second;
            object.insert_or_assign("parentInstanceId", parent);
        }
        parents.emplace(id, parent);
        if (parent.empty()) hasRoot = true;
    }
    if (!hasRoot) return false;
    for (const auto& [id, parent] : parents) {
        (void)parent;
        std::unordered_set<std::string> visited;
        std::string cursor = id;
        while (!cursor.empty()) {
            if (!visited.insert(cursor).second) return false;
            const auto found = parents.find(cursor);
            if (found == parents.end()) return false;
            cursor = found->second;
        }
    }
    return true;
}

bool InstantiatePrefabAssetImpl(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides,
                            const std::unordered_map<std::string, std::string>* preserveGuids,
                            EntityID replacingRoot)
{
    outRootEntities.clear();
    if (overrides && overrides->hasStructuralOverrides) {
        FBZZ_LOG_WARN("Prefab preserve refresh refused local hierarchy changes");
        return false;
    }

    const std::string diskPath = ResolvePrefabAssetPath(path);

    /// @note 取り置きは «読むだけ»。振り直しはすべて下の copied 側で行うので共有して問題ない。
    const toml::table* prefabDoc = CachedPrefabDoc(diskPath);
    if (!prefabDoc) {
        FBZZ_LOG_ERROR("Prefab load failed: %s", diskPath.c_str());
        return false;
    }

    toml::table preparedDoc = *prefabDoc;
    auto* prefabObjects = preparedDoc["gameobjects"].as_array();
    if (!prefabObjects || !PreparePrefabObjects(*prefabObjects)) {
        FBZZ_LOG_ERROR("Prefab has an invalid hierarchy: %s", diskPath.c_str());
        return false;
    }
    if (replacingRoot.IsValid()) {
        std::size_t rootCount = 0;
        for (const auto& item : *prefabObjects)
            if ((*item.as_table())["parentInstanceId"].value_or(std::string{}).empty()) ++rootCount;
        if (rootCount != 1) {
            FBZZ_LOG_WARN("Prefab replacement needs a single-root asset: %s", diskPath.c_str());
            return false;
        }
    }
    if (prefabObjects->size() > Scene::MAX_ENTITIES - scene.GameObjectCount()) {
        FBZZ_LOG_ERROR("Prefab needs %zu free entities before replacing the old hierarchy: %s",
                       prefabObjects->size(), diskPath.c_str());
        return false;
    }
    std::vector<SavedPrefabIdentity> replacedIdentities;
    if (replacingRoot.IsValid()) {
        auto* oldRoot = scene.GetGameObject(replacingRoot);
        if (!oldRoot) return false;
        CollectReplacementIdentity(*oldRoot, replacedIdentities);
    }
    std::unordered_set<uint32_t> existingEntities;
    std::unordered_set<std::string> replacedGuids;
    for (const auto& identity : replacedIdentities) replacedGuids.insert(identity.guid);
    std::ostringstream baselineStream;
    baselineStream << preparedDoc;
    const std::string sourceSnapshot = baselineStream.str();

    auto* resources = renderer::ResourceManager::Active();
    std::unordered_set<std::string> usedNames;
    for (auto& gameObject : scene.GameObjects()) {
        existingEntities.insert(gameObject.GetID().index);
        if (!replacedGuids.contains(gameObject.instanceId)) usedNames.insert(gameObject.name);
    }

    /// @note guidMap:       instanceId (旧) → 新規 UUID (新)
    /// @note guidToNewName: instanceId (旧) → 一意化した新名
    /// @note nameMap:       旧名 → 新名 (名前ベース参照のフォールバック。重複名では最後の 1 件のみ)
    /// @note 名前でなく instanceId をキーにするのは、骨の "Bone" 等の同名オブジェクトが複数あると
    /// @note 名前ベースの表が全員を同じ新名へ潰し、親子と参照の解決が壊れるため。
    std::unordered_map<std::string, std::string> guidMap;
    std::unordered_map<std::string, std::string> guidToNewName;
    std::unordered_map<std::string, std::string> nameMap;

    for (const auto& item : *prefabObjects) {
        const auto* source = item.as_table();
        if (!source) continue;

        const std::string oldName = (*source)["name"].value_or(std::string{"GameObject"});
        const std::string newName = UniqueName(oldName, usedNames);
        nameMap[oldName] = newName;

        const std::string oldGuid = (*source)["instanceId"].value_or(std::string{});
        if (!oldGuid.empty()) {
            /// @note 作り直しでは «元の実体と同じ guid» を名乗らせる。表に無い分だけ新規採番する。
            std::string newGuid;
            if (preserveGuids != nullptr) {
                const auto preserved = preserveGuids->find(oldGuid);
                if (preserved != preserveGuids->end()) newGuid = preserved->second;
            }
            if (newGuid.empty()) newGuid = util::GenerateUUID();
            guidMap[oldGuid]       = newGuid;
            guidToNewName[oldGuid] = newName;
        }
    }

    toml::array newObjects;
    for (const auto& item : *prefabObjects) {
        const auto* source = item.as_table();
        if (!source) continue;

        toml::table copied = *source;
        const std::string oldName = copied["name"].value_or(std::string{"GameObject"});
        const std::string oldGuidForName = copied["instanceId"].value_or(std::string{});

        /// @note インスタンス側の override を GUID / 名前のリマップより前に流し込む。override のパスは
        /// @note transform や各コンポーネントのプロパティで、リマップ対象 (name/parent/*Guid) とは
        /// @note 重ならないため、先に当てておけば通常のインスタンス化経路をそのまま通せる。
        if (overrides != nullptr && !oldGuidForName.empty()) {
            const auto snapshot = overrides->instanceTables.find(oldGuidForName);
            if (snapshot != overrides->instanceTables.end()) {
                for (const PrefabOverride& entry : overrides->entries) {
                    if (entry.prefabSourceId != oldGuidForName) continue;
                    if (const toml::node* value = FindNodeAtPath(snapshot->second, entry.path))
                        SetNodeAtPath(copied, entry.path, *value);
                    else
                        RemoveNodeAtPath(copied, entry.path);
                }
            }
        }

        /// @note コンポーネント側の «プレファブ内部を指す guid» を新しい実体へ振り替える。参照キー
        /// @note (targetGuid/poleGuid/skinnedMeshOwnerGuid/skeletonRootGuid や FBZZ_REF のフィールド名)
        /// @note は際限がないため列挙せず、instanceId と文字列完全一致したものだけを機械的に振り替える。
        /// @note 順序依存: instanceId / parentInstanceId / name / parent の 4 つは直後の識別子処理が
        /// @note 旧値を読んで振り直すため、ここで先に書き換えると «知らない guid» を見て新しい UUID を
        /// @note 作り直し親子が切れる。
        RemapPrefabObjectReferences(copied, guidMap);

        const std::string oldParent = copied["parent"].value_or(std::string{});
        const std::string oldParentGuid = copied["parentInstanceId"].value_or(std::string{});

        /// @note 自身の新名は instanceId から引く (同名オブジェクトでも一意)。
        /// @note guid が無い旧アセットは名前フォールバックに退避する。
        const std::string newName =
            (!oldGuidForName.empty() && guidToNewName.contains(oldGuidForName))
                ? guidToNewName.at(oldGuidForName)
                : (nameMap.contains(oldName) ? nameMap.at(oldName) : oldName);

        copied.erase("name");
        copied.insert("name", newName);

        /// @note parent 名前フィールドも親の guid → 新名で解決する。実際の親子付けは
        /// @note AppendObjects が parentInstanceId(guid) で行うため、ここは表示・root 判定用。
        copied.erase("parent");
        std::string newParentName;
        if (!oldParentGuid.empty() && guidToNewName.contains(oldParentGuid))
            newParentName = guidToNewName.at(oldParentGuid);
        else if (!oldParent.empty() && nameMap.contains(oldParent))
            newParentName = nameMap.at(oldParent);
        copied.insert("parent", newParentName);

        copied.erase("parentInstanceId");
        if (!oldParentGuid.empty() && guidMap.contains(oldParentGuid))
            copied.insert("parentInstanceId", guidMap[oldParentGuid]);
        else
            copied.insert("parentInstanceId", std::string{});

        /// @note instanceId はインスタンスごとに新規 UUID を割り当てる。重複すると FindByGuid が
        /// @note 誤った GO を返す。
        {
            const std::string oldGuid = copied["instanceId"].value_or(std::string{});
            const std::string newGuid = guidMap.contains(oldGuid)
                ? guidMap.at(oldGuid)
                : util::GenerateUUID();
            copied.erase("instanceId");
            copied.insert("instanceId", newGuid);
        }

        /// @note IKSolverComponent の targetName / poleName / *Guid をリマップする。複数インスタンス化で
        /// @note 名前が変わる (KneePole_L → (1)) 際に chains の参照を更新しないと別インスタンスの Pole を
        /// @note 解決し膝が逆に折れるため。プレファブ内部の参照だけ更新し外部参照はそのまま残す。
        if (auto* ikTable = copied["IKSolverComponent"].as_table()) {
            if (auto* chains = (*ikTable)["chains"].as_array()) {
                for (auto& chainElement : *chains) {
                    auto* chain = chainElement.as_table();
                    if (!chain) continue;

                    const auto remap = [&](const char* key,
                                           const std::unordered_map<std::string, std::string>& table) {
                        const std::string old = (*chain)[key].value_or(std::string{});
                        if (old.empty() || !table.contains(old)) return;
                        chain->erase(key);
                        chain->insert(key, table.at(old));
                    };
                    remap("targetName", nameMap);
                    remap("poleName", nameMap);
                    remap("targetGuid", guidMap);
                    remap("poleGuid", guidMap);
                }
            }
        }

        /// @note BoneComponent の skinnedMeshOwner / skinnedMeshOwnerGuid をリマップする。指す
        /// @note SkinnedMeshRenderer の識別子もインスタンス化で変わるため、別インスタンスの SMR を
        /// @note 指さないよう更新する。
        if (auto* boneTable = copied["BoneComponent"].as_table()) {
            const std::string oldOwner = (*boneTable)["skinnedMeshOwner"].value_or(std::string{});
            if (!oldOwner.empty() && nameMap.contains(oldOwner)) {
                boneTable->erase("skinnedMeshOwner");
                boneTable->insert("skinnedMeshOwner", nameMap.at(oldOwner));
            }
            const std::string oldOwnerGuid =
                (*boneTable)["skinnedMeshOwnerGuid"].value_or(std::string{});
            if (!oldOwnerGuid.empty() && guidMap.contains(oldOwnerGuid)) {
                boneTable->erase("skinnedMeshOwnerGuid");
                boneTable->insert("skinnedMeshOwnerGuid", guidMap.at(oldOwnerGuid));
            }
        }

        newObjects.push_back(std::move(copied));
    }

    toml::table doc;
    doc.insert("gameobjects", std::move(newObjects));

    std::ostringstream stream;
    stream << doc;
    const std::string text = stream.str();

    /// @note 全コンポーネントの復元まで済ませ、旧階層は追加が成功するまで生かす。
    auto prefabScene = SceneSerializer::LoadFromText(text, resources, diskPath);
    if (!prefabScene || prefabScene->GameObjectCount() != prefabObjects->size()) return false;
    for (const auto& identity : replacedIdentities) {
        if (auto* object = scene.GetGameObject(identity.entity)) {
            object->instanceId = util::GenerateUUID();
            object->name = "__PrefabReplacement_" + object->instanceId;
        }
    }
    if (!SceneSerializer::AppendObjects(scene, text, resources, outRootEntities)) {
        std::vector<EntityID> addedEntities;
        for (auto& object : scene.GameObjects())
            if (!existingEntities.contains(object.GetID().index)) addedEntities.push_back(object.GetID());
        for (const auto id : addedEntities) scene.DestroyGameObject(id);
        for (const auto& identity : replacedIdentities)
            if (auto* object = scene.GetGameObject(identity.entity)) {
                object->instanceId = identity.guid;
                object->name = identity.name;
            }
        outRootEntities.clear();
        return false;
    }
    std::vector<std::pair<EntityID, EntityID>> preparedEntities;
    for (auto& sourceObject : prefabScene->GameObjects()) {
        if (auto* target = scene.FindByGuid(sourceObject.instanceId))
            preparedEntities.emplace_back(sourceObject.GetID(), target->GetID());
    }
    CopyPreparedPrefabComponents(*prefabScene, scene, preparedEntities);

    /// @note 補完コピーしたコンポーネント内の EntityID 参照を現在の Scene の EntityID へ張り直す。
    /// @note 一時 Scene からコピーした EntityID は一時 Scene の値を指すため、GUID / 名前を正として
    /// @note 再解決する。
    for (const auto& [prepared, live] : preparedEntities) {
        (void)prepared;
        auto& target = *scene.GetGameObject(live);
        if (auto* grid = target.GetComponent<TerrainGridComponent>())
            grid->ResolveFromScene(scene);

        auto* ik = target.GetComponent<IKSolverComponent>();
        if (ik == nullptr) continue;
        for (auto& chain : ik->chains) {
            if (!chain.targetGuid.empty()) {
                if (auto* found = scene.FindByGuid(chain.targetGuid))
                    chain.targetEntity = found->GetID();
            } else if (!chain.targetName.empty()) {
                if (auto* found = scene.Find(chain.targetName))
                    chain.targetEntity = found->GetID();
            }

            if (!chain.poleGuid.empty()) {
                if (auto* found = scene.FindByGuid(chain.poleGuid))
                    chain.poleEntity = found->GetID();
            } else if (!chain.poleName.empty()) {
                if (auto* found = scene.Find(chain.poleName))
                    chain.poleEntity = found->GetID();
            }
        }
    }

    if (auto* objects = doc["gameobjects"].as_array()) {
        for (const auto& item : *objects) {
            const auto* objectTable = item.as_table();
            if (!objectTable) continue;
            if (const auto* skinTable = (*objectTable)["SkinnedMeshRenderer"].as_table()) {
                const std::string ownerGuid = (*objectTable)["instanceId"].value_or(std::string{});
                auto* owner = scene.FindByGuid(ownerGuid);
                auto* skin = owner ? owner->GetComponent<SkinnedMeshRenderer>() : nullptr;
                if (skin) {
                    /// @note 一時 Scene に存在しない共有スケルトンは、本 Scene の識別子で解決し直す。
                    const std::string rootGuid = (*skinTable)["skeletonRootGuid"].value_or(std::string{});
                    const std::string rootName = (*skinTable)["skeletonRootName"].value_or(std::string{});
                    auto* skeletonRoot = rootGuid.empty() ? nullptr : scene.FindByGuid(rootGuid);
                    if (!skeletonRoot && !rootName.empty()) skeletonRoot = scene.Find(rootName);
                    skin->skeletonRootEntity = skeletonRoot ? skeletonRoot->GetID() : EntityID::INVALID;
                    /// @note nodeEntities は EnsureBoneHierarchy が nodeIndex から再構築する。
                    skin->nodeEntities.clear();
                }
            }
            const auto* boneTable = (*objectTable)["BoneComponent"].as_table();
            if (!boneTable) continue;

            const std::string boneGuid = (*objectTable)["instanceId"].value_or(std::string{});
            const std::string boneName = (*objectTable)["name"].value_or(std::string{});
            GameObject* boneObject = !boneGuid.empty() ? scene.FindByGuid(boneGuid) : nullptr;
            if (!boneObject && !boneName.empty()) boneObject = scene.Find(boneName);
            if (!boneObject) continue;

            auto* bone = boneObject->GetComponent<BoneComponent>();
            if (!bone) continue;

            const std::string ownerGuid = (*boneTable)["skinnedMeshOwnerGuid"].value_or(std::string{});
            const std::string ownerName = (*boneTable)["skinnedMeshOwner"].value_or(std::string{});
            GameObject* owner = !ownerGuid.empty() ? scene.FindByGuid(ownerGuid) : nullptr;
            if (!owner && !ownerName.empty()) owner = scene.Find(ownerName);
            if (owner) bone->skinnedMeshEntity = owner->GetID();
        }
    }

    const std::string sourceReference = CanonicalPrefabAssetRef(diskPath);
    for (const EntityID id : outRootEntities) {
        if (auto* gameObject = scene.GetGameObject(id)) {
            gameObject->prefabAssetPath = sourceReference;
            gameObject->prefabSourceSnapshot = sourceSnapshot;
        }
    }

    /// @note プロパティ単位の差分を後から計算できるよう、階層内の全 GO へ
    /// @note 「プレファブ側のどのオブジェクト由来か」を刻む。instanceId は毎回振り直されるため、
    /// @note これが無いとインスタンスとアセットを対応付ける手段が無い。
    for (const auto& [sourceGuid, newGuid] : guidMap) {
        if (auto* gameObject = scene.FindByGuid(newGuid))
            gameObject->prefabSourceId = sourceGuid;
    }

    if (replacingRoot.IsValid()) {
        std::vector<std::pair<EntityID, EntityID>> remappedEntities;
        for (const auto& identity : replacedIdentities)
            if (auto* replacement = scene.FindByGuid(identity.guid))
                remappedEntities.emplace_back(identity.entity, replacement->GetID());
            else
                remappedEntities.emplace_back(identity.entity, EntityID::INVALID);
        RemapScenePrefabEntityReferences(scene, remappedEntities);
        scene.DestroyGameObject(replacingRoot);
    }

    for (const auto id : outRootEntities)
        if (auto* root = scene.GetGameObject(id)) FlushWorldTransforms(*root);
    FBZZ_LOG_INFO("Instantiated prefab: %s", diskPath.c_str());
    return !outRootEntities.empty();
}

} /// @note namespace

bool InstantiatePrefabAsset(Scene& scene, const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides,
                            const std::unordered_map<std::string, std::string>* preserveGuids)
{
    return InstantiatePrefabAssetImpl(scene, path, outRootEntities, overrides, preserveGuids, {});
}

bool ReplacePrefabInstanceAsset(Scene& scene, EntityID rootEntity,
                                 const std::string& reference,
                                 std::vector<EntityID>& outRootEntities,
                                 const PrefabOverrideSet* overrides)
{
    outRootEntities.clear();
    auto* root = scene.GetGameObject(rootEntity);
    if (!root) return false;
    std::vector<SavedPrefabIdentity> identities;
    CollectReplacementIdentity(*root, identities);
    std::unordered_map<std::string, std::string> preserveGuids;
    for (const auto& identity : identities)
        if (auto* object = scene.GetGameObject(identity.entity); object && !object->prefabSourceId.empty())
            preserveGuids.emplace(object->prefabSourceId, identity.guid);
    return InstantiatePrefabAssetImpl(scene, reference, outRootEntities, overrides, &preserveGuids, rootEntity);
}

} /// @note namespace fbzz::scene
