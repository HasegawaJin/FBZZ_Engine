/// @file    PrefabInstantiate.cpp
/// @brief   .prefab / .vfx の TOML を読み、GUID と名前を振り直してシーンへ展開する。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/PrefabInstantiate.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
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

/// 拡張子を補い、"Assets/..." 相対パスをプロジェクトルートで解決する。
/// @note PrefabRef::path は Assets 起点の相対パスで保存される。Editor は自分でプロジェクトルートを
///       知っているが、スタンドアロン実行では誰も補完しないため、生成経路自身が解決する。
std::string ResolvePrefabPath(const std::string& path)
{
    std::string resolved = path;
    if (util::FileSystem::GetExtension(resolved).empty()) resolved += ".prefab";
    if (util::FileSystem::Exists(resolved)) return resolved;

    const std::string root = asset::AssetDatabase::ProjectRoot();
    if (root.empty()) return resolved;
    const std::string candidate = root + "/" + resolved;
    return util::FileSystem::Exists(candidate) ? candidate : resolved;
}

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

/// 解析済みプレファブの取り置き。鍵は実ファイルパス、鮮度は最終更新時刻で見る。
/// @note 展開は «読む → TOML 解析 → 木を複製する» の順で、同じファイルなら結果が変わらないため
///       取り置く。更新時刻を見るのは保存し直したら次の生成から反映させるためで、読み直すより
///       時刻の問い合わせの方が桁で安い。
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
    /// @note 時刻が取れないときは «変わっていない» とみなす。取り置きがあるのに読み直すと、
    ///       読めない状況 (排他中など) で毎回もとの重さへ戻ってしまう。
    if (found != cache.end() && (error || found->second.stamp == stamp))
        return &found->second.doc;

    Entry entry;
    if (!ReadToml(diskPath, entry.doc)) return nullptr;
    entry.stamp = error ? std::filesystem::file_time_type{} : stamp;

    return &(cache[diskPath] = std::move(entry)).doc;
}

} // namespace

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

namespace {

/// node 配下の文字列を辿り、guidMap の鍵と «完全一致» するものを新しい guid へ差し替える。
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

/// テーブル直下のスカラーには触れず、入れ子 (= コンポーネントの中身) だけを辿る。
void RemapGuidsInChildNodes(toml::table& table,
                            const std::unordered_map<std::string, std::string>& guidMap)
{
    for (auto&& [key, value] : table) {
        (void)key;
        if (value.is_table() || value.is_array()) RemapGuidsInNode(value, guidMap);
    }
}

} // namespace

bool InstantiatePrefabAsset(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides,
                            const std::unordered_map<std::string, std::string>* preserveGuids)
{
    outRootEntities.clear();

    const std::string diskPath = ResolvePrefabPath(path);

    /// @note 取り置きは «読むだけ»。振り直しはすべて下の copied 側で行うので共有して問題ない。
    const toml::table* prefabDoc = CachedPrefabDoc(diskPath);
    if (!prefabDoc) {
        FBZZ_LOG_ERROR("Prefab load failed: %s", diskPath.c_str());
        return false;
    }

    const auto* prefabObjects = (*prefabDoc)["gameobjects"].as_array();
    if (!prefabObjects || prefabObjects->empty()) return false;

    auto* resources = renderer::ResourceManager::Active();
    if (resources == nullptr) {
        FBZZ_LOG_ERROR("Prefab instantiate needs an active ResourceManager: %s", diskPath.c_str());
        return false;
    }

    std::unordered_set<std::string> usedNames;
    for (auto& gameObject : scene.GameObjects())
        usedNames.insert(gameObject.name);

    /// @note guidMap:       instanceId (旧) → 新規 UUID (新)
    ///       guidToNewName: instanceId (旧) → 一意化した新名
    ///       nameMap:       旧名 → 新名 (名前ベース参照のフォールバック。重複名では最後の 1 件のみ)
    ///       名前でなく instanceId をキーにするのは、骨の "Bone" 等の同名オブジェクトが複数あると
    ///       名前ベースの表が全員を同じ新名へ潰し、親子と参照の解決が壊れるため。
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
        ///       transform や各コンポーネントのプロパティで、リマップ対象 (name/parent/*Guid) とは
        ///       重ならないため、先に当てておけば通常のインスタンス化経路をそのまま通せる。
        if (overrides != nullptr && !oldGuidForName.empty()) {
            const auto snapshot = overrides->instanceTables.find(oldGuidForName);
            if (snapshot != overrides->instanceTables.end()) {
                for (const PrefabOverride& entry : overrides->entries) {
                    if (entry.prefabSourceId != oldGuidForName) continue;
                    if (const toml::node* value = FindNodeAtPath(snapshot->second, entry.path))
                        SetNodeAtPath(copied, entry.path, *value);
                }
            }
        }

        /// @note コンポーネント側の «プレファブ内部を指す guid» を新しい実体へ振り替える。参照キー
        ///       (targetGuid/poleGuid/skinnedMeshOwnerGuid/skeletonRootGuid や FBZZ_REF のフィールド名)
        ///       は際限がないため列挙せず、instanceId と文字列完全一致したものだけを機械的に振り替える。
        /// @note 順序依存: instanceId / parentInstanceId / name / parent の 4 つは直後の識別子処理が
        ///       旧値を読んで振り直すため、ここで先に書き換えると «知らない guid» を見て新しい UUID を
        ///       作り直し親子が切れる。
        RemapGuidsInChildNodes(copied, guidMap);

        const std::string oldParent = copied["parent"].value_or(std::string{});
        const std::string oldParentGuid = copied["parentInstanceId"].value_or(std::string{});

        /// @note 自身の新名は instanceId から引く (同名オブジェクトでも一意)。
        ///       guid が無い旧アセットは名前フォールバックに退避する。
        const std::string newName =
            (!oldGuidForName.empty() && guidToNewName.contains(oldGuidForName))
                ? guidToNewName.at(oldGuidForName)
                : (nameMap.contains(oldName) ? nameMap.at(oldName) : oldName);

        copied.erase("name");
        copied.insert("name", newName);

        /// @note parent 名前フィールドも親の guid → 新名で解決する。実際の親子付けは
        ///       AppendObjects が parentInstanceId(guid) で行うため、ここは表示・root 判定用。
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
        ///       誤った GO を返す。
        {
            const std::string oldGuid = copied["instanceId"].value_or(std::string{});
            const std::string newGuid = guidMap.contains(oldGuid)
                ? guidMap.at(oldGuid)
                : util::GenerateUUID();
            copied.erase("instanceId");
            copied.insert("instanceId", newGuid);
        }

        /// @note IKSolverComponent の targetName / poleName / *Guid をリマップする。複数インスタンス化で
        ///       名前が変わる (KneePole_L → (1)) 際に chains の参照を更新しないと別インスタンスの Pole を
        ///       解決し膝が逆に折れるため。プレファブ内部の参照だけ更新し外部参照はそのまま残す。
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
        ///       SkinnedMeshRenderer の識別子もインスタンス化で変わるため、別インスタンスの SMR を
        ///       指さないよう更新する。
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

    if (!SceneSerializer::AppendObjects(scene, text, *resources, outRootEntities)) return false;

    /// @note AppendObjects で生成した GO へ、通常のシーンロード経路でしか復元されないコンポーネントを
    ///       補完する。AppendObjects は Script フィールド復元など独自経路を持つため全コンポーネントへの
    ///       追従が漏れやすく、通常ロードを補完元にしてシーン保存と同じ集合を扱えるようにする。
    if (auto prefabScene = SceneSerializer::LoadFromText(text, *resources, diskPath)) {
        for (auto& sourceObject : prefabScene->GameObjects()) {
            if (sourceObject.instanceId.empty()) continue;
            auto* target = scene.FindByGuid(sourceObject.instanceId);
            if (!target) continue;
            scene.CopyComponentsFrom(*prefabScene, sourceObject.GetID(), target->GetID());
        }
    }

    /// @note 補完コピーしたコンポーネント内の EntityID 参照を現在の Scene の EntityID へ張り直す。
    ///       一時 Scene からコピーした EntityID は一時 Scene の値を指すため、GUID / 名前を正として
    ///       再解決する。
    for (auto& target : scene.GameObjects()) {
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

    /// @note インスタンス追跡のため、生成したルート GO へ出所パスを Assets 起点で書き込む。
    ///       Apply / Revert は diskPath ではなくこの値を参照する。
    const std::string::size_type assetsPos = [&] {
        const auto pos = diskPath.rfind("/Assets/");
        return pos != std::string::npos ? pos + 1 : std::string::npos;
    }();
    const std::string relativePath =
        (assetsPos != std::string::npos) ? diskPath.substr(assetsPos) : diskPath;

    for (const EntityID id : outRootEntities) {
        if (auto* gameObject = scene.GetGameObject(id))
            gameObject->prefabAssetPath = relativePath;
    }

    /// @note プロパティ単位の差分を後から計算できるよう、階層内の全 GO へ
    ///       「プレファブ側のどのオブジェクト由来か」を刻む。instanceId は毎回振り直されるため、
    ///       これが無いとインスタンスとアセットを対応付ける手段が無い。
    for (const auto& [sourceGuid, newGuid] : guidMap) {
        if (auto* gameObject = scene.FindByGuid(newGuid))
            gameObject->prefabSourceId = sourceGuid;
    }

    FBZZ_LOG_INFO("Instantiated prefab: %s", diskPath.c_str());
    return !outRootEntities.empty();
}

} // namespace fbzz::scene
