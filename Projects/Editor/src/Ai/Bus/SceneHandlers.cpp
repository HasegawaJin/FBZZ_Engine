/// @file    SceneHandlers.cpp
/// @brief   scene.* / node.* / component.* / prefab.* / material.* / physics.* のハンドラー。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ComponentDefaults.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @brief 反射値を検索演算子で比較する。
/// @return 型が演算子に合わない場合は false。
bool PropertyValueMatches(const JsonValue& actual, const JsonValue& expected, const std::string& op)
{
    if (op == "equals") return SerializeJson(actual) == SerializeJson(expected);
    if (op == "notEquals") return SerializeJson(actual) != SerializeJson(expected);
    if (op == "contains") {
        return actual.IsString() && expected.IsString()
            && LowerAscii(actual.AsString()).find(LowerAscii(expected.AsString())) != std::string::npos;
    }
    if (!actual.IsNumber() || !expected.IsNumber()) return false;
    if (op == "greater") return actual.AsNumber() > expected.AsNumber();
    if (op == "less") return actual.AsNumber() < expected.AsNumber();
    return false;
}

/// @brief 名前・タグ・状態・複数コンポーネント・反射プロパティを AND 条件で検索する。
Outcome FindSceneNodes(scene::Scene& activeScene, const JsonValue& payload)
{
    const std::string name = StringField(payload, "name");
    const std::string tag = StringField(payload, "tag");
    const std::string component = StringField(payload, "comp");
    const JsonValue* activeValue = payload.Find("active");
    const JsonValue* componentsValue = payload.Find("components");
    const JsonValue* propertiesValue = payload.Find("properties");
    if (name.empty() && tag.empty() && component.empty() && activeValue == nullptr
        && componentsValue == nullptr && propertiesValue == nullptr) {
        return Outcome::Err("BAD_ARG", "検索条件が必要です");
    }

    std::vector<std::string> requiredComponents;
    /// @note 旧 comp 入力との互換性を維持する。
    if (!component.empty()) requiredComponents.push_back(component);
    if (componentsValue != nullptr && componentsValue->IsArray()) {
        for (const JsonValue& value : componentsValue->AsArray()) {
            if (value.IsString()) requiredComponents.push_back(value.AsString());
        }
    }
    for (const std::string& required : requiredComponents) {
        if (!IsPublicComponentName(required)) {
            return Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + required);
        }
    }
    if (propertiesValue != nullptr && propertiesValue->IsArray()) {
        for (const JsonValue& filter : propertiesValue->AsArray()) {
            const std::string filterComponent = StringField(filter, "comp");
            if (!IsPublicComponentName(filterComponent)) {
                return Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + filterComponent);
            }
        }
    }

    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(
        limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 100,
        1,
        500);
    const std::string nameNeedle = LowerAscii(name);
    JsonValue matches = JsonValue::MakeArray();
    int total = 0;

    std::function<void(GameObject*, const std::string&)> visit;
    visit = [&](GameObject* go, const std::string& parentPath) {
        if (go == nullptr) return;
        const std::string nodePath = parentPath.empty() ? go->name : parentPath + "/" + go->name;
        const bool nameMatches = name.empty()
            || LowerAscii(go->name).find(nameNeedle) != std::string::npos;
        const bool tagMatches = tag.empty() || go->tag == tag;
        const bool activeMatches = activeValue == nullptr || !activeValue->IsBool()
            || go->activeSelf() == activeValue->AsBool();
        bool componentsMatch = true;
        for (const std::string& required : requiredComponents) {
            if (!InspectComponentType(*go, required).present) {
                componentsMatch = false;
                break;
            }
        }
        bool propertiesMatch = true;
        if (propertiesValue != nullptr && propertiesValue->IsArray()) {
            for (const JsonValue& filter : propertiesValue->AsArray()) {
                const std::string filterComponent = StringField(filter, "comp");
                const std::string field = StringField(filter, "field");
                const std::string op = StringField(filter, "op");
                const JsonValue* expected = filter.Find("value");
                const auto fields = ReadComponentFields(*go, filterComponent);
                const JsonValue* actual = fields.has_value() ? fields->Find(field) : nullptr;
                if (actual == nullptr || expected == nullptr
                    || !PropertyValueMatches(*actual, *expected, op.empty() ? "equals" : op)) {
                    propertiesMatch = false;
                    break;
                }
            }
        }
        if (nameMatches && tagMatches && activeMatches && componentsMatch && propertiesMatch) {
            ++total;
            if (static_cast<int>(matches.AsArray().size()) < limit) {
                JsonValue match = JsonValue::MakeObject();
                match.Set("id", JsonValue(go->instanceId));
                match.Set("name", JsonValue(go->name));
                match.Set("tag", JsonValue(go->tag));
                match.Set("active", JsonValue(go->activeSelf()));
                match.Set("layer", JsonValue(go->layer));
                match.Set("path", JsonValue(nodePath));
                if (GameObject* parent = go->GetParent()) match.Set("parent", JsonValue(parent->instanceId));
                matches.Push(std::move(match));
            }
        }
        for (int index = 0; index < go->GetChildCount(); ++index) {
            visit(go->GetChild(index), nodePath);
        }
    };

    for (GameObject* root : activeScene.GetRootGameObjects()) visit(root, "");

    JsonValue result = JsonValue::MakeObject();
    result.Set("matches", std::move(matches));
    result.Set("count", JsonValue(total));
    result.Set("truncated", JsonValue(total > limit));
    return Outcome::Ok(std::move(result));
}

/// @brief `SnapshotComponents` の結果を go へ復元する (delete の Undo)。
/// @note 反射不可の型は既定値のみ復元する。
void RestoreComponents(GameObject& go, const JsonValue& snapshot)
{
    if (!snapshot.IsArray()) return;
    for (const JsonValue& entry : snapshot.AsArray()) {
        const std::string type = StringField(entry, "type");
        if (type.empty()) continue;
        AddComponentByName(go, type);
        const JsonValue* fields = entry.Find("fields");
        if (fields == nullptr || !fields->IsObject()) continue;
        for (const auto& member : fields->AsObject()) {
            std::string ignored;
            WriteComponentField(go, type, member.first, member.second, ignored);
        }
    }
}

GameObject* FindColliderOwner(scene::Scene& activeScene, const physics::Collider* collider)
{
    if (collider == nullptr) return nullptr;
    for (GameObject& go : activeScene.GameObjects()) {
        GameObject* owner = nullptr;
        scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
            if (owner != nullptr) return;
            if constexpr (std::is_base_of_v<scene::ColliderComponent, T>) {
                if (T* component = go.GetComponent<T>(); component != nullptr && component->collider.get() == collider) {
                    owner = &go;
                }
            }
        });
        if (owner != nullptr) return owner;
    }
    return nullptr;
}

/// @brief Scene の比較に必要な安定 ID・Transform・公開 Component を正規化して返す。
JsonValue BuildSceneSnapshot(scene::Scene& activeScene)
{
    JsonValue nodes = JsonValue::MakeArray();
    int generatedCount = 0;
    for (GameObject& go : activeScene.GameObjects()) {
        /// @note ランタイム生成物は保存されず再生状態で差分が出るため snapshot 対象外。
        if (go.runtimeGenerated) { ++generatedCount; continue; }
        JsonValue node = JsonValue::MakeObject();
        node.Set("id", JsonValue(go.instanceId));
        node.Set("name", JsonValue(go.name));
        node.Set("tag", JsonValue(go.tag));
        node.Set("active", JsonValue(go.activeSelf()));
        node.Set("layer", JsonValue(go.layer));
        if (GameObject* parent = go.GetParent()) node.Set("parent", JsonValue(parent->instanceId));

        JsonValue transform = JsonValue::MakeObject();
        transform.Set("position", VectorToJson(go.transform.position));
        transform.Set("rotation", QuaternionToJson(go.transform.rotation));
        transform.Set("scale", VectorToJson(go.transform.scale));
        transform.Set("worldPosition", VectorToJson(go.transform.worldPosition));
        node.Set("transform", std::move(transform));
        node.Set("components", SnapshotComponents(go));
        nodes.Push(std::move(node));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("nodes", std::move(nodes));
    result.Set("count", JsonValue(static_cast<int>(activeScene.GameObjectCount()) - generatedCount));
    /// @note 除外件数は出す。snapshot に無いことを不整合と読ませないため。
    if (generatedCount > 0) result.Set("excludedGenerated", JsonValue(generatedCount));
    return result;
}

JsonValue PhysicsHitToJson(scene::Scene& activeScene, const physics::World::RaycastHit& hit)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("point", VectorToJson(hit.point));
    result.Set("normal", VectorToJson(hit.normal));
    result.Set("distance", JsonValue(hit.distance));
    if (GameObject* owner = FindColliderOwner(activeScene, hit.collider)) {
        result.Set("id", JsonValue(owner->instanceId));
        result.Set("name", JsonValue(owner->name));
    }
    return result;
}

Outcome DoPhysicsRaycast(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.runtime == nullptr || ctx.activeScene == nullptr) return Outcome::Err("NO_PHYSICS", "Physics World または Scene が未設定です");
    math::Vector3 origin;
    math::Vector3 direction;
    if (!ReadVec3(payload, "origin", origin) || !ReadVec3(payload, "direction", direction)) {
        return Outcome::Err("BAD_ARG", "origin / direction が必要です");
    }
    if (direction.LengthSq() <= 0.00000001f) return Outcome::Err("BAD_ARG", "direction はゼロベクトルにできません");
    const JsonValue* distanceValue = payload.Find("maxDistance");
    const float maxDistance = distanceValue != nullptr && distanceValue->IsNumber()
        ? static_cast<float>(distanceValue->AsNumber()) : 0.0f;
    if (maxDistance <= 0.0f) return Outcome::Err("BAD_ARG", "maxDistance は正数で指定してください");

    physics::World::RaycastHit hit{};
    const JsonValue* radiusValue = payload.Find("sphereRadius");
    const bool useSphere = radiusValue != nullptr && radiusValue->IsNumber();
    const bool didHit = useSphere
        ? ctx.runtime->GetPhysicsWorld().SphereCast(origin, static_cast<float>(radiusValue->AsNumber()), direction.Normalized(), maxDistance, hit)
        : ctx.runtime->GetPhysicsWorld().Raycast(origin, direction.Normalized(), maxDistance, hit);
    JsonValue result = JsonValue::MakeObject();
    result.Set("hit", JsonValue(didHit));
    if (didHit) result.Set("result", PhysicsHitToJson(*ctx.activeScene, hit));
    return Outcome::Ok(std::move(result));
}

Outcome DoPhysicsOverlapSphere(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.runtime == nullptr || ctx.activeScene == nullptr) return Outcome::Err("NO_PHYSICS", "Physics World または Scene が未設定です");
    math::Vector3 center;
    if (!ReadVec3(payload, "center", center)) return Outcome::Err("BAD_ARG", "center が必要です");
    const JsonValue* radiusValue = payload.Find("radius");
    const float radius = radiusValue != nullptr && radiusValue->IsNumber()
        ? static_cast<float>(radiusValue->AsNumber()) : 0.0f;
    if (radius <= 0.0f) return Outcome::Err("BAD_ARG", "radius は正数で指定してください");

    JsonValue matches = JsonValue::MakeArray();
    std::vector<std::string> emittedIds;
    for (const physics::ColliderInstance* instance : ctx.runtime->GetPhysicsWorld().OverlapSphere(center, radius)) {
        if (instance == nullptr) continue;
        GameObject* owner = FindColliderOwner(*ctx.activeScene, instance->collider);
        if (owner == nullptr || std::find(emittedIds.begin(), emittedIds.end(), owner->instanceId) != emittedIds.end()) continue;
        emittedIds.push_back(owner->instanceId);
        JsonValue match = JsonValue::MakeObject();
        match.Set("id", JsonValue(owner->instanceId));
        match.Set("name", JsonValue(owner->name));
        match.Set("isTrigger", JsonValue(instance->isTrigger));
        match.Set("layer", JsonValue(instance->layer));
        matches.Push(std::move(match));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("matches", std::move(matches));
    result.Set("count", JsonValue(static_cast<int>(emittedIds.size())));
    return Outcome::Ok(std::move(result));
}

JsonValue CollisionEventToJson(scene::Scene& activeScene, const physics::CollisionEvent& event)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("point", VectorToJson(event.point));
    result.Set("normal", VectorToJson(event.normal));
    result.Set("depth", JsonValue(event.depth));
    result.Set("trigger", JsonValue(event.isTrigger));
    if (GameObject* ownerA = FindColliderOwner(activeScene, event.colliderA)) {
        result.Set("a", JsonValue(ownerA->instanceId));
        result.Set("aName", JsonValue(ownerA->name));
    }
    if (GameObject* ownerB = FindColliderOwner(activeScene, event.colliderB)) {
        result.Set("b", JsonValue(ownerB->instanceId));
        result.Set("bName", JsonValue(ownerB->name));
    }
    return result;
}

Outcome DoPhysicsEvents(editor::EditorContext& ctx)
{
    if (ctx.runtime == nullptr || ctx.activeScene == nullptr) return Outcome::Err("NO_PHYSICS", "Physics World または Scene が未設定です");
    const physics::World& world = ctx.runtime->GetPhysicsWorld();
    JsonValue enter = JsonValue::MakeArray();
    JsonValue stay = JsonValue::MakeArray();
    JsonValue exit = JsonValue::MakeArray();
    for (const auto& event : world.GetEnterEvents()) enter.Push(CollisionEventToJson(*ctx.activeScene, event));
    for (const auto& event : world.GetStayEvents()) stay.Push(CollisionEventToJson(*ctx.activeScene, event));
    for (const auto& event : world.GetExitEvents()) exit.Push(CollisionEventToJson(*ctx.activeScene, event));
    JsonValue result = JsonValue::MakeObject();
    result.Set("enter", std::move(enter));
    result.Set("stay", std::move(stay));
    result.Set("exit", std::move(exit));
    return Outcome::Ok(std::move(result));
}

Outcome DoMaterialInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string id = StringField(payload, "id");
    GameObject* go = ctx.activeScene->FindByGuid(id);
    if (go == nullptr) return Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id);
    scene::MaterialComponent* material = go->GetComponent<scene::MaterialComponent>();
    if (material == nullptr) return Outcome::Err("NOT_PRESENT", "MaterialComponent が装着されていません");
    material->EnsureMaterialAsset();
    JsonValue overrides = JsonValue::MakeObject();
    for (const auto& [name, values] : material->paramOverrides) {
        JsonValue value = JsonValue::MakeArray();
        for (float element : values) value.Push(JsonValue(element));
        overrides.Set(name, std::move(value));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(id));
    result.Set("materialPath", JsonValue(material->materialPath));
    result.Set("shaderPath", JsonValue(material->GetShaderPath()));
    result.Set("enabled", JsonValue(material->enabled));
    result.Set("overrides", std::move(overrides));
    return Outcome::Ok(std::move(result));
}

bool IsFiniteVector(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFiniteQuaternion(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

void AddValidationIssue(JsonValue& issues, const char* severity, const char* code,
                        const GameObject& go, std::string message)
{
    JsonValue issue = JsonValue::MakeObject();
    issue.Set("severity", JsonValue(severity));
    issue.Set("code", JsonValue(code));
    issue.Set("id", JsonValue(go.instanceId));
    issue.Set("name", JsonValue(go.name));
    issue.Set("message", JsonValue(std::move(message)));
    issues.Push(std::move(issue));
}

Outcome DoSceneValidate(editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    if (ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    JsonValue issues = JsonValue::MakeArray();
    std::unordered_map<std::string, std::vector<GameObject*>> names;
    std::unordered_set<std::string> guids;
    int errors = 0;
    int warnings = 0;
    for (GameObject& go : ctx.activeScene->GameObjects()) {
        names[LowerAscii(go.name)].push_back(&go);
        if (go.instanceId.empty() || !guids.insert(go.instanceId).second) {
            AddValidationIssue(issues, "error", "DUPLICATE_OR_EMPTY_ID", go, "NodeId が空または重複しています");
            ++errors;
        }
        if (!IsFiniteVector(go.transform.position) || !IsFiniteQuaternion(go.transform.rotation)
            || !IsFiniteVector(go.transform.scale) || !IsFiniteVector(go.transform.worldPosition)) {
            AddValidationIssue(issues, "error", "NON_FINITE_TRANSFORM", go, "Transform に NaN または Infinity があります");
            ++errors;
        }
        if (std::abs(go.transform.scale.x) < 0.000001f || std::abs(go.transform.scale.y) < 0.000001f
            || std::abs(go.transform.scale.z) < 0.000001f) {
            AddValidationIssue(issues, "warning", "ZERO_SCALE", go, "Transform scale に 0 があります");
            ++warnings;
        }
        if (!go.prefabAssetPath.empty()) {
            fs::path prefabPath;
            std::string relative;
            if (!ResolveProjectFile(ctx, go.prefabAssetPath, prefabPath, relative) || !fs::is_regular_file(prefabPath)) {
                AddValidationIssue(issues, "error", "BROKEN_PREFAB", go, "Prefab 参照が見つかりません: " + go.prefabAssetPath);
                ++errors;
            }
        }
        if (auto* material = go.GetComponent<scene::MaterialComponent>(); material != nullptr && !material->materialPath.empty()) {
            fs::path materialPath;
            std::string relative;
            if (!ResolveProjectFile(ctx, material->materialPath, materialPath, relative) || !fs::is_regular_file(materialPath)) {
                AddValidationIssue(issues, "error", "BROKEN_MATERIAL", go, "Material 参照が見つかりません: " + material->materialPath);
                ++errors;
            }
        }
        if (auto* animator = go.GetComponent<scene::AnimatorComponent>(); animator != nullptr && !animator->controllerPath.empty()) {
            fs::path controllerPath;
            std::string relative;
            if (!ResolveProjectFile(ctx, animator->controllerPath, controllerPath, relative) || !fs::is_regular_file(controllerPath)) {
                AddValidationIssue(issues, "error", "BROKEN_ANIMATOR_CONTROLLER", go, "Animator Controller が見つかりません: " + animator->controllerPath);
                ++errors;
            }
        }
    }
    for (const auto& [name, objects] : names) {
        if (name.empty() || objects.size() < 2) continue;
        for (const GameObject* go : objects) {
            AddValidationIssue(issues, "warning", "DUPLICATE_NAME", *go, "同じ階層名が複数あります: " + go->name);
            ++warnings;
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("issues", std::move(issues));
    result.Set("errors", JsonValue(errors));
    result.Set("warnings", JsonValue(warnings));
    result.Set("valid", JsonValue(errors == 0));
    return Outcome::Ok(std::move(result));
}

/// @note mutating Command は `ICommand` へ変換してから Undo に載せる (ここでは実行しない)。失敗時は nullptr + err。
/// @note createdSink は生成系 Command が代表ルートの instanceId を書き込む先。detailSink は改名や budget 引き上げなど
///       「適用の副作用」を応答へ伝える先で、黙って起きる変更は必ずここへ載せる。

/// @name ワールドオーサリング (Scene 入出力 / Terrain / NavMesh / Environment / Audio / UI / Build)
/// @note 地形・NavMesh・空と光の設定はブラシとベイクでしか変えられず component.set では読めないため、ここに集約する。

/// @brief projectRoot 配下を走査して `.scene` を列挙する。
/// @note AI は「今開いているシーン」以外を知る手段が無いため列挙する。Library/Baked は生成物として除外 (asset.list と同じ方針)。
Outcome DoSceneList(editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    if (ec) return Outcome::Err("BAD_PATH", "projectRoot の解決に失敗しました");

    const fs::path currentPath = ctx.currentScenePath.empty()
        ? fs::path{} : fs::weakly_canonical(fs::path(ctx.currentScenePath), ec);
    ec.clear();

    JsonValue scenes = JsonValue::MakeArray();
    std::string currentRelative;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::path& path = it->path();
        if (it->is_directory(ec)) {
            /// @note 生成物・VCS ディレクトリへは降りない (走査時間と応答量を無駄にする)。
            const std::string name = path.filename().string();
            if (name == "Library" || name == "Baked" || name == ".git" || name == "node_modules")
                it.disable_recursion_pending();
            continue;
        }
        if (LowerAscii(path.extension().string()) != ".scene") continue;
        const fs::path relative = fs::relative(path, root, ec);
        if (ec) { ec.clear(); continue; }
        const bool isCurrent = !currentPath.empty() && path == currentPath;
        if (isCurrent) currentRelative = relative.generic_string();
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("path", JsonValue(relative.generic_string()));
        entry.Set("name", JsonValue(path.stem().string()));
        entry.Set("sizeBytes", JsonValue(static_cast<int>(fs::file_size(path, ec))));
        ec.clear();
        if (isCurrent) entry.Set("isCurrent", JsonValue(true));
        scenes.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(scenes.AsArray().size())));
    result.Set("current", JsonValue(currentRelative));
    /// @note dirty のときは scene.open が拒否される。先に知れないと 1 往復無駄になる。
    result.Set("dirty", JsonValue(ctx.sceneDirty));
    result.Set("prefabEditMode", JsonValue(ctx.InPrefabEditMode()));
    result.Set("scenes", std::move(scenes));
    return Outcome::Ok(std::move(result));
}

/// @brief アクティブシーンを切り替える。
/// @note 未保存変更は discardUnsaved を明示しない限り拒否する。確認モーダルを開くと人が操作するまでバスの drain が止まるため。
Outcome DoSceneOpen(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    if (!ctx.openScenePathImmediate) return Outcome::Err("NO_HOST", "シーンを開く機能が未接続です");
    if (ctx.InPrefabEditMode()) return Outcome::Err("PREFAB_EDIT_MODE", "Prefab 編集モード中はシーンを切り替えられません");
    if (ctx.playMode != nullptr && !ctx.playMode->IsInEditor())
        return Outcome::Err("INVALID_PLAY_STATE", "Play 中はシーンを切り替えられません。先に play_control stop を実行してください");

    const std::string requested = StringField(payload, "path");
    if (requested.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    std::filesystem::path absolute;
    std::string relative;
    if (!ResolveProjectFile(ctx, requested, absolute, relative))
        return Outcome::Err("BAD_PATH", "path は projectRoot 配下で指定してください: " + requested);
    if (LowerAscii(absolute.extension().string()) != ".scene")
        return Outcome::Err("BAD_PATH", ".scene を指定してください: " + relative);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(absolute, ec))
        return Outcome::Err("SCENE_NOT_FOUND", "シーンが見つかりません: " + relative);

    const JsonValue* discardValue = payload.Find("discardUnsaved");
    const bool discardUnsaved = discardValue != nullptr && discardValue->AsBool();
    if (ctx.sceneDirty && !discardUnsaved) {
        return Outcome::Err("SCENE_DIRTY",
            "未保存の変更があります。scene_save で保存するか discardUnsaved=true を指定してください");
    }

    if (dryRun) {
        Outcome preview = DryRunPreview("scene.open");
        preview.result.Set("path", JsonValue(relative));
        preview.result.Set("wouldDiscard", JsonValue(ctx.sceneDirty));
        return preview;
    }

    if (!ctx.openScenePathImmediate(absolute.generic_string()))
        return Outcome::Err("OPEN_FAILED", "シーンの読み込みに失敗しました: " + relative);

    JsonValue result = JsonValue::MakeObject();
    result.Set("opened", JsonValue(relative));
    result.Set("discarded", JsonValue(discardUnsaved));
    /// @note 開き直すと Undo スタックは破棄される (別シーンの EntityID を持つコマンドは復元できない)。
    result.Set("undoCleared", JsonValue(true));
    return Outcome::Ok(std::move(result));
}

/// @brief 現在のシーンを保存する。
/// @note path 省略で上書き、指定で別名保存 (以降のカレントもそのパスになる)。
Outcome DoSceneSave(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    if (!ctx.saveScenePathImmediate) return Outcome::Err("NO_HOST", "シーン保存機能が未接続です");
    if (ctx.InPrefabEditMode()) return Outcome::Err("PREFAB_EDIT_MODE", "Prefab 編集モード中はシーンを保存できません");
    /// @note Play 中に保存すると、走っているシーン (遷移後は別ファイルの中身) を currentScenePath へ書き込んでしまう。
    if (ctx.playMode != nullptr && !ctx.playMode->IsInEditor())
        return Outcome::Err("INVALID_PLAY_STATE", "Play 中はシーンを保存できません。先に play_control stop を実行してください");

    const std::string requested = StringField(payload, "path");
    std::string relative;
    std::string absolute;
    if (!requested.empty()) {
        std::filesystem::path resolved;
        if (!ResolveProjectFile(ctx, requested, resolved, relative))
            return Outcome::Err("BAD_PATH", "path は projectRoot 配下で指定してください: " + requested);
        if (LowerAscii(resolved.extension().string()) != ".scene")
            return Outcome::Err("BAD_PATH", ".scene を指定してください: " + relative);
        absolute = resolved.generic_string();
    } else if (ctx.currentScenePath.empty()) {
        /// @note 名前の決定は設計判断なので機械的に埋めない。AI に明示させる。
        return Outcome::Err("NO_SCENE_PATH", "現在のシーンは未保存です。path を指定してください");
    }

    if (dryRun) {
        Outcome preview = DryRunPreview("scene.save");
        preview.result.Set("path", JsonValue(relative.empty() ? ctx.currentScenePath : relative));
        return preview;
    }

    if (!ctx.saveScenePathImmediate(absolute))
        return Outcome::Err("SAVE_FAILED", "シーンの保存に失敗しました");

    JsonValue result = JsonValue::MakeObject();
    result.Set("saved", JsonValue(relative.empty() ? ctx.currentScenePath : relative));
    result.Set("dirty", JsonValue(ctx.sceneDirty));
    return Outcome::Ok(std::move(result));
}

/// @name Add Object プリセット

/// @brief Hierarchy の Add Object メニューと同じ登録表を返す。
/// @note 自力で組み立てさせると毎回中身が変わり、人が置いた Cube と AI が置いた Cube が別物になる。
Outcome DoPresetCatalog(const JsonValue& payload)
{
    const std::string category = LowerAscii(StringField(payload, "category"));
    JsonValue presets = JsonValue::MakeArray();
    for (const editor::ObjectPreset& preset : editor::ObjectPresetCatalog()) {
        if (!category.empty() && LowerAscii(std::string(preset.category)) != category) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(std::string(preset.id)));
        entry.Set("category", JsonValue(std::string(preset.category)));
        entry.Set("label", JsonValue(std::string(preset.label)));
        entry.Set("description", JsonValue(std::string(preset.description)));
        presets.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(presets.AsArray().size())));
    result.Set("presets", std::move(presets));
    return Outcome::Ok(std::move(result));
}

/// @brief preset.create / node.create の Undo コマンド。適用は UndoStack::Execute に任せる (dryRun で触らない)。
/// @return 検証に失敗したら nullptr と err。
std::unique_ptr<ICommand> BuildCreateObjectCommand(editor::EditorContext& ctx,
                                                   const editor::CreateObjectRequest& request,
                                                   Outcome& err,
                                                   const std::shared_ptr<std::string>& createdSink,
                                                   JsonValue* detailSink)
{
    std::string code;
    std::string message;
    if (!editor::ValidateCreateObjectRequest(ctx, request, code, message)) {
        err = Outcome::Err(code, message);
        return nullptr;
    }

    if (detailSink != nullptr) {
        if (const editor::ObjectPreset* preset = editor::FindObjectPreset(request.key)) {
            detailSink->Set("preset", JsonValue(std::string(preset->id)));
            detailSink->Set("label", JsonValue(std::string(preset->label)));
            detailSink->Set("contents", JsonValue(std::string(preset->description)));
        }
        const GameObject* parent = request.parentGuid.empty()
            ? nullptr : ctx.activeScene->FindByGuid(request.parentGuid);
        if (editor::IsInsidePrefabInstance(parent))
            detailSink->Set("warning", JsonValue(std::string(editor::PrefabInstanceChildWarning())));
    }

    return editor::MakeCreateObjectCommand(
        ctx, request, "AI: " + editor::CreateObjectLabel(request), /*applyNow*/false,
        [createdSink](const std::vector<std::string>& ids) {
            if (createdSink && !ids.empty()) *createdSink = ids.front();
        });
}

std::unique_ptr<ICommand> BuildPresetCreateCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name Create プリセット
    /// @note Hierarchy・GameObject メニュー・node.create_preset Operator と同じ MakeCreateObjectCommand を通す。
    ///       Operator を直接呼ばないのは、dryRun と transaction では適用を UndoStack::Execute に任せる必要があるため。
    if (type == "preset.create") {
        editor::CreateObjectRequest request;
        request.source      = editor::CreateObjectSource::Preset;
        request.key         = StringField(payload, "preset");
        request.parentGuid  = StringField(payload, "parent");
        request.name        = StringField(payload, "name");
        request.hasPosition = ReadVec3(payload, "position", request.position);
        return BuildCreateObjectCommand(ctx, request, err, createdSink, detailSink);
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeCreateCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @note プリセット empty と同じ実体。名前を省略すると兄弟の中で一意な "GameObject (N)" になる。
    if (type == "node.create") {
        editor::CreateObjectRequest request;
        request.source     = editor::CreateObjectSource::Preset;
        request.key        = "empty";
        request.parentGuid = StringField(payload, "parent");
        request.name       = StringField(payload, "name");
        return BuildCreateObjectCommand(ctx, request, err, createdSink, detailSink);
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeDuplicateCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.duplicate") {
        const std::string sourceId = StringField(payload, "id");
        const std::string requestedParentId = StringField(payload, "parent");
        const std::string requestedName = StringField(payload, "name");
        GameObject* source = scene->FindByGuid(sourceId);
        if (source == nullptr) {
            err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + sourceId);
            return nullptr;
        }
        if (!requestedParentId.empty()) {
            GameObject* requestedParent = scene->FindByGuid(requestedParentId);
            if (requestedParent == nullptr) {
                err = Outcome::Err("NODE_NOT_FOUND", "parent が見つかりません: " + requestedParentId);
                return nullptr;
            }
            /// @note 元ノード配下へ複製すると、コピー中に生成物を再帰走査へ取り込み得るため禁止する。
            if (requestedParent == source || requestedParent->IsDescendantOf(*source)) {
                err = Outcome::Err("BAD_DUPLICATE_PARENT", "複製先に元ノード自身またはその子孫は指定できません");
                return nullptr;
            }
        }

        auto duplicateGuid = std::make_shared<std::string>();
        auto previousSelection = std::make_shared<std::vector<EntityID>>(ctx.selectedEntities);
        editor::EditorContext* context = &ctx;
        return std::make_unique<LambdaCommand>("AI: Duplicate Node",
            [context, scene, sourceId, requestedParentId, requestedName,
             duplicateGuid, createdSink, markDirty]() {
                GameObject* sourceObject = scene->FindByGuid(sourceId);
                if (sourceObject == nullptr) return;
                EntityID parentId{};
                if (!requestedParentId.empty()) {
                    if (GameObject* parent = scene->FindByGuid(requestedParentId)) parentId = parent->GetID();
                } else if (GameObject* parent = sourceObject->GetParent()) {
                    parentId = parent->GetID();
                }

                const EntityID duplicateId = editor::DuplicateHierarchyRecursive(
                    *context,
                    sourceObject->GetID(),
                    parentId,
                    true);
                GameObject* duplicate = scene->GetGameObject(duplicateId);
                if (duplicate == nullptr) return;
                if (duplicateGuid->empty()) *duplicateGuid = duplicate->instanceId;
                else duplicate->instanceId = *duplicateGuid;
                if (!requestedName.empty()) duplicate->name = requestedName;
                if (createdSink) *createdSink = duplicate->instanceId;
                editor::SelectEntity(*context, duplicateId);
                markDirty();
            },
            [context, scene, duplicateGuid, previousSelection, markDirty]() {
                if (!duplicateGuid->empty()) {
                    if (GameObject* duplicate = scene->FindByGuid(*duplicateGuid)) {
                        scene->DestroyGameObject(duplicate->GetID());
                    }
                }
                editor::SelectEntities(*context, *previousSelection, editor::SelectionReveal::Skip);
                editor::PruneSelection(*context);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildPrefabInstantiateCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "prefab.instantiate") {
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) {
            err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
            return nullptr;
        }
        const std::string requestedPath = StringField(payload, "path");
        const std::string parentId = StringField(payload, "parent");
        std::error_code ec;
        const fs::path rootPath = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
        const fs::path prefabPath = fs::weakly_canonical(rootPath / fs::path(requestedPath), ec);
        const fs::path relativePath = fs::relative(prefabPath, rootPath, ec);
        if (ec || relativePath.empty() || relativePath.begin()->generic_string() == "..") {
            err = Outcome::Err("BAD_PATH", "Prefab は projectRoot 配下を指定してください");
            return nullptr;
        }
        if (!editor::IsInstantiableAssetExtension(LowerAscii(prefabPath.extension().string()))
            || !fs::is_regular_file(prefabPath, ec)) {
            err = Outcome::Err("PREFAB_NOT_FOUND", "Prefab が見つかりません: " + requestedPath);
            return nullptr;
        }
        if (!parentId.empty() && scene->FindByGuid(parentId) == nullptr) {
            err = Outcome::Err("NODE_NOT_FOUND", "parent が見つかりません: " + parentId);
            return nullptr;
        }

        auto rootGuids = std::make_shared<std::vector<std::string>>();
        return std::make_unique<LambdaCommand>("AI: Instantiate Prefab",
            [scene, prefabPath, parentId, rootGuids, createdSink, markDirty]() {
                std::vector<EntityID> roots;
                if (!editor::PrefabSerializer::Instantiate(*scene, prefabPath.generic_string(), roots)) return;
                for (size_t index = 0; index < roots.size(); ++index) {
                    GameObject* root = scene->GetGameObject(roots[index]);
                    if (root == nullptr) continue;
                    if (index < rootGuids->size()) root->instanceId = (*rootGuids)[index];
                    else rootGuids->push_back(root->instanceId);
                    if (!parentId.empty()) {
                        if (GameObject* parent = scene->FindByGuid(parentId)) root->SetParent(*parent);
                    }
                }
                if (createdSink && !rootGuids->empty()) *createdSink = rootGuids->front();
                markDirty();
            },
            [scene, rootGuids, markDirty]() {
                for (const std::string& guid : *rootGuids) {
                    if (GameObject* root = scene->FindByGuid(guid)) scene->DestroyGameObject(root->GetID());
                }
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildPrefabCreateCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "prefab.create") {
        /// @note 選択 (または payload.ids) から `.prefab` を作りソースをインスタンス接続する。
        ///       Hierarchy / AssetBrowser D&D と同じ SaveSelectionAndConnect を通すため、青色表示・Apply/Revert 接続も自動で付く。
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) {
            err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
            return nullptr;
        }

        /// @note 対象ノード: payload.ids があればそれを、無ければ現在の選択を使う。
        std::vector<std::string> selectionGuids;
        if (const JsonValue* ids = payload.Find("ids"); ids != nullptr && ids->IsArray()) {
            for (const JsonValue& v : ids->AsArray())
                if (v.IsString() && !v.AsString().empty()) selectionGuids.push_back(v.AsString());
        } else {
            for (EntityID id : ctx.selectedEntities)
                if (GameObject* go = scene->GetGameObject(id); go != nullptr && !go->instanceId.empty())
                    selectionGuids.push_back(go->instanceId);
        }
        if (selectionGuids.empty()) {
            err = Outcome::Err("NO_SELECTION", "prefab 化する対象がありません (ids か選択を指定してください)");
            return nullptr;
        }

        /// @note 保存先 `.prefab` パスを projectRoot 配下へ正規化する (まだ存在しなくてよい)。
        const std::string requestedPath = StringField(payload, "path");
        if (requestedPath.empty()) {
            err = Outcome::Err("BAD_PATH", "保存先 path (.prefab) を指定してください");
            return nullptr;
        }
        std::error_code ec;
        const fs::path rootPath     = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
        const fs::path prefabPath   = fs::weakly_canonical(rootPath / fs::path(requestedPath), ec);
        const fs::path relativePath = fs::relative(prefabPath, rootPath, ec);
        if (ec || relativePath.empty() || relativePath.begin()->generic_string() == "..") {
            err = Outcome::Err("BAD_PATH", "Prefab は projectRoot 配下を指定してください");
            return nullptr;
        }
        if (!editor::IsInstantiableAssetExtension(LowerAscii(prefabPath.extension().string()))) {
            err = Outcome::Err("BAD_PATH", "保存先は .prefab / .vfx 拡張子にしてください");
            return nullptr;
        }

        const std::string diskPath = prefabPath.generic_string();
        auto connectedGuids = std::make_shared<std::vector<std::string>>();
        return std::make_unique<LambdaCommand>("AI: Create Prefab",
            [scene, selectionGuids, diskPath, connectedGuids, createdSink, markDirty]() {
                /// @note guid → EntityID を毎回解決する (redo でも最新 Scene に追随)。
                std::vector<EntityID> selection;
                for (const std::string& guid : selectionGuids)
                    if (GameObject* go = scene->FindByGuid(guid)) selection.push_back(go->GetID());
                if (selection.empty()) return;

                std::vector<EntityID> roots;
                if (!editor::PrefabSerializer::SaveSelectionAndConnect(*scene, selection, diskPath, roots))
                    return;

                connectedGuids->clear();
                for (EntityID id : roots)
                    if (GameObject* go = scene->GetGameObject(id)) connectedGuids->push_back(go->instanceId);
                if (createdSink && !connectedGuids->empty()) *createdSink = connectedGuids->front();
                markDirty();
            },
            [scene, diskPath, connectedGuids, markDirty]() {
                std::error_code rec;
                std::filesystem::remove(std::filesystem::path(diskPath), rec);
                for (const std::string& guid : *connectedGuids)
                    if (GameObject* go = scene->FindByGuid(guid)) go->prefabAssetPath.clear();
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildPrefabApplyCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "prefab.apply") {
        /// @note インスタンスの現在状態を元 `.prefab` へ書き戻す (ディスクのみ変更)。
        if (ctx.projectRoot.empty()) {
            err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
            return nullptr;
        }
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (go->prefabAssetPath.empty()) {
            err = Outcome::Err("NOT_PREFAB_INSTANCE", "prefab インスタンスではありません: " + id);
            return nullptr;
        }

        const std::string projectRoot = ctx.projectRoot;
        auto beforeContent = std::make_shared<std::string>();
        auto existedBefore = std::make_shared<bool>(false);
        /// @note Propagate は他インスタンスを作り直すため、アセットだけ戻してもシーンは新定義のまま残る。シーンのスナップショットも併せて持つ。
        auto beforeScene = std::make_shared<std::string>();
        auto afterScene  = std::make_shared<std::string>();
        auto captured    = std::make_shared<bool>(false);
        return std::make_unique<LambdaCommand>("AI: Apply Prefab",
            [scene, id, projectRoot, beforeContent, existedBefore,
             beforeScene, afterScene, captured, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr || target->prefabAssetPath.empty()) return;
                if (!*captured) {
                    /// @note Apply 前のアセット内容とシーンを退避して undo で書き戻せるようにする。
                    const std::string disk =
                        editor::ToProjectAssetDiskPath(projectRoot, target->prefabAssetPath);
                    *existedBefore = util::FileSystem::ReadText(disk, *beforeContent);
                    *beforeScene = editor::SceneIO::Serialize(*scene);
                    /// @note Apply だけでは同じ `.prefab` の他インスタンスが新定義へ揃わず、画面の実体だけ古いままになる。
                    (void)editor::PrefabSerializer::ApplyAndPropagate(
                        *scene, target->GetID(), projectRoot);
                    *afterScene = editor::SceneIO::Serialize(*scene);
                    *captured = true;
                } else if (!afterScene->empty()) {
                    editor::SceneIO::Deserialize(*scene, *afterScene);
                }
                markDirty();
            },
            [scene, id, projectRoot, beforeContent, existedBefore,
             beforeScene, markDirty]() {
                /// @note アセットを戻し、伝播で作り直された実体もシーンごと戻す。
                if (GameObject* target = scene->FindByGuid(id);
                    target != nullptr && !target->prefabAssetPath.empty()) {
                    const std::string disk =
                        editor::ToProjectAssetDiskPath(projectRoot, target->prefabAssetPath);
                    if (*existedBefore) util::FileSystem::WriteText(disk, *beforeContent);
                    else std::filesystem::remove(std::filesystem::path(disk));
                }
                if (!beforeScene->empty()) editor::SceneIO::Deserialize(*scene, *beforeScene);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildPrefabRevertCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "prefab.revert") {
        /// @note インスタンスを元 `.prefab` の定義へ戻す。シーン変更のため前後スナップショットで undo する。
        if (ctx.projectRoot.empty()) {
            err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
            return nullptr;
        }
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (go->prefabAssetPath.empty()) {
            err = Outcome::Err("NOT_PREFAB_INSTANCE", "prefab インスタンスではありません: " + id);
            return nullptr;
        }

        const std::string projectRoot = ctx.projectRoot;
        auto before   = std::make_shared<std::string>();
        auto after    = std::make_shared<std::string>();
        auto captured = std::make_shared<bool>(false);
        return std::make_unique<LambdaCommand>("AI: Revert Prefab",
            [scene, id, projectRoot, before, after, captured, markDirty]() {
                /// @note 初回は revert を実行して前後スナップショットを取り、redo 以降は after を復元する。
                if (!*captured) {
                    *before = editor::SceneIO::Serialize(*scene);
                    GameObject* target = scene->FindByGuid(id);
                    if (target == nullptr) return;
                    std::vector<EntityID> newRoots;
                    if (!editor::PrefabSerializer::Revert(*scene, target->GetID(), newRoots, projectRoot))
                        return;
                    *after = editor::SceneIO::Serialize(*scene);
                    *captured = true;
                } else if (!after->empty()) {
                    editor::SceneIO::Deserialize(*scene, *after);
                }
                markDirty();
            },
            [scene, before, captured, markDirty]() {
                if (*captured && !before->empty()) editor::SceneIO::Deserialize(*scene, *before);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildMaterialAssignCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "material.assign") {
        namespace fs = std::filesystem;
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        fs::path materialFile;
        std::string materialPath;
        if (!ResolveProjectFile(ctx, StringField(payload, "path"), materialFile, materialPath)
            || LowerAscii(materialFile.extension().string()) != ".mat" || !fs::is_regular_file(materialFile)) {
            err = Outcome::Err("MATERIAL_NOT_FOUND", "projectRoot 配下の .mat を指定してください");
            return nullptr;
        }
        scene::MaterialComponent* existing = go->GetComponent<scene::MaterialComponent>();
        const bool wasPresent = existing != nullptr;
        const std::string oldPath = existing != nullptr ? existing->materialPath : std::string{};
        return std::make_unique<LambdaCommand>("AI: Assign Material",
            [scene, id, materialPath, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr) return;
                scene::MaterialComponent* material = target->GetComponent<scene::MaterialComponent>();
                if (material == nullptr) material = &target->AddComponent<scene::MaterialComponent>(scene::MaterialComponent{});
                material->materialPath = materialPath;
                material->materialAsset = {};
                material->material.reset();
                markDirty();
            },
            [scene, id, wasPresent, oldPath, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr) return;
                if (!wasPresent) target->RemoveComponent<scene::MaterialComponent>();
                else if (auto* material = target->GetComponent<scene::MaterialComponent>()) {
                    material->materialPath = oldPath;
                    material->materialAsset = {};
                    material->material.reset();
                }
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildMaterialOverrideCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "material.override") {
        const std::string id = StringField(payload, "id");
        const std::string parameter = StringField(payload, "parameter");
        const JsonValue* value = payload.Find("value");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        auto* material = go->GetComponent<scene::MaterialComponent>();
        if (material == nullptr) { err = Outcome::Err("NOT_PRESENT", "MaterialComponent が装着されていません"); return nullptr; }
        if (parameter.empty() || value == nullptr || !value->IsArray() || value->AsArray().empty() || value->AsArray().size() > 4) {
            err = Outcome::Err("BAD_ARG", "parameter と1〜4要素の value が必要です");
            return nullptr;
        }
        std::vector<float> newValue;
        for (const JsonValue& element : value->AsArray()) {
            if (!element.IsNumber()) { err = Outcome::Err("BAD_ARG", "value は数値配列です"); return nullptr; }
            newValue.push_back(static_cast<float>(element.AsNumber()));
        }
        const auto oldIterator = material->paramOverrides.find(parameter);
        const bool hadOldValue = oldIterator != material->paramOverrides.end();
        const std::vector<float> oldValue = hadOldValue ? oldIterator->second : std::vector<float>{};
        return std::make_unique<LambdaCommand>("AI: Override Material Parameter",
            [scene, id, parameter, newValue, markDirty]() {
                if (GameObject* target = scene->FindByGuid(id)) {
                    if (auto* component = target->GetComponent<scene::MaterialComponent>()) component->paramOverrides[parameter] = newValue;
                }
                markDirty();
            },
            [scene, id, parameter, hadOldValue, oldValue, markDirty]() {
                if (GameObject* target = scene->FindByGuid(id)) {
                    if (auto* component = target->GetComponent<scene::MaterialComponent>()) {
                        if (hadOldValue) component->paramOverrides[parameter] = oldValue;
                        else component->paramOverrides.erase(parameter);
                    }
                }
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildMaterialSetShaderCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "material.asset.setShader") {
        namespace fs = std::filesystem;
        fs::path materialFile;
        std::string materialPath;
        fs::path shaderFile;
        std::string shaderPath;
        if (!ResolveProjectFile(ctx, StringField(payload, "path"), materialFile, materialPath)
            || LowerAscii(materialFile.extension().string()) != ".mat" || !fs::is_regular_file(materialFile)) {
            err = Outcome::Err("MATERIAL_NOT_FOUND", "projectRoot 配下の .mat を指定してください");
            return nullptr;
        }
        if (!ResolveProjectFile(ctx, StringField(payload, "shaderPath"), shaderFile, shaderPath)
            || LowerAscii(shaderFile.extension().string()) != ".hlsl" || !fs::is_regular_file(shaderFile)) {
            err = Outcome::Err("SHADER_NOT_FOUND", "projectRoot 配下の .hlsl を指定してください");
            return nullptr;
        }
        asset::MaterialAsset oldAsset;
        if (!asset::LoadMaterialAssetFromFile(materialFile.generic_string(), oldAsset)) {
            err = Outcome::Err("MATERIAL_READ_FAILED", "MaterialAsset を読み取れません: " + materialPath);
            return nullptr;
        }
        asset::MaterialAsset newAsset = oldAsset;
        newAsset.shaderPath = shaderPath;
        editor::EditorContext* context = &ctx;
        /// @note Unload はスロットを解放して世代を進めるため、配ってある AssetHandle が一斉に死ぬ。ReloadPath はハンドルを保ったまま実体を差し替える。
        const auto applyToDisk = [context](const fs::path& file,
                                           const asset::MaterialAsset& value) {
            if (!asset::SaveMaterialAssetToFile(file.generic_string(), value)) return;
            asset::AssetManager::ReloadPath(file.generic_string());
            context->requestAssetBrowserRefresh = true;
        };
        return std::make_unique<LambdaCommand>("AI: Set Material Shader",
            [applyToDisk, materialFile, newAsset]() { applyToDisk(materialFile, newAsset); },
            [applyToDisk, materialFile, oldAsset]() { applyToDisk(materialFile, oldAsset); });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeRenameCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @note 実体は SceneEditUtils の `MakeRenameNodeCommand`。Hierarchy パネルのインラインリネームも同じ経路を通るため Undo の重さ・復元の仕方は変わらない。
    /// @see Docs/design/editor-operator-model.md
    if (type == "node.rename") {
        const std::string id = StringField(payload, "id");
        const std::string name = StringField(payload, "name");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (name.empty()) { err = Outcome::Err("BAD_ARG", "name が空です"); return nullptr; }
        /// @note 履歴ラベルだけ AI 用にする (editor_get_undo_history で自分の編集を識別できる)。
        /// @note dryRun でもここまでは通るため適用は UndoStack::Execute に任せる (applyNow=true だと dryRun が実際に書き換えてしまう)。
        auto command = MakeRenameNodeCommand(ctx, go->GetID(), name,
                                             "AI: Rename Node", /*applyNow*/false);
        if (!command) { err = Outcome::Err("NO_CHANGE", "名前が変わりません: " + name); return nullptr; }
        return command;
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeSetActiveCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.setActive") {
        const std::string id = StringField(payload, "id");
        const JsonValue* activeValue = payload.Find("active");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (activeValue == nullptr || !activeValue->IsBool()) { err = Outcome::Err("BAD_ARG", "active (真偽) が必要です"); return nullptr; }
        const bool active = activeValue->AsBool();
        const bool oldActive = go->activeSelf();
        return std::make_unique<LambdaCommand>("AI: Set Node Active",
            [scene, id, active, markDirty]()    { if (GameObject* g = scene->FindByGuid(id)) g->SetActive(active); markDirty(); },
            [scene, id, oldActive, markDirty]() { if (GameObject* g = scene->FindByGuid(id)) g->SetActive(oldActive); markDirty(); });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeSetTagCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.setTag") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const JsonValue* tagValue = payload.Find("tag");
        if (tagValue == nullptr || !tagValue->IsString() || tagValue->AsString().empty()) { err = Outcome::Err("BAD_ARG", "tag (文字列) が必要です"); return nullptr; }
        const std::string tag = tagValue->AsString();
        auto oldTag = std::make_shared<std::string>(go->tag);
        return std::make_unique<LambdaCommand>("AI: Set Node Tag",
            [scene, id, tag, markDirty]()    { if (GameObject* g = scene->FindByGuid(id)) g->tag = tag; markDirty(); },
            [scene, id, oldTag, markDirty]() { if (GameObject* g = scene->FindByGuid(id)) g->tag = *oldTag; markDirty(); });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeSetLayerCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.setLayer") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const JsonValue* layerValue = payload.Find("layer");
        if (layerValue == nullptr || !layerValue->IsNumber()) { err = Outcome::Err("BAD_ARG", "layer (整数) が必要です"); return nullptr; }
        const int layer = layerValue->AsInt();
        if (layer < 0 || layer > 31) { err = Outcome::Err("BAD_ARG", "layer は 0〜31 です"); return nullptr; }
        const int oldLayer = go->layer;
        return std::make_unique<LambdaCommand>("AI: Set Node Layer",
            [scene, id, layer, markDirty]()    { if (GameObject* g = scene->FindByGuid(id)) g->layer = layer; markDirty(); },
            [scene, id, oldLayer, markDirty]() { if (GameObject* g = scene->FindByGuid(id)) g->layer = oldLayer; markDirty(); });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeReparentCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.reparent") {
        const std::string id = StringField(payload, "id");
        const std::string parent = StringField(payload, "parent");
        const JsonValue* indexValue = payload.Find("index");
        const int index = (indexValue != nullptr && indexValue->IsNumber()) ? indexValue->AsInt() : 0;
        GameObject* go = scene->FindByGuid(id);
        GameObject* parentGo = scene->FindByGuid(parent);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (parentGo == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "parent が見つかりません: " + parent); return nullptr; }
        if (go == parentGo || parentGo->IsDescendantOf(*go)) { err = Outcome::Err("BAD_REPARENT", "循環する親子関係です"); return nullptr; }
        GameObject* oldParent = go->GetParent();
        auto oldParentGuid = std::make_shared<std::string>(oldParent ? oldParent->instanceId : std::string{});
        const int oldSibling = go->GetSiblingIndex();
        return std::make_unique<LambdaCommand>("AI: Reparent Node",
            [scene, id, parent, index, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                GameObject* p = scene->FindByGuid(parent);
                if (g && p) { g->SetParent(*p); g->SetSiblingIndex(index); }
                markDirty();
            },
            [scene, id, oldParentGuid, oldSibling, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                if (oldParentGuid->empty()) g->ClearParent();
                else if (GameObject* p = scene->FindByGuid(*oldParentGuid)) g->SetParent(*p);
                g->SetSiblingIndex(oldSibling);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildTransformSetCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "transform.set") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        math::Vector3 pos, rotDeg, scl;
        const bool hasPos   = ReadVec3(payload, "pos", pos);
        const bool hasRot   = ReadVec3(payload, "rot", rotDeg);
        const bool hasScale = ReadVec3(payload, "scale", scl);
        if (!hasPos && !hasRot && !hasScale) { err = Outcome::Err("BAD_ARG", "pos/rot/scale のいずれかが必要です"); return nullptr; }
        const math::Vector3    oldPos = go->transform.position;
        const math::Quaternion oldRot = go->transform.rotation;
        const math::Vector3    oldScale = go->transform.scale;
        /// @note rot は Euler 度で受け取り Quaternion へ変換する (Inspector と同じ表現)。
        const math::Quaternion newRot = hasRot
            ? math::Quaternion::FromEuler({ math::ToRad(rotDeg.x), math::ToRad(rotDeg.y), math::ToRad(rotDeg.z) })
            : oldRot;
        return std::make_unique<LambdaCommand>("AI: Set Transform",
            [scene, id, hasPos, hasRot, hasScale, pos, newRot, scl, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                if (hasPos)   g->transform.position = pos;
                if (hasRot)   g->transform.rotation = newRot;
                if (hasScale) g->transform.scale = scl;
                markDirty();
            },
            [scene, id, hasPos, hasRot, hasScale, oldPos, oldRot, oldScale, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                if (hasPos)   g->transform.position = oldPos;
                if (hasRot)   g->transform.rotation = oldRot;
                if (hasScale) g->transform.scale = oldScale;
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNodeDeleteCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "node.delete") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        /// @note 単一ノードのスナップショット (子階層は Undo 対象外。MVP の制約)。
        auto snapshot = std::make_shared<JsonValue>(SnapshotComponents(*go));
        auto name = std::make_shared<std::string>(go->name);
        auto tag = std::make_shared<std::string>(go->tag);
        const int  layer  = go->layer;
        const bool active = go->activeSelf();
        const math::Vector3    pos = go->transform.position;
        const math::Quaternion rot = go->transform.rotation;
        const math::Vector3    scl = go->transform.scale;
        GameObject* parent = go->GetParent();
        auto parentGuid = std::make_shared<std::string>(parent ? parent->instanceId : std::string{});
        const int sibling = go->GetSiblingIndex();
        return std::make_unique<LambdaCommand>("AI: Delete Node",
            [scene, id, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id)) scene->DestroyGameObject(g->GetID());
                markDirty();
            },
            [scene, id, name, tag, layer, active, pos, rot, scl, parentGuid, sibling, snapshot, markDirty]() {
                GameObject& go = scene->CreateGameObject(*name);
                /// @note NodeId を保って再生成し、参照の安定性を維持する。
                go.instanceId = id;
                go.tag = *tag;
                go.layer = layer;
                go.transform.position = pos;
                go.transform.rotation = rot;
                go.transform.scale = scl;
                if (!parentGuid->empty()) { if (GameObject* p = scene->FindByGuid(*parentGuid)) go.SetParent(*p); }
                go.SetSiblingIndex(sibling);
                RestoreComponents(go, *snapshot);
                /// @note active は SetParent 後に適用し、activeInHierarchy の再計算を正しく行う。
                go.SetActive(active);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildComponentAddCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "component.add") {
        const std::string id = StringField(payload, "id");
        const std::string comp = StringField(payload, "comp");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const TypeInfo info = InspectComponentType(*go, comp);
        if (!info.known)   { err = Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + comp); return nullptr; }
        if (!info.addable) { err = Outcome::Err("NOT_ADDABLE", "追加できないコンポーネント: " + comp); return nullptr; }
        /// @note Inspector の Add Component と同じ既定値・依存で付ける。Undo は依存も含めて増えた型だけを外す
        ///       (元から在ったものは消さない)。
        auto added = std::make_shared<std::vector<std::string>>();
        return std::make_unique<LambdaCommand>("AI: Add Component",
            [scene, id, comp, added, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                *added = editor::AddRegisteredComponentByNameTracked(*g, comp);
                markDirty();
            },
            [scene, id, added, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id))
                    for (auto it = added->rbegin(); it != added->rend(); ++it)
                        RemoveComponentByName(*g, *it);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildComponentRemoveCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "component.remove") {
        const std::string id = StringField(payload, "id");
        const std::string comp = StringField(payload, "comp");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const TypeInfo info = InspectComponentType(*go, comp);
        if (!info.known)   { err = Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + comp); return nullptr; }
        if (!info.present) { err = Outcome::Err("NOT_PRESENT", "そのコンポーネントは装着されていません: " + comp); return nullptr; }
        /// @note Inspector の Remove と同じ依存規則で止める。AI 経路だけ壊れた組み合わせを作れないようにする。
        std::string blocker;
        scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
            if (std::string_view(Reg::serializedName) == comp) blocker = FindComponentRemovalBlocker(*go, typeid(T));
        });
        if (!blocker.empty()) {
            err = Outcome::Err("REQUIRED_BY", comp + " は " + blocker + " が必要としているため外せません");
            return nullptr;
        }
        /// @note Undo 用に単一エントリのスナップショットを作る (RestoreComponents で再構築)。
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("type", JsonValue(comp));
        if (auto fields = ReadComponentFields(*go, comp)) entry.Set("fields", *fields);
        auto snapshot = std::make_shared<JsonValue>(JsonValue::MakeArray());
        snapshot->Push(std::move(entry));
        return std::make_unique<LambdaCommand>("AI: Remove Component",
            [scene, id, comp, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id)) RemoveComponentByName(*g, comp);
                markDirty();
            },
            [scene, id, snapshot, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id)) RestoreComponents(*g, *snapshot);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildComponentSetCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "component.set") {
        const std::string id = StringField(payload, "id");
        const std::string comp = StringField(payload, "comp");
        const std::string field = StringField(payload, "field");
        const JsonValue* valuePtr = payload.Find("value");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (field.empty() || valuePtr == nullptr) { err = Outcome::Err("BAD_ARG", "field / value が必要です"); return nullptr; }
        const TypeInfo info = InspectComponentType(*go, comp);
        if (!info.known)       { err = Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + comp); return nullptr; }
        if (!info.present)     { err = Outcome::Err("NOT_PRESENT", "そのコンポーネントは装着されていません: " + comp); return nullptr; }
        if (!info.reflectable) { err = Outcome::Err("NOT_REFLECTABLE", "反射できないコンポーネント: " + comp); return nullptr; }
        auto oldFields = ReadComponentFields(*go, comp);
        const JsonValue* oldValue = oldFields ? oldFields->Find(field) : nullptr;
        if (oldValue == nullptr) { err = Outcome::Err("UNKNOWN_FIELD", "未知のフィールド: " + field); return nullptr; }
        if (!CompatibleJsonType(*oldValue, *valuePtr)) { err = Outcome::Err("TYPE_MISMATCH", "field '" + field + "' の型が一致しません"); return nullptr; }
        /// @note Sprite 参照は解決できないと静かにアトラス全面になる。書き込む前にここで検証する。
        if (valuePtr->IsString()) {
            const std::string reference = valuePtr->AsString();
            std::string spriteTexturePath;
            std::string spriteToken;
            if (asset::ParseSpriteReference(reference, spriteTexturePath, spriteToken)
                && asset::LookupSpriteId(spriteTexturePath, spriteToken).empty()
                && !IsImplicitSingleSprite(spriteTexturePath, spriteToken)) {
                err = Outcome::Err("SPRITE_NOT_FOUND",
                    "この Sprite 参照は解決できません: " + reference
                    + " (sprite_list で ID / 名前を確認してください)");
                return nullptr;
            }
        }
        auto newVal = std::make_shared<JsonValue>(*valuePtr);
        auto oldVal = std::make_shared<JsonValue>(*oldValue);
        return std::make_unique<LambdaCommand>("AI: Set Component Field",
            [scene, id, comp, field, newVal, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                std::string ignored;
                WriteComponentField(*g, comp, field, *newVal, ignored);
                markDirty();
            },
            [scene, id, comp, field, oldVal, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                std::string ignored;
                WriteComponentField(*g, comp, field, *oldVal, ignored);
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

Outcome DoSceneTreeQuery(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    Outcome outcome = Outcome::Err("UNKNOWN", "未対応の要求です");
    if (activeScene == nullptr) { outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); }
    else {
        /// @note includeGenerated: 実行時に作られた GO (VFX ノード実体、Water splash) をツリーへ含めるか。既定は含めない。
        ///       生成物は編集しても保存されないため、混ぜると context を無駄に消費するだけの編集を誘発する。
        const JsonValue* includeValue = payload.Find("includeGenerated");
        const bool includeGenerated = includeValue != nullptr && includeValue->AsBool();
        struct Builder {
            static JsonValue Node(GameObject* go, bool includeGenerated) {
                JsonValue node = JsonValue::MakeObject();
                node.Set("id", JsonValue(go->instanceId));
                node.Set("name", JsonValue(go->name));
                node.Set("tag", JsonValue(go->tag));
                node.Set("active", JsonValue(go->activeSelf()));
                node.Set("layer", JsonValue(go->layer));
                /// @note 生成物を含める指定のときだけ印を付ける。既定応答では常に false になるため出さず冗長さを避ける。
                if (includeGenerated && go->runtimeGenerated)
                    node.Set("runtimeGenerated", JsonValue(true));
                JsonValue children = JsonValue::MakeArray();
                int hiddenGenerated = 0;
                for (int i = 0; i < go->GetChildCount(); ++i) {
                    GameObject* child = go->GetChild(i);
                    if (child == nullptr) continue;
                    if (!includeGenerated && child->runtimeGenerated) {
                        ++hiddenGenerated;
                        continue;
                    }
                    children.Push(Node(child, includeGenerated));
                }
                node.Set("children", std::move(children));
                /// @note 「出ていない = 存在しない」と読ませないための件数。
                if (hiddenGenerated > 0)
                    node.Set("hiddenGeneratedChildren", JsonValue(hiddenGenerated));
                return node;
            }
        };
        JsonValue roots = JsonValue::MakeArray();
        for (GameObject* go : activeScene->GetRootGameObjects()) {
            if (go == nullptr) continue;
            if (!includeGenerated && go->runtimeGenerated) continue;
            roots.Push(Builder::Node(go, includeGenerated));
        }
        JsonValue result = JsonValue::MakeObject();
        result.Set("roots", std::move(roots));
        result.Set("includeGenerated", JsonValue(includeGenerated));
        outcome = Outcome::Ok(std::move(result));
    }
    return outcome;
}

Outcome DoNodeComponentsQuery(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    Outcome outcome = Outcome::Err("UNKNOWN", "未対応の要求です");
    const std::string nodeId = StringField(payload, "id");
    GameObject* go = (activeScene != nullptr) ? activeScene->FindByGuid(nodeId) : nullptr;
    if (go == nullptr) { outcome = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + nodeId); }
    else {
        JsonValue result = JsonValue::MakeObject();
        result.Set("id", JsonValue(nodeId));
        result.Set("name", JsonValue(go->name));
        result.Set("tag", JsonValue(go->tag));
        result.Set("active", JsonValue(go->activeSelf()));
        result.Set("layer", JsonValue(go->layer));
        result.Set("components", SnapshotComponents(*go));
        outcome = Outcome::Ok(std::move(result));
    }
    return outcome;
}
} // namespace

void RegisterSceneHandlers(BusHandlerTable& table)
{
    table.AddQuery("scene.tree", [](BusCall& call) { return DoSceneTreeQuery(call.ctx, call.payload); });
    table.AddQuery("scene.find", [](BusCall& call) {
        if (call.ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
        return FindSceneNodes(*call.ctx.activeScene, call.payload);
    });
    table.AddQuery("scene.snapshot", [](BusCall& call) {
        if (call.ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
        return Outcome::Ok(BuildSceneSnapshot(*call.ctx.activeScene));
    });
    table.AddQuery("scene.validate", [](BusCall& call) { return DoSceneValidate(call.ctx); });
    table.AddQuery("scene.list", [](BusCall& call) { return DoSceneList(call.ctx); });
    table.AddQuery("node.components", [](BusCall& call) { return DoNodeComponentsQuery(call.ctx, call.payload); });
    table.AddQuery("material.inspect", [](BusCall& call) { return DoMaterialInspect(call.ctx, call.payload); });
    table.AddQuery("physics.raycast", [](BusCall& call) { return DoPhysicsRaycast(call.ctx, call.payload); });
    table.AddQuery("physics.overlapSphere", [](BusCall& call) { return DoPhysicsOverlapSphere(call.ctx, call.payload); });
    table.AddQuery("physics.events", [](BusCall& call) { return DoPhysicsEvents(call.ctx); });
    table.AddQuery("preset.catalog", [](BusCall& call) { return DoPresetCatalog(call.payload); });

    /// @note シーンの入出力は Undo でシーンが閉じる方が事故になるため UndoStack へ載せない。
    table.AddCommand("scene.open", [](BusCall& call) { return DoSceneOpen(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("scene.save", [](BusCall& call) { return DoSceneSave(call.ctx, call.payload, call.dryRun); });

    table.AddBuilder("preset.create", BuildPresetCreateCommand, true);
    table.AddBuilder("node.create", BuildNodeCreateCommand, true);
    table.AddBuilder("node.duplicate", BuildNodeDuplicateCommand, true);
    table.AddBuilder("prefab.instantiate", BuildPrefabInstantiateCommand, true);
    table.AddBuilder("prefab.create", BuildPrefabCreateCommand, true);
    table.AddBuilder("prefab.apply", BuildPrefabApplyCommand);
    table.AddBuilder("prefab.revert", BuildPrefabRevertCommand);
    table.AddBuilder("material.assign", BuildMaterialAssignCommand);
    table.AddBuilder("material.override", BuildMaterialOverrideCommand);
    table.AddBuilder("material.asset.setShader", BuildMaterialSetShaderCommand);
    table.AddBuilder("node.rename", BuildNodeRenameCommand);
    table.AddBuilder("node.setActive", BuildNodeSetActiveCommand);
    table.AddBuilder("node.setTag", BuildNodeSetTagCommand);
    table.AddBuilder("node.setLayer", BuildNodeSetLayerCommand);
    table.AddBuilder("node.reparent", BuildNodeReparentCommand);
    table.AddBuilder("transform.set", BuildTransformSetCommand);
    table.AddBuilder("node.delete", BuildNodeDeleteCommand);
    table.AddBuilder("component.add", BuildComponentAddCommand);
    table.AddBuilder("component.remove", BuildComponentRemoveCommand);
    table.AddBuilder("component.set", BuildComponentSetCommand);
}

} // namespace fbzz::editor::ai::bus
