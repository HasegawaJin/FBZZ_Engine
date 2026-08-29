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
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

// 拡張子を補い、"Assets/..." 相対パスをプロジェクトルートで解決する。
// WHY ここで解決するか: PrefabRef::path は Assets 起点の相対パスで保存される。
//     Editor は自分でプロジェクトルートを知っているが、スタンドアロン実行では
//     誰も補完しないため、生成経路そのものが解決できる必要がある。
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
            // WHY: toml++ の node は多態なので、visit で具体型へ落としてから複製する。
            value.visit([&](const auto& concrete) {
                current->insert(key, concrete);
            });
            return true;
        }

        const std::string key = path.substr(start, dot - start);
        toml::node* node = current->get(key);
        if (!node) return false;      // 途中のテーブルが無い場合は作らない (想定外の形)
        current = node->as_table();
        if (!current) return false;
        start = dot + 1;
    }
}

bool InstantiatePrefabAsset(Scene& scene,
                            const std::string& path,
                            std::vector<EntityID>& outRootEntities,
                            const PrefabOverrideSet* overrides)
{
    outRootEntities.clear();

    const std::string diskPath = ResolvePrefabPath(path);

    toml::table prefabDoc;
    if (!ReadToml(diskPath, prefabDoc)) {
        FBZZ_LOG_ERROR("Prefab load failed: %s", diskPath.c_str());
        return false;
    }

    auto* prefabObjects = prefabDoc["gameobjects"].as_array();
    if (!prefabObjects || prefabObjects->empty()) return false;

    auto* resources = renderer::ResourceManager::Active();
    if (resources == nullptr) {
        FBZZ_LOG_ERROR("Prefab instantiate needs an active ResourceManager: %s", diskPath.c_str());
        return false;
    }

    std::unordered_set<std::string> usedNames;
    for (auto& gameObject : scene.GameObjects())
        usedNames.insert(gameObject.name);

    // guidMap:       プレファブ内の instanceId (旧) → 新規 UUID (新)
    // guidToNewName: プレファブ内の instanceId (旧) → 一意化した新名
    // nameMap:       旧名 → 新名 (名前ベース参照のフォールバック。重複名では最後の 1 件のみ)
    // WHY 改名を名前ではなく instanceId をキーにするか: プレファブ内に同名オブジェクト
    //     (骨の "Bone" など) が複数あると、名前をキーにした表は全員を同じ新名へ潰し、
    //     親子と参照の解決が壊れる。
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
            guidMap[oldGuid]       = util::GenerateUUID();
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

        // インスタンス側の override を、GUID / 名前のリマップより前に流し込む。
        // WHY: override のパスは transform や各コンポーネントのプロパティで、
        //      リマップ対象 (name/parent/*Guid) とは重ならない。先に当てておけば
        //      あとは通常のインスタンス化経路をそのまま通せる。
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

        const std::string oldParent = copied["parent"].value_or(std::string{});
        const std::string oldParentGuid = copied["parentInstanceId"].value_or(std::string{});

        // 自身の新名は instanceId から引く (同名オブジェクトでも一意)。
        // guid が無い旧アセットは名前フォールバックに退避する。
        const std::string newName =
            (!oldGuidForName.empty() && guidToNewName.contains(oldGuidForName))
                ? guidToNewName.at(oldGuidForName)
                : (nameMap.contains(oldName) ? nameMap.at(oldName) : oldName);

        copied.erase("name");
        copied.insert("name", newName);

        // parent 名前フィールドも親の guid → 新名で解決する。実際の親子付けは
        // AppendObjects が parentInstanceId(guid) で行うため、ここは表示・root 判定用。
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

        // instanceId: インスタンスごとに新規 UUID を割り当てる。
        // WHY: 重複すると FindByGuid が誤った GO を返す。
        {
            const std::string oldGuid = copied["instanceId"].value_or(std::string{});
            const std::string newGuid = guidMap.contains(oldGuid)
                ? guidMap.at(oldGuid)
                : util::GenerateUUID();
            copied.erase("instanceId");
            copied.insert("instanceId", newGuid);
        }

        // IKSolverComponent: targetName / poleName / *Guid をリマップする。
        // WHY: 複数インスタンス化で KneePole_L → KneePole_L (1) のように名前が変わるが、
        //      chains 内の参照を更新しないと別インスタンスの Pole を解決し、膝が逆に折れる。
        //      nameMap / guidMap に含まれる (= プレファブ内部の) 参照だけを更新し、
        //      外部を指す参照はそのまま残してシーン上の既存オブジェクトを参照させる。
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

        // BoneComponent: skinnedMeshOwner / skinnedMeshOwnerGuid をリマップする。
        // WHY: 指す SkinnedMeshRenderer オーナーの識別子もインスタンス化で変わるため、
        //      別インスタンスの SMR を指さないよう更新する。
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

    // AppendObjects で生成した GO へ、通常のシーンロード経路でしか復元されない
    // コンポーネントを補完する。
    // WHY: AppendObjects は Script フィールドの復元など独自の経路を持つ一方、
    //      全コンポーネントへの追従が漏れやすい。通常ロードを補完元にすることで、
    //      プレファブがシーン保存と同じコンポーネント集合を扱えるようにする。
    if (auto prefabScene = SceneSerializer::LoadFromText(text, *resources, diskPath)) {
        for (auto& sourceObject : prefabScene->GameObjects()) {
            if (sourceObject.instanceId.empty()) continue;
            auto* target = scene.FindByGuid(sourceObject.instanceId);
            if (!target) continue;
            scene.CopyComponentsFrom(*prefabScene, sourceObject.GetID(), target->GetID());
        }
    }

    // 補完コピーしたコンポーネント内の EntityID 参照を、現在の Scene の EntityID へ張り直す。
    // WHY: 一時 Scene からコピーした EntityID は一時 Scene の値を指すため、
    //      GUID / 名前を正として再解決する必要がある。
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

    // インスタンス追跡のため、生成したルート GO へ出所パスを Assets 起点で書き込む。
    // Apply / Revert は diskPath ではなくこの値を参照する。
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

    // プロパティ単位の差分を後から計算できるよう、階層内の全 GO へ
    // 「プレファブ側のどのオブジェクト由来か」を刻む。instanceId は毎回振り直されるため、
    // これが無いとインスタンスとアセットを対応付ける手段が無い。
    for (const auto& [sourceGuid, newGuid] : guidMap) {
        if (auto* gameObject = scene.FindByGuid(newGuid))
            gameObject->prefabSourceId = sourceGuid;
    }

    FBZZ_LOG_INFO("Instantiated prefab: %s", diskPath.c_str());
    return !outRootEntities.empty();
}

} // namespace fbzz::scene
