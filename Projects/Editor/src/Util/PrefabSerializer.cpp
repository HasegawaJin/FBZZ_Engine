// FBZZ Engine
// PrefabSerializer.cpp | fbzz::editor
// TOML-based prefab save and instantiate helpers
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
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

std::string UniqueName(const std::string& base, std::unordered_set<std::string>& used)
{
    if (!used.contains(base)) {
        used.insert(base);
        return base;
    }

    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = base + " (" + std::to_string(i) + ")";
        if (!used.contains(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
    return base + " (Prefab)";
}

// prefabAssetPath 同士を比べる。
// WHY: 保存経路によって "Assets/Prefabs/A.prefab" と "assets\\prefabs\\A.prefab" が
//      混在し得る。区切りと大小を正規化しないと、同じアセットのインスタンスを取り逃す。
bool SamePrefabPath(const std::string& lhs, const std::string& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
        b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
        if (a != b) return false;
    }
    return true;
}

// 同じプレファブのインスタンスが入れ子になっている場合、外側だけを対象にする。
// WHY: 親インスタンスを Revert すると子孫ごと作り直されるため、内側も処理すると
//      既に破棄された GO を触ることになる。
bool HasSamePrefabAncestor(const scene::GameObject& go, const std::string& prefabAssetPath)
{
    for (const scene::GameObject* p = go.GetParent(); p; p = p->GetParent())
        if (SamePrefabPath(p->prefabAssetPath, prefabAssetPath)) return true;
    return false;
}

// 自己書き込み記録 (ディスク監視の自己反応を弾くため)。
// キーは比較しやすいよう小文字 + '/' 区切りに正規化する。
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

bool PrefabSerializer::SaveSelection(const scene::Scene& scene,
                                     const std::vector<scene::EntityID>& selectedEntities,
                                     const std::string& path)
{
    if (selectedEntities.empty()) return false;

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

    // インスタンス側の instanceId → アセットへ書く id の対応を先に決める。
    //
    // WHY: 既存インスタンスを Apply で書き戻すとき、素直に instanceId をそのまま書くと
    //      アセット側のオブジェクト id が毎回入れ替わる。すると他インスタンスが持つ
    //      prefabSourceId (= 旧アセット id) が行き先を失い、override の対応付けが切れる。
    //      prefabSourceId を持っている GO は「アセット側の元 id」に書き戻すことで、
    //      Apply を何度繰り返してもアセットの id を安定させる。
    //
    //      インスタンス内で子を複製した場合など prefabSourceId が重複しうるので、
    //      先着だけがそれを使い、後発は自分の instanceId をそのまま使う。
    std::unordered_map<std::string, std::string> assetIdMap;
    std::unordered_set<std::string> usedAssetIds;
    if (auto* gameObjects = sceneResult.table()["gameobjects"].as_array()) {
        for (const auto& item : *gameObjects) {
            const auto* source = item.as_table();
            if (!source) continue;
            const std::string instanceId = (*source)["instanceId"].value_or(std::string{});
            if (instanceId.empty() || !includedIds.contains(instanceId)) continue;

            const std::string sourceId = (*source)["prefabSourceId"].value_or(std::string{});
            const std::string assetId =
                (!sourceId.empty() && !usedAssetIds.contains(sourceId)) ? sourceId : instanceId;
            usedAssetIds.insert(assetId);
            assetIdMap.emplace(instanceId, assetId);
        }
    }

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

            // アセット側にはリンク情報を残さない。
            // WHY: prefabAssetPath を書くと自分自身を指すインスタンスに見え、
            //      prefabSourceId は「1 世代前のアセット id」でしかなく意味を持たない。
            //      どちらも Instantiate が生成時に正しい値を入れ直す。
            copied.erase("prefabAssetPath");
            copied.insert("prefabAssetPath", std::string{});
            copied.erase("prefabSourceId");
            copied.insert("prefabSourceId", std::string{});

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

    // Assets 起点の相対パスをルートに書き込んでインスタンス接続する。
    // WHY: prefabAssetPath は配布後も壊れない Assets 相対で保持する。SaveSelection の後に
    //      設定することで、保存されたアセット側には空の prefabAssetPath が入る (自己参照回避)。
    const std::string relPath = NormalizeAssetPath(outputPath);
    for (scene::EntityID id : selectedEntities) {
        auto* go = scene.GetGameObject(id);
        // 選択された祖先を持つものは子。ルートのみ接続する (SaveSelection のルート判定に合わせる)。
        if (!go || HasSelectedAncestor(*go, selectedEntities)) continue;
        go->prefabAssetPath = relPath;
        outRoots.push_back(id);
    }
    return true;
}

bool PrefabSerializer::Instantiate(scene::Scene& scene,
                                   const std::string& path,
                                   std::vector<scene::EntityID>& outRootEntities,
                                   const PrefabOverrideSet* overrides)
{
    outRootEntities.clear();

    toml::table prefabDoc;
    if (!ReadToml(path, prefabDoc)) {
        FBZZ_LOG_ERROR("Prefab load failed: %s", path.c_str());
        return false;
    }

    auto* prefabObjects = prefabDoc["gameobjects"].as_array();
    if (!prefabObjects || prefabObjects->empty()) return false;

    std::unordered_set<std::string> usedNames;
    for (auto& go : scene.GameObjects())
        usedNames.insert(go.name);

    // guidMap:        プレファブ内の instanceId (旧) → 新規 UUID (新)
    // guidToNewName:   プレファブ内の instanceId (旧) → 一意化した新名
    // nameMap:         旧名 → 新名 (名前ベース参照のフォールバック。重複名では最後の1件のみ)
    // WHY: インスタンス化のたびに新しい UUID を割り当てることで、同一プレファブを複数
    //      インスタンス化しても GUID が衝突しない。加えて改名は "名前" ではなく instanceId を
    //      キーにする。旧実装は nameMap[oldName] を上書きしていたため、プレファブ内に同名
    //      オブジェクト (骨の "Bone" など) が複数あると全員が同じ新名へ潰れ、親子・参照解決が
    //      壊れていた。guid をキーにすれば同名でも 1 オブジェクト 1 新名を保証できる。
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

        // インスタンス側の override を、GUID/名前のリマップより前に流し込む。
        // WHY: override のパスは transform や各コンポーネントのプロパティで、
        //      リマップ対象 (name/parent/*Guid) とは重ならない。先に当てておけば
        //      あとは通常のインスタンス化経路をそのまま通せる。
        if (overrides && !oldGuidForName.empty()) {
            const auto snapshot = overrides->instanceTables.find(oldGuidForName);
            if (snapshot != overrides->instanceTables.end()) {
                for (const PrefabOverride& ov : overrides->entries) {
                    if (ov.prefabSourceId != oldGuidForName) continue;
                    if (const toml::node* value = FindNodeAtPath(snapshot->second, ov.path))
                        SetNodeAtPath(copied, ov.path, *value);
                }
            }
        }
        const std::string oldParent = copied["parent"].value_or(std::string{});
        const std::string oldParentGuid = copied["parentInstanceId"].value_or(std::string{});

        // 自身の新名は instanceId から引く (同名オブジェクトでも一意)。guid が無い旧アセットは
        // 名前フォールバックに退避する。
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
        if (!oldParentGuid.empty() && guidMap.contains(oldParentGuid)) {
            copied.insert("parentInstanceId", guidMap[oldParentGuid]);
        } else {
            copied.insert("parentInstanceId", std::string{});
        }

        // instanceId: インスタンスごとに新規 UUID を割り当てる。
        // WHY: 同じプレファブを複数インスタンス化したとき instanceId が重複すると
        //      FindByGuid が誤った GO を返すため、必ず一意の値に置き換える。
        {
            const std::string oldGuid = copied["instanceId"].value_or(std::string{});
            const std::string newGuid = guidMap.contains(oldGuid)
                ? guidMap.at(oldGuid)
                : util::GenerateUUID();
            copied.erase("instanceId");
            copied.insert("instanceId", newGuid);
        }

        // IKSolverComponent: targetName / poleName を nameMap でリマップする。
        // WHY: プレファブを複数インスタンス化すると KneePole_L → KneePole_L (1) のように
        //      名前が変わるが、chains 内の参照文字列を更新しないと Pass 3 が別インスタンスの
        //      Pole を解決してしまい、膝が逆方向に折れる。
        //      nameMap に含まれる (プレファブ内部の) 参照のみ更新し、プレファブ外部を指す
        //      参照はそのまま保持してシーン上の既存オブジェクトを参照させる。
        if (auto* ikTbl = copied["IKSolverComponent"].as_table()) {
            if (auto* chainsArr = (*ikTbl)["chains"].as_array()) {
                for (auto& chainElem : *chainsArr) {
                    auto* chainTbl = chainElem.as_table();
                    if (!chainTbl) continue;

                    // 名前リマップ (nameMap に含まれる = プレファブ内部の参照のみ対象)
                    const std::string oldTarget = (*chainTbl)["targetName"].value_or(std::string{});
                    if (!oldTarget.empty() && nameMap.contains(oldTarget)) {
                        chainTbl->erase("targetName");
                        chainTbl->insert("targetName", nameMap.at(oldTarget));
                    }

                    const std::string oldPole = (*chainTbl)["poleName"].value_or(std::string{});
                    if (!oldPole.empty() && nameMap.contains(oldPole)) {
                        chainTbl->erase("poleName");
                        chainTbl->insert("poleName", nameMap.at(oldPole));
                    }

                    // GUID リマップ (guidMap に含まれる = プレファブ内部の参照のみ対象)
                    const std::string oldTargetGuid = (*chainTbl)["targetGuid"].value_or(std::string{});
                    if (!oldTargetGuid.empty() && guidMap.contains(oldTargetGuid)) {
                        chainTbl->erase("targetGuid");
                        chainTbl->insert("targetGuid", guidMap.at(oldTargetGuid));
                    }

                    const std::string oldPoleGuid = (*chainTbl)["poleGuid"].value_or(std::string{});
                    if (!oldPoleGuid.empty() && guidMap.contains(oldPoleGuid)) {
                        chainTbl->erase("poleGuid");
                        chainTbl->insert("poleGuid", guidMap.at(oldPoleGuid));
                    }
                }
            }
        }

        // BoneComponent: skinnedMeshOwner / skinnedMeshOwnerGuid をリマップする。
        // WHY: BoneComponent が指す SkinnedMeshRenderer オーナーの識別子も
        //      インスタンス化で変わるため、別インスタンスの SMR を指さないよう更新する。
        if (auto* boneTbl = copied["BoneComponent"].as_table()) {
            const std::string oldOwner = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
            if (!oldOwner.empty() && nameMap.contains(oldOwner)) {
                boneTbl->erase("skinnedMeshOwner");
                boneTbl->insert("skinnedMeshOwner", nameMap.at(oldOwner));
            }
            const std::string oldOwnerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
            if (!oldOwnerGuid.empty() && guidMap.contains(oldOwnerGuid)) {
                boneTbl->erase("skinnedMeshOwnerGuid");
                boneTbl->insert("skinnedMeshOwnerGuid", guidMap.at(oldOwnerGuid));
            }
        }

        newObjects.push_back(std::move(copied));
    }

    toml::table doc;
    doc.insert("gameobjects", std::move(newObjects));

    std::ostringstream ss;
    ss << doc;

    if (!SceneIO::AppendObjects(scene, ss.str(), outRootEntities)) return false;

    // WHAT: 既存 AppendObjects で生成した GO に、通常 Scene ロード経路で復元できる
    //       Component のうち未追加のものだけを補完する。
    // WHY: AppendObjects は Script フィールド復元など既存のインスタンス化挙動を持つ一方、
    //      全 Component への追従が漏れやすい。通常ロードを補完元にすることで、
    //      Prefab が Scene 保存と同じ Component セットを扱えるようにする。
    scene::Scene prefabScene;
    if (SceneIO::Deserialize(prefabScene, ss.str())) {
        for (auto& srcGo : prefabScene.GameObjects()) {
            if (srcGo.instanceId.empty()) continue;
            auto* dstGo = scene.FindByGuid(srcGo.instanceId);
            if (!dstGo) continue;
            scene.CopyComponentsFrom(prefabScene, srcGo.GetID(), dstGo->GetID());
        }
    }

    // WHAT: 補完コピーした Component 内の EntityID 参照を、現在の Scene の EntityID に張り直す。
    // WHY: 一時 Scene から Component をコピーすると EntityID は一時 Scene の値を指すため、
    //      GUID / name を正として現在 Scene 側へ再解決する必要がある。
    for (auto& dstGo : scene.GameObjects()) {
        if (auto* grid = dstGo.GetComponent<scene::TerrainGridComponent>())
            grid->ResolveFromScene(scene);

        if (auto* ik = dstGo.GetComponent<scene::IKSolverComponent>()) {
            for (auto& chain : ik->chains) {
                if (!chain.targetGuid.empty()) {
                    if (auto* target = scene.FindByGuid(chain.targetGuid))
                        chain.targetEntity = target->GetID();
                } else if (!chain.targetName.empty()) {
                    if (auto* target = scene.Find(chain.targetName))
                        chain.targetEntity = target->GetID();
                }

                if (!chain.poleGuid.empty()) {
                    if (auto* pole = scene.FindByGuid(chain.poleGuid))
                        chain.poleEntity = pole->GetID();
                } else if (!chain.poleName.empty()) {
                    if (auto* pole = scene.Find(chain.poleName))
                        chain.poleEntity = pole->GetID();
                }
            }
        }
    }

    if (auto* objects = doc["gameobjects"].as_array()) {
        for (const auto& item : *objects) {
            const auto* goTbl = item.as_table();
            if (!goTbl) continue;
            const auto* boneTbl = (*goTbl)["BoneComponent"].as_table();
            if (!boneTbl) continue;

            const std::string boneGuid = (*goTbl)["instanceId"].value_or(std::string{});
            const std::string boneName = (*goTbl)["name"].value_or(std::string{});
            scene::GameObject* boneGo = !boneGuid.empty() ? scene.FindByGuid(boneGuid) : nullptr;
            if (!boneGo && !boneName.empty()) boneGo = scene.Find(boneName);
            if (!boneGo) continue;

            auto* bone = boneGo->GetComponent<scene::BoneComponent>();
            if (!bone) continue;

            const std::string ownerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
            const std::string ownerName = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
            scene::GameObject* owner = !ownerGuid.empty() ? scene.FindByGuid(ownerGuid) : nullptr;
            if (!owner && !ownerName.empty()) owner = scene.Find(ownerName);
            if (owner) bone->skinnedMeshEntity = owner->GetID();
        }
    }

    FBZZ_LOG_INFO("Instantiated prefab: %s", path.c_str());

    // WHY: インスタンス追跡のために、プレファブから生成されたルート GO に出所パスを書き込む。
    //      Assets 起点の相対パスで保存し、Apply/Revert が diskPath ではなくこの値を参照する。
    //      path は diskPath 形式 (絶対パスまたはプロジェクトルートからのフル)。
    //      Assets/ 以降の相対パスに正規化して保存する。
    const std::string::size_type assetsPos = [&] {
        const std::string marker = "/Assets/";
        auto pos = path.rfind(marker);
        return pos != std::string::npos ? pos + 1 : std::string::npos;
    }();
    const std::string relPath = (assetsPos != std::string::npos)
        ? path.substr(assetsPos)
        : path;

    for (scene::EntityID id : outRootEntities) {
        if (auto* go = scene.GetGameObject(id))
            go->prefabAssetPath = relPath;
    }

    // WHY: プロパティ単位の差分 (override) を後から計算できるよう、階層内の全 GO に
    //      「プレファブ側のどのオブジェクト由来か」を刻む。instanceId は毎回振り直される
    //      ため、これが無いとインスタンスとアセットを対応付ける手段が無い。
    for (const auto& [sourceGuid, newGuid] : guidMap) {
        if (auto* go = scene.FindByGuid(newGuid))
            go->prefabSourceId = sourceGuid;
    }

    return !outRootEntities.empty();
}

bool PrefabSerializer::Apply(const scene::Scene& scene, scene::EntityID rootEntity,
                             const std::string& projectRoot)
{
    // Apply: インスタンスの現在状態をプレファブアセットに書き戻す。
    // WHAT: prefabAssetPath は "Assets/..." 相対パスで格納されているが、
    //       CWD はエディタ起動時に exe ディレクトリへ変更されるため、
    //       ToProjectAssetDiskPath で絶対パスへ解決してから SaveSelection を呼ぶ。
    const scene::GameObject* go = scene.GetGameObject(rootEntity);
    if (!go || go->prefabAssetPath.empty()) {
        FBZZ_LOG_WARN("PrefabSerializer::Apply: entity is not a prefab instance");
        return false;
    }

    const std::string diskPath = ToProjectAssetDiskPath(projectRoot, go->prefabAssetPath);
    return SaveSelection(scene, { rootEntity }, diskPath);
}

bool PrefabSerializer::RefreshInstanceKeepingOverrides(scene::Scene& scene,
                                                       scene::EntityID rootEntity,
                                                       std::vector<scene::EntityID>& outNewRoots,
                                                       const std::string& projectRoot)
{
    // 作り直す前に現在の差分を採取する。GO は Revert で破棄されるため、
    // PrefabOverrideSet 側が値のコピーを持っている点が要 (ポインタでは追えない)。
    PrefabOverrideSet overrides;
    const bool hasOverrides = ComputePrefabOverrides(scene, rootEntity, projectRoot, overrides);
    return Revert(scene, rootEntity, outNewRoots, projectRoot,
                  hasOverrides ? &overrides : nullptr);
}

bool PrefabSerializer::Revert(scene::Scene& scene,
                              scene::EntityID rootEntity,
                              std::vector<scene::EntityID>& outNewRoots,
                              const std::string& projectRoot,
                              const PrefabOverrideSet* keepOverrides)
{
    // Revert: インスタンスをプレファブアセットの定義に戻す。
    // WHAT: 1. 旧インスタンスの Transform / parent / prefabAssetPath を保存する。
    //       2. 旧インスタンス階層全体を Destroy する。
    //       3. 保存した prefabAssetPath を絶対パスに解決して再インスタンス化する。
    //       4. 再生成ルート GO に旧 Transform (位置・回転・スケール) を適用する。
    // WHY: Revert はインスタンスの「コンポーネント・子構成」をリセットするが、
    //      ワールド上での配置 (Transform) は保持するのが自然な挙動のため。
    scene::GameObject* go = scene.GetGameObject(rootEntity);
    if (!go || go->prefabAssetPath.empty()) {
        FBZZ_LOG_WARN("PrefabSerializer::Revert: entity is not a prefab instance");
        return false;
    }

    // prefabAssetPath は相対パスのため、絶対パスへ解決してから Instantiate に渡す。
    const std::string diskPath      = ToProjectAssetDiskPath(projectRoot, go->prefabAssetPath);
    const scene::Transform savedTransform = go->transform;
    scene::GameObject* savedParent  = go->GetParent();
    const scene::EntityID savedParentId =
        savedParent ? savedParent->GetID() : scene::EntityID{};

    // 旧階層を「即時」破棄する。
    // WHY: 旧実装は GameObject::Destroy(*go, 0.0f) で破棄を破棄キューへ積んでいたが、実際の
    //      削除は次フレーム末尾まで遅延する。その間に直後の Instantiate が走ると旧階層がまだ
    //      生存しており、新インスタンスが UniqueName で "Xxx (1)" に押し出され、シーンに重複が
    //      残ってしまう。DestroyGameObject は DestroyImmediate 経由で子孫ごと同フレーム内に
    //      消すため、名前も元に戻り重複も生じない。
    scene.DestroyGameObject(rootEntity);

    // 再インスタンス化 (keepOverrides があれば展開時に差分を当て直す)
    if (!Instantiate(scene, diskPath, outNewRoots, keepOverrides) || outNewRoots.empty())
        return false;

    // 先頭ルートに旧 Transform / 親を復元する。
    // WHY: 複数ルートを持つプレファブは稀で、複数ある場合は先頭のみ位置を合わせる。
    if (auto* newGo = scene.GetGameObject(outNewRoots.front())) {
        newGo->transform = savedTransform;
        if (savedParentId.IsValid()) {
            if (auto* parent = scene.GetGameObject(savedParentId))
                newGo->SetParent(*parent);
        }
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

int PrefabSerializer::PropagateToInstances(scene::Scene& scene,
                                           const std::string& prefabAssetPath,
                                           scene::EntityID exceptRoot,
                                           const std::string& projectRoot,
                                           bool preserveOverrides)
{
    if (prefabAssetPath.empty()) return 0;

    // WHY: Revert は GO を破棄して作り直すため EntityID が無効になる。走査中に
    //      作り直すとイテレータも壊れるので、先に GUID だけを集めてから処理する。
    //      GUID はシーン内で安定しており、他インスタンスの Revert では変化しない。
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
        if (!instance) continue;   // 先行する Revert で入れ子ごと作り直された等

        std::vector<scene::EntityID> newRoots;
        const bool ok = preserveOverrides
            ? RefreshInstanceKeepingOverrides(scene, instance->GetID(), newRoots, projectRoot)
            : Revert(scene, instance->GetID(), newRoots, projectRoot);
        if (ok) ++updated;
    }
    return updated;
}

} // namespace fbzz::editor
