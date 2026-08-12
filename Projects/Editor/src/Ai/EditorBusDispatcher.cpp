// FBZZ Engine
// EditorBusDispatcher.cpp | fbzz::editor::ai
// Editor Command Bus のメインスレッド処理。Query/Command を Scene・UndoStack・Renderer へ写像する。
#include <Editor/Ai/EditorBusDispatcher.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Ai/PreviewMetrics.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphEditor/GraphSubgraphOps.hpp>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Editor/VFXEditor/Services/VFXRecipeLibrary.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>
#include <Engine/Asset/TextureAnalysis.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ParticleCurvePresets.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXParameterRuntime.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::ai {

namespace {

using scene::GameObject;
using scene::EntityID; // ローカル変数 scene が名前空間 scene:: を隠すため、EntityID は事前に using する

// ── 小さな JSON 取り出しヘルパ ───────────────────────────────────────────────
std::string StringField(const JsonValue& obj, const char* key)
{
    const JsonValue* v = obj.Find(key);
    return (v != nullptr && v->IsString()) ? v->AsString() : std::string{};
}

// payload の key を長さ3の数値配列として Vector3 (度/座標) に読む。
bool ReadVec3(const JsonValue& obj, const char* key, math::Vector3& out)
{
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || !v->IsArray() || v->AsArray().size() < 3) return false;
    const auto& a = v->AsArray();
    if (!a[0].IsNumber() || !a[1].IsNumber() || !a[2].IsNumber()) return false;
    out = { static_cast<float>(a[0].AsNumber()), static_cast<float>(a[1].AsNumber()), static_cast<float>(a[2].AsNumber()) };
    return true;
}

// ── コンポーネント名ベースの型操作 (ForEachRegisteredComponent 経由) ──────────
struct TypeInfo { bool known = false; bool present = false; bool addable = false; bool reflectable = false; };

// AIへ公開可能な登録コンポーネント名かを調べる。Hidden型は内部実装なので検索条件にも露出しない。
bool IsPublicComponentName(std::string_view componentName)
{
    bool known = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden) {
            if (std::string_view(Reg::serializedName) == componentName) known = true;
        }
    });
    return known;
}

TypeInfo InspectComponentType(GameObject& go, const std::string& comp)
{
    TypeInfo info;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (info.known) return;
        if (std::string_view(Reg::serializedName) != comp) return;
        info.known = true;
        info.present = (go.GetComponent<T>() != nullptr);
        // AddComponent は値渡し (move) するため、既定構築かつ move 可能な型のみ追加可能とみなす。
        if constexpr (Reg::addable && std::is_default_constructible_v<T> && std::is_move_constructible_v<T>) info.addable = true;
        if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) info.reflectable = true;
    });
    return info;
}

void AddComponentByName(GameObject& go, const std::string& comp)
{
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (std::string_view(Reg::serializedName) != comp) return;
        if constexpr (std::is_default_constructible_v<T> && std::is_move_constructible_v<T>) {
            if (go.GetComponent<T>() == nullptr) go.AddComponent<T>(T{});
        }
    });
}

void RemoveComponentByName(GameObject& go, const std::string& comp)
{
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (std::string_view(Reg::serializedName) != comp) return;
        go.RemoveComponent<T>();
    });
}

// 指定コンポーネントの反射フィールドを JSON オブジェクトで返す (型不明/未装着/非反射は nullopt)。
std::optional<JsonValue> ReadComponentFields(GameObject& go, const std::string& comp)
{
    std::optional<JsonValue> result;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (result.has_value()) return;
        if (std::string_view(Reg::serializedName) != comp) return;
        if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) {
            if (T* component = go.GetComponent<T>()) {
                JsonReadReflector reader;
                component->Reflect(reader);
                result = reader.Result();
            }
        }
    });
    return result;
}

// 1フィールドを JSON 値で上書きする。成功で true。失敗理由は errMsg。
bool WriteComponentField(GameObject& go, const std::string& comp, const std::string& field,
                         const JsonValue& value, std::string& errMsg)
{
    bool applied = false;
    std::string error;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (applied || !error.empty()) return;
        if (std::string_view(Reg::serializedName) != comp) return;
        if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) {
            if (T* component = go.GetComponent<T>()) {
                JsonWriteReflector writer(field, value);
                component->Reflect(writer);
                if (writer.Applied()) applied = true;
                else if (!writer.Error().empty()) error = writer.Error();
                else error = "field '" + field + "' が見つかりません";
            }
        }
    });
    errMsg = error;
    return applied;
}

// 全反射コンポーネントを [{type, fields}] 配列に写す (node.components / delete スナップショット共用)。
JsonValue SnapshotComponents(GameObject& go)
{
    JsonValue array = JsonValue::MakeArray();
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::inspectorMode == scene::ComponentInspectorMode::Hidden) {
            return; // Bone / ScriptComponent 等の内部型は公開しない
        } else {
            if (T* component = go.GetComponent<T>()) {
                JsonValue entry = JsonValue::MakeObject();
                entry.Set("type", JsonValue(Reg::serializedName));
                if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) {
                    JsonReadReflector reader;
                    component->Reflect(reader);
                    entry.Set("fields", reader.Result());
                } else {
                    entry.Set("fields", JsonValue::MakeObject());
                }
                array.Push(std::move(entry));
            }
        }
    });
    return array;
}

const char* ComponentCategoryName(scene::ComponentCategory category)
{
    switch (category) {
    case scene::ComponentCategory::Rendering:   return "Rendering";
    case scene::ComponentCategory::Lighting:    return "Lighting";
    case scene::ComponentCategory::Physics:     return "Physics";
    case scene::ComponentCategory::Animation:   return "Animation";
    case scene::ComponentCategory::Audio:       return "Audio";
    case scene::ComponentCategory::Effects:     return "Effects";
    case scene::ComponentCategory::Environment: return "Environment";
    case scene::ComponentCategory::Navigation:  return "Navigation";
    case scene::ComponentCategory::Terrain:     return "Terrain";
    case scene::ComponentCategory::UI:          return "UI";
    case scene::ComponentCategory::Misc:        return "Misc";
    case scene::ComponentCategory::Internal:    return "Internal";
    }
    return "Misc";
}

const char* ComponentInspectorName(scene::ComponentInspectorMode mode)
{
    switch (mode) {
    case scene::ComponentInspectorMode::Automatic: return "automatic";
    case scene::ComponentInspectorMode::Custom:    return "custom";
    case scene::ComponentInspectorMode::Hidden:    return "hidden";
    }
    return "hidden";
}

// ComponentRegistry と各 Reflect() を走査し、component.add/set の正確な入力契約を返す。
JsonValue BuildEditorCatalog()
{
    JsonValue components = JsonValue::MakeArray();
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("type", JsonValue(Reg::serializedName));
            entry.Set("displayName", JsonValue(Reg::displayName));
            entry.Set("category", JsonValue(ComponentCategoryName(Reg::category)));
            entry.Set("inspector", JsonValue(ComponentInspectorName(Reg::inspectorMode)));
            constexpr bool addable = Reg::addable
                && std::is_default_constructible_v<T>
                && std::is_move_constructible_v<T>;
            entry.Set("addable", JsonValue(addable));

            JsonValue fields = JsonValue::MakeArray();
            if constexpr (Reg::hasReflect && std::is_default_constructible_v<T>) {
                T component{};
                JsonCatalogReflector reflector;
                component.Reflect(reflector);
                fields = reflector.Result();
            }
            entry.Set("fields", std::move(fields));
            components.Push(std::move(entry));
        }
    });

    JsonValue result = JsonValue::MakeObject();
    result.Set("components", std::move(components));
    return result;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

struct Outcome {
    bool        ok = false;
    JsonValue   result;
    std::string code;
    std::string message;
    static Outcome Ok(JsonValue value) { Outcome outcome; outcome.ok = true; outcome.result = std::move(value); return outcome; }
    static Outcome Err(std::string errorCode, std::string errorMessage)
    {
        Outcome outcome;
        outcome.code = std::move(errorCode);
        outcome.message = std::move(errorMessage);
        return outcome;
    }
};

Outcome SearchEditorCatalog(const JsonValue& payload)
{
    const std::string query = LowerAscii(StringField(payload, "query"));
    const std::string category = LowerAscii(StringField(payload, "category"));
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 25, 1, 100);
    if (query.empty() && category.empty()) return Outcome::Err("BAD_ARG", "query / category のいずれかが必要です");

    JsonValue catalog = BuildEditorCatalog();
    const JsonValue* source = catalog.Find("components");
    JsonValue matches = JsonValue::MakeArray();
    int total = 0;
    if (source != nullptr && source->IsArray()) {
        for (const JsonValue& entry : source->AsArray()) {
            const std::string entryCategory = LowerAscii(StringField(entry, "category"));
            const bool categoryMatches = category.empty() || entryCategory == category;
            const bool queryMatches = query.empty()
                || LowerAscii(SerializeJson(entry)).find(query) != std::string::npos;
            if (!categoryMatches || !queryMatches) continue;
            ++total;
            if (static_cast<int>(matches.AsArray().size()) < limit) matches.Push(entry);
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("components", std::move(matches));
    result.Set("count", JsonValue(total));
    result.Set("truncated", JsonValue(total > limit));
    return Outcome::Ok(std::move(result));
}

// 反射値を検索演算子で比較する。型が演算子に合わない場合は一致しない。
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

// 名前・タグ・状態・複数コンポーネント・反射プロパティをAND条件で検索する。
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
    if (!component.empty()) requiredComponents.push_back(component); // 旧 comp 入力との互換性を維持する
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

// SnapshotComponents の結果を go へ復元する (delete の Undo)。反射不可の型は既定値のみ復元。
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

const char* VFXParamTypeName(asset::VFXParamType type)
{
    switch (type) {
    case asset::VFXParamType::Float: return "Float";
    case asset::VFXParamType::Int: return "Int";
    case asset::VFXParamType::Bool: return "Bool";
    case asset::VFXParamType::Color: return "Color";
    case asset::VFXParamType::Vector3: return "Vector3";
    case asset::VFXParamType::AssetRef: return "AssetRef";
    }
    return "Unknown";
}

JsonValue VFXConstantJson(const asset::VFXConstant& value)
{
    if (const auto* item = std::get_if<float>(&value)) return JsonValue(*item);
    if (const auto* item = std::get_if<int>(&value)) return JsonValue(*item);
    if (const auto* item = std::get_if<bool>(&value)) return JsonValue(*item);
    if (const auto* item = std::get_if<std::string>(&value)) return JsonValue(*item);
    JsonValue result = JsonValue::MakeArray();
    if (const auto* item = std::get_if<math::Vector3>(&value)) {
        result.Push(JsonValue(item->x)); result.Push(JsonValue(item->y)); result.Push(JsonValue(item->z));
    } else if (const auto* item = std::get_if<math::Vector4>(&value)) {
        result.Push(JsonValue(item->x)); result.Push(JsonValue(item->y));
        result.Push(JsonValue(item->z)); result.Push(JsonValue(item->w));
    }
    return result;
}

// 値ソースをAIが意味論を保ったまま再編集できる形へ変換する。
JsonValue VFXParamValueJson(const asset::VFXGraphAsset& graph, const asset::VFXParamValue& value,
                            float normalizedTime, std::uint32_t seed, std::string_view name)
{
    JsonValue result = JsonValue::MakeObject();
    if (const auto* constant = std::get_if<asset::VFXConstant>(&value.source)) {
        result.Set("source", JsonValue("Constant"));
        result.Set("value", VFXConstantJson(*constant));
    } else if (const auto* curve = std::get_if<asset::VFXCurveSource>(&value.source)) {
        result.Set("source", JsonValue("Curve"));
        result.Set("value", JsonValue(curve->curve.Evaluate(normalizedTime)));
        JsonValue keys = JsonValue::MakeArray();
        for (std::uint32_t index = 0; index < (std::min)(curve->curve.keyCount, scene::kMaxParticleCurveKeys); ++index) {
            JsonValue key = JsonValue::MakeArray();
            key.Push(JsonValue(curve->curve.keys[index].time));
            key.Push(JsonValue(curve->curve.keys[index].value));
            keys.Push(std::move(key));
        }
        result.Set("keys", std::move(keys));
    } else if (const auto* gradient = std::get_if<asset::VFXGradientSource>(&value.source)) {
        result.Set("source", JsonValue("Gradient"));
        const auto sampled = gradient->gradient.Evaluate(normalizedTime);
        JsonValue color = JsonValue::MakeArray();
        color.Push(JsonValue(sampled.x)); color.Push(JsonValue(sampled.y));
        color.Push(JsonValue(sampled.z)); color.Push(JsonValue(sampled.w));
        result.Set("value", std::move(color));
        JsonValue keys = JsonValue::MakeArray();
        for (std::uint32_t index = 0; index < (std::min)(gradient->gradient.keyCount, scene::kMaxParticleCurveKeys); ++index) {
            JsonValue key = JsonValue::MakeArray();
            key.Push(JsonValue(gradient->gradient.keys[index].time));
            key.Push(JsonValue(gradient->gradient.keys[index].color.x));
            key.Push(JsonValue(gradient->gradient.keys[index].color.y));
            key.Push(JsonValue(gradient->gradient.keys[index].color.z));
            key.Push(JsonValue(gradient->gradient.keys[index].color.w));
            keys.Push(std::move(key));
        }
        result.Set("keys", std::move(keys));
    } else if (const auto* random = std::get_if<asset::VFXRandomRange>(&value.source)) {
        result.Set("source", JsonValue("RandomRange"));
        result.Set("minimum", JsonValue(random->minimum));
        result.Set("maximum", JsonValue(random->maximum));
        const float alpha = asset::DeterministicVFXRandom(seed, name);
        result.Set("value", JsonValue(random->minimum + (random->maximum - random->minimum) * alpha));
    } else if (const auto* attribute = std::get_if<asset::VFXAttributeRef>(&value.source)) {
        result.Set("source", JsonValue("AttributeRef"));
        result.Set("path", JsonValue(attribute->path));
    } else if (const auto* signal = std::get_if<asset::VFXSignalRef>(&value.source)) {
        result.Set("source", JsonValue("Signal"));
        result.Set("name", JsonValue(signal->signalName));
        const auto output = std::find_if(graph.signalOutputs.begin(), graph.signalOutputs.end(),
            [&](const auto& item) { return item.name == signal->signalName; });
        if (output != graph.signalOutputs.end()) {
            std::unordered_map<int, float> cache;
            result.Set("value", JsonValue(asset::EvaluateVFXSignalNode(graph, output->nodeId, normalizedTime, cache)));
        }
    }
    return result;
}

const char* PropertyTypeName(reflection::PropertyType type)
{
    switch (type) {
    case reflection::PropertyType::Float: return "Float";
    case reflection::PropertyType::Int: return "Int";
    case reflection::PropertyType::Bool: return "Bool";
    case reflection::PropertyType::Vector2: return "Vector2";
    case reflection::PropertyType::Vector3: return "Vector3";
    case reflection::PropertyType::Color: return "Color";
    case reflection::PropertyType::Quaternion: return "Quaternion";
    case reflection::PropertyType::String: return "String";
    case reflection::PropertyType::AssetRef: return "AssetRef";
    case reflection::PropertyType::Curve: return "Curve";
    case reflection::PropertyType::Gradient: return "Gradient";
    case reflection::PropertyType::Enum: return "Enum";
    case reflection::PropertyType::Struct: return "Struct";
    case reflection::PropertyType::Array: return "Array";
    }
    return "Unknown";
}

// スキーマ内の葉プロパティのパスを全て集める。
// 配列は owner の実要素数ぶん "bursts[0].count" のように展開されるため、
// 走査には対象インスタンスが要る (要素数はインスタンスにしか無い情報のため)。
// 走査の実体は reflection::CollectLeafPaths ひとつに集約し、Inspector・テストと同じ結果を返す。
void CollectVFXSchemaPaths(const reflection::ITypeSchema& schema, const void* owner,
                           const std::string& prefix, std::vector<std::string>& output)
{
    reflection::CollectLeafPaths(schema, owner, prefix, output);
}

// プロパティ値を比較・提示用のテキストへ落とす。
// WHY: 差分は「変わったかどうか」と「何から何へ」が伝わればよく、型ごとの JSON 表現を
//      作り分けるほどの情報量は要らない。テキスト1本にすると比較も出力も1経路で済む。
std::string VFXSchemaValueToText(reflection::PropertyType type, const std::any& value)
{
    const auto number = [](double v) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.4g", v);
        return std::string(buffer);
    };
    if (const auto* v = std::any_cast<float>(&value)) return number(*v);
    if (const auto* v = std::any_cast<int>(&value)) return std::to_string(*v);
    if (const auto* v = std::any_cast<bool>(&value)) return *v ? "true" : "false";
    if (const auto* v = std::any_cast<std::string>(&value)) return *v;
    if (const auto* v = std::any_cast<math::Vector3>(&value))
        return "[" + number(v->x) + ", " + number(v->y) + ", " + number(v->z) + "]";
    if (const auto* v = std::any_cast<math::Vector4>(&value))
        return "[" + number(v->x) + ", " + number(v->y) + ", " + number(v->z) + ", " + number(v->w) + "]";
    // Curve / Gradient など構造値は個別比較しない (差分としては「変更あり」で十分)。
    return std::string("<") + PropertyTypeName(type) + ">";
}

// スキーマ値を JSON へ戻す。JsonToSchemaValue の対になる読み出しで、
// ここが返した value をそのまま vfx.node.setField の value へ渡せる (=往復できる) ことが要件。
//
// WHY: これまで書き込み側 (setField) しか存在せず、「今このノードの blendMode が何か」
//      「texturePath に何が入っているか」を API 越しに確かめる手段が無かった。
//      その結果 AI は現在値を知らないまま上書きし、変更が効いたのかどうかも
//      プレビュー画像からしか判断できなかった。読み出しを対で用意して推測を消す。
//
// Curve / Gradient は JsonToParticleCurve / JsonToParticleGradient が受け付ける
// { interp, keys } 形式で返す。キー列を配列で書き戻せば同じ形が再現する。
JsonValue SchemaValueToJson(reflection::PropertyType type, const std::any& value)
{
    if (const auto* v = std::any_cast<bool>(&value)) return JsonValue(*v);
    if (const auto* v = std::any_cast<int>(&value)) return JsonValue(*v);
    if (const auto* v = std::any_cast<float>(&value)) return JsonValue(static_cast<double>(*v));
    if (const auto* v = std::any_cast<std::string>(&value)) return JsonValue(*v);
    if (const auto* v = std::any_cast<math::Vector3>(&value)) {
        JsonValue result = JsonValue::MakeArray();
        result.Push(JsonValue(v->x)); result.Push(JsonValue(v->y)); result.Push(JsonValue(v->z));
        return result;
    }
    if (const auto* v = std::any_cast<math::Vector4>(&value)) {
        JsonValue result = JsonValue::MakeArray();
        result.Push(JsonValue(v->x)); result.Push(JsonValue(v->y));
        result.Push(JsonValue(v->z)); result.Push(JsonValue(v->w));
        return result;
    }
    if (const auto* v = std::any_cast<scene::ParticleCurve>(&value)) {
        JsonValue keys = JsonValue::MakeArray();
        for (std::uint32_t index = 0; index < (std::min)(v->keyCount, scene::kMaxParticleCurveKeys); ++index) {
            JsonValue key = JsonValue::MakeArray();
            key.Push(JsonValue(v->keys[index].time));
            key.Push(JsonValue(v->keys[index].value));
            keys.Push(std::move(key));
        }
        JsonValue result = JsonValue::MakeObject();
        result.Set("interp", JsonValue(static_cast<int>(v->interpolation)));
        result.Set("keys", std::move(keys));
        return result;
    }
    if (const auto* v = std::any_cast<scene::ParticleGradient>(&value)) {
        JsonValue keys = JsonValue::MakeArray();
        for (std::uint32_t index = 0; index < (std::min)(v->keyCount, scene::kMaxParticleCurveKeys); ++index) {
            JsonValue key = JsonValue::MakeArray();
            key.Push(JsonValue(v->keys[index].time));
            key.Push(JsonValue(v->keys[index].color.x));
            key.Push(JsonValue(v->keys[index].color.y));
            key.Push(JsonValue(v->keys[index].color.z));
            key.Push(JsonValue(v->keys[index].color.w));
            keys.Push(std::move(key));
        }
        JsonValue result = JsonValue::MakeObject();
        result.Set("interp", JsonValue(static_cast<int>(v->interpolation)));
        result.Set("keys", std::move(keys));
        return result;
    }
    // ここへ来るのは PropertyType を足したのに変換を書き忘れた場合だけ。
    // null を返すと「値が無い」と誤読されるため、型名を文字列で返して欠落だと分かるようにする。
    return JsonValue(std::string("<unsupported:") + PropertyTypeName(type) + ">");
}

void AppendSchemaProperties(JsonValue& output, const reflection::ITypeSchema& schema,
                            const std::string& prefix)
{
    for (const auto& property : schema.Properties()) {
        const std::string path = prefix.empty() ? std::string(property.key)
                                                : prefix + "." + std::string(property.key);
        if (property.type == reflection::PropertyType::Struct && property.childSchema != nullptr) {
            AppendSchemaProperties(output, *property.childSchema, path);
            continue;
        }
        JsonValue item = JsonValue::MakeObject();
        item.Set("path", JsonValue(path));
        item.Set("type", JsonValue(PropertyTypeName(property.type)));
        item.Set("display", JsonValue(std::string(property.display)));
        item.Set("category", JsonValue(std::string(property.category)));
        item.Set("exposable", JsonValue(property.exposable));
        if (property.range.enabled) {
            item.Set("minimum", JsonValue(property.range.minimum));
            item.Set("maximum", JsonValue(property.range.maximum));
        }
        // enum は数値の意味が名前を見ないと分からない。選択肢を並べて渡す。
        if (!property.enumNames.empty()) {
            JsonValue names = JsonValue::MakeArray();
            for (const std::string_view name : property.enumNames)
                names.Push(JsonValue(std::string(name)));
            item.Set("enumNames", std::move(names));
        }
        // 配列は path 自体を setField の対象にできない。添字を付けて要素へ降りることと、
        // 要素型のフィールド一覧を AI が引けるよう明示する。
        if (property.type == reflection::PropertyType::Array) {
            item.Set("indexed", JsonValue(true));
            item.Set("elementPathExample", JsonValue(path + "[0]"));
            if (property.childSchema != nullptr) {
                JsonValue elementFields = JsonValue::MakeArray();
                AppendSchemaProperties(elementFields, *property.childSchema, path + "[0]");
                item.Set("elementFields", std::move(elementFields));
            }
        }
        output.Push(std::move(item));
    }
}

// vfx.lint — .vfx を静的診断し、問題を構造化テキストで返す。
// WHY: AI がエフェクトを組むとき、スクリーンショットを見て気付くより
//      「Entry から届かないノードがある」「テクスチャが無い」を文章で受け取る方が
//      桁違いに速く確実に直せる。Validate が通っても実際には何も出ない、という
//      typo 起因の空振りをここで潰す。
// lint の各 code に対する「どう直すか」。issue へ添えて返す。
//
// WHY: これまで lint は「何が壊れているか」だけを返し、直し方は AI の推測に任せていた。
//      結果として、同じ警告に対して呼ぶコマンドが毎回変わり、直したつもりで別の規約を
//      踏み直す往復が発生する。code ごとに正解の操作を 1 つ書いておけば、
//      修正は推論ではなく参照になる。文言ではなく code で引けることが重要で、
//      これは Editor の警告 banner と AI が同じ code 集合を共有しているから成立する。
//
// autoFixable: vfx.repair が判断なしで直せるもの。false は設計判断が要るため AI に残す。
struct VFXFixHint {
    const char* code;
    bool autoFixable;
    const char* action;  // 呼ぶべきコマンドと引数
    const char* caution; // 直す前に確認すべきこと (空なら無し)
};

const VFXFixHint* FindVFXFixHint(std::string_view code)
{
    static constexpr VFXFixHint kHints[] = {
        { "UNREACHABLE_NODE", true,
          "vfx_repair(connectOrphans=true) で Entry から OnStart で接続する。"
          "意図した発火順があるなら vfx_link_add で適切な source から繋ぐ。",
          "Entry 直結は「グラフ開始と同時に出る」意味になる。遅らせたいなら startOffset も設定する。" },
        { "MISSING_ASSET", true,
          "vfx_repair(fixAssets=true) でファイル名の近いアセットへ張り替える。"
          "見つからない場合は asset_list で実在パスを調べ vfx_node_set_field で設定する。",
          "自動置換はファイル名の類似だけで選ぶため、置換後に vfx_inspect_graph でパスを確認すること。" },
        { "EMPTY_SUBGRAPH", false,
          "vfx_node_set_field(schemaPath=\"subGraph.graphPath\") で .vfx を指定するか、"
          "vfx_node_remove でノードごと削除する。", "" },
        { "SHEARED_SPRITE", true,
          "vfx_repair(fixSprites=true) で sizeAxisScale を等方 [1,1,1] へ戻す。"
          "縦長にしたい場合は代わりに回転 (angularVelocity / rotationCurve) を 0 にする。",
          "どちらを捨てるかは見た目の意図次第。炎の舌なら非等方、破片なら回転を残す。" },
        { "LIGHTING_SATURATED", true,
          "vfx_repair(fixLighting=true) で particle.lightingStrength を 0.8 へ落とす。", "" },
        { "ALPHA_NO_SORT", true,
          "vfx_repair(fixSorting=true) で particle.sortMode を 1 (BackToFront) にする。", "" },
        { "MESH_NO_FADE", true,
          "vfx_repair(fixMeshFade=true) で mesh.colorEnd の RGB を 0 にする。",
          "加算ブレンドでは RGB が 0 になって初めて消える。alpha だけ 0 にしても残る。" },
        { "BOUND_FIELD_OVERRIDDEN", false,
          "ノード側ではなく vfx_param_set_default で公開パラメーターの既定値を変更する。"
          "そのノードだけ別の値にしたいなら vfx_param_bind を解いてから設定する。",
          "bind 済み leaf への書き込みは保存されるが実行時に必ず上書きされる。" },
        { "OVER_BUDGET_PARTICLES", true,
          "vfx_optimize_budget(targetParticles=...) で各ノードの maxParticles を按分して下げる。",
          "見た目の密度が落ちる。粒を大きくして枚数を減らす方が破綻しにくい。" },
        { "GPU_FALLBACK", false,
          "message が名指しした設定を変えて GPU 条件を満たすか、"
          "その設定を残すなら particle.simulationMode を 0 (Cpu) へ戻して意図を明示する。"
          "どちらを採るかは表現の要求次第で、機械的には決められない。",
          "「GPU で大量」と「per-particle Trail・SubEmitter・Local 空間」は両立しない"
          "(ソートとメッシュパーティクルは GPU 側で対応済みなので縮退しない)。"
          "粒子数を増やしても、縮退したままでは GPU 側の性能は一切使われない。" },
        { "PARENT_HAS_NO_TRANSFORM", true,
          "vfx_repair(fixParents=true) で親指定を外す。位置を継承したいなら"
          "vfx_node_set_parent で実体を持つノード (Particle / Mesh / Light など) を親にする。",
          "Entry / Delay / Reroute は実体を持たず、空間上の位置も持たない。" },
        { "PARENT_OVERRIDDEN_BY_SOCKET", false,
          "attachBone と parentNodeId のどちらを使うか決める。ボーン追従が要るなら"
          "vfx_node_set_parent(parentNodeId=-1) で親を外し、要らないなら"
          "vfx_node_set_field(schemaPath=\"attachBone\", value=\"\") で socket を外す。",
          "実行時は socket が優先されるため、今は parentNodeId 側が効いていない。" },
        { "NO_OUTPUT", false,
          "vfx_node_add で Particle / Mesh / Light などの実体ノードを追加する。"
          "骨格から作るなら vfx_template_apply が早い。", "" },
        { "UNKNOWN_SHADER_PARAM", false,
          "shader_inspect でそのマテリアルのシェーダー変数一覧を取り、実在する名前へ直す。"
          "動かしたい効果に対応する変数が無ければ、別のシェーダーを持つ .mat へ差し替える。",
          "未使用変数はコンパイル時に消えるため、HLSL に宣言があっても実行時には存在しないことがある。"
          "shader_inspect が返すのはコンパイル済みバイトコードの実体。" },
    };
    for (const auto& hint : kHints)
        if (code == hint.code) return &hint;
    return nullptr;
}

Outcome DoVFXLint(editor::EditorContext& ctx, const JsonValue& payload)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    asset::VFXGraphAsset graph;
    std::string parseError;
    if (!asset::ParseVFXGraphAsset(path, graph, &parseError))
        return Outcome::Err("VFX_INVALID", parseError);

    JsonValue issues = JsonValue::MakeArray();
    int errorCount = 0;
    int warningCount = 0;
    const auto addIssue = [&](const char* severity, const char* code,
                              std::string message, int nodeId) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("severity", JsonValue(std::string(severity)));
        item.Set("code", JsonValue(std::string(code)));
        item.Set("message", JsonValue(std::move(message)));
        if (nodeId > 0) item.Set("nodeId", JsonValue(nodeId));
        // 直し方を code から引いて添える。AI に毎回推論させないための機械可読な手順。
        if (const VFXFixHint* hint = FindVFXFixHint(code); hint != nullptr) {
            item.Set("autoFixable", JsonValue(hint->autoFixable));
            item.Set("fix", JsonValue(std::string(hint->action)));
            if (hint->caution[0] != '\0') item.Set("caution", JsonValue(std::string(hint->caution)));
        } else {
            // 手順を用意していない code は「自動修復できない」と明示する。
            // 黙って欠落させると、AI は fix が無いことを「直さなくてよい」と読みかねない。
            item.Set("autoFixable", JsonValue(false));
        }
        issues.Push(std::move(item));
        if (std::string_view(severity) == "error") ++errorCount;
        else ++warningCount;
    };

    // 1. スキーマ検証 (ID重複・Entry数・負の時間など)
    std::string validationError;
    if (!asset::ValidateVFXGraphAsset(graph, &validationError))
        addIssue("error", "INVALID_GRAPH", validationError, 0);

    // 2. スケジュール構築 = 循環検出
    std::vector<float> startTimes;
    float duration = 0.0f;
    std::string scheduleError;
    const bool scheduled = asset::BuildVFXGraphSchedule(graph, startTimes, duration, &scheduleError);
    if (!scheduled) addIssue("error", "SCHEDULE_FAILED", scheduleError, 0);

    // 3. Entry からの到達性。検証は通るが実行時に一度も起動しない配線ミス。
    const auto entry = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [](const asset::VFXGraphNode& node) { return node.type == asset::VFXNodeType::Entry; });
    if (entry != graph.nodes.end()) {
        std::vector<int> reachable{ entry->id };
        for (std::size_t head = 0; head < reachable.size(); ++head) {
            for (const auto& link : graph.links) {
                if (link.fromNode != reachable[head]) continue;
                if (std::find(reachable.begin(), reachable.end(), link.toNode) == reachable.end())
                    reachable.push_back(link.toNode);
            }
        }
        for (const auto& node : graph.nodes) {
            if (std::find(reachable.begin(), reachable.end(), node.id) != reachable.end()) continue;
            addIssue("error", "UNREACHABLE_NODE",
                     "Entry から到達できないため実行時に起動しません: " + node.name, node.id);
        }
    }

    // 4. 参照アセットの実在確認。空振りの最頻原因。
    namespace fs = std::filesystem;
    const auto checkAsset = [&](const std::string& assetPath, const char* label, int nodeId) {
        if (assetPath.empty()) return;
        if (assetPath.rfind("primitive:", 0) == 0) return;
        std::error_code ec;
        const bool inProject = !ctx.projectRoot.empty()
            && fs::is_regular_file(fs::path(ctx.projectRoot) / assetPath, ec);
        ec.clear();
        const bool inEngine = !ctx.engineRoot.empty()
            && fs::is_regular_file(fs::path(ctx.engineRoot) / assetPath, ec);
        ec.clear();
        if (inProject || inEngine || fs::is_regular_file(assetPath, ec)) return;
        addIssue("error", "MISSING_ASSET",
                 std::string(label) + " が見つかりません: " + assetPath, nodeId);
    };
    for (const auto& node : graph.nodes) {
        switch (node.type) {
        case asset::VFXNodeType::Particle: {
            checkAsset(node.particle.texturePath, "texture", node.id);
            checkAsset(node.particle.materialPath, "material", node.id);
            checkAsset(node.particle.meshShapePath, "meshShape", node.id);
            // GPU シミュレーションの無言の縮退。simulationMode = Gpu にしても、
            // 条件のどれか 1 つを外すと黙って CPU へ落ちる。
            // WHY: 10 万粒子を狙って Gpu を指定したのに per-particle Trail を付けたせいで
            //      CPU で回っていた、という事故が起きるが、それが今までどこにも出ていなかった。
            //      静的解析で判る条件ばかりなので、実行する前にここで名指しする。
            if (const auto reason = scene::GetParticleGpuFallbackReason(node.particle);
                reason != scene::ParticleGpuFallbackReason::None
                && reason != scene::ParticleGpuFallbackReason::NotRequested) {
                addIssue("warning", "GPU_FALLBACK",
                         std::string("simulationMode = Gpu ですが ")
                             + scene::ParticleGpuFallbackFieldName(reason)
                             + " のため CPU で実行されます。"
                             + scene::ParticleGpuFallbackDescription(reason),
                         node.id);
            }
            break;
        }
        case asset::VFXNodeType::Trail:
        case asset::VFXNodeType::MeshTrail:
            checkAsset(node.trail.texturePath, "texture", node.id);
            checkAsset(node.trail.materialPath, "material", node.id);
            checkAsset(node.trail.meshPath, "mesh", node.id);
            break;
        case asset::VFXNodeType::Audio:
            checkAsset(node.audio.clipPath, "clip", node.id);
            break;
        case asset::VFXNodeType::Decal:
            checkAsset(node.decal.albedoPath, "albedo", node.id);
            if (node.decal.albedoPath.empty())
                addIssue("warning", "EMPTY_DECAL", "Decal に albedo がありません", node.id);
            break;
        case asset::VFXNodeType::Mesh:
            checkAsset(node.mesh.meshPath, "mesh", node.id);
            checkAsset(node.mesh.materialPath, "material", node.id);
            // animatedParam は「マテリアルのシェーダーに実在する変数名」でなければならない。
            // WHY: 存在しない名前を書いても保存は通り、実行時は黙って無視される。
            //      Dissolve の alphaCutoff を動かすつもりが綴り違いで何も起きない、という
            //      失敗はプレビュー画像から原因を特定できない (「変化しない」としか見えない)。
            if (!node.mesh.animatedParam.empty() && ctx.resources != nullptr) {
                const std::string materialPath = node.mesh.materialPath.empty()
                    ? std::string(asset::VFX_MESH_FALLBACK_MATERIAL) : node.mesh.materialPath;
                asset::MaterialAsset material;
                if (asset::LoadMaterialAssetFromFile(
                        asset::AssetManager::ResolveAssetPath(materialPath), material)
                    && !material.shaderPath.empty()) {
                    const std::string resolvedShader =
                        asset::AssetManager::ResolveAssetPath(material.shaderPath);
                    const auto handle = ctx.resources->LoadShader(
                        resolvedShader.empty() ? material.shaderPath : resolvedShader);
                    const renderer::IShader* shader =
                        handle.IsValid() ? ctx.resources->Get(handle) : nullptr;
                    if (shader != nullptr && shader->GetDescriptor().IsValid()
                        && shader->GetDescriptor().FindVar(node.mesh.animatedParam) == nullptr)
                        addIssue("warning", "UNKNOWN_SHADER_PARAM",
                                 "animatedParam \"" + node.mesh.animatedParam
                                     + "\" が " + materialPath
                                     + " のシェーダーに存在しません (実行時に無視されます)",
                                 node.id);
                }
            }
            // colorEnd の RGB が残っていると加算ブレンドで消えずに残り続ける。
            if ((std::max)({ node.mesh.colorEnd.x, node.mesh.colorEnd.y, node.mesh.colorEnd.z })
                > 0.02f)
                addIssue("warning", "MESH_NO_FADE",
                         "colorEnd の RGB が 0 でないため消えずに残ります", node.id);
            break;
        case asset::VFXNodeType::SubGraph:
            if (node.subGraph.graphPath.empty())
                addIssue("error", "EMPTY_SUBGRAPH", "Sub Graph に .vfx 参照がありません", node.id);
            else checkAsset(node.subGraph.graphPath, "graph", node.id);
            break;
        default: break;
        }
    }

    // 5. budget 超過
    const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(graph);
    if (budget.particles > graph.maxParticles)
        addIssue("warning", "OVER_BUDGET_PARTICLES",
                 "particle budget 超過: " + std::to_string(budget.particles) + " / "
                     + std::to_string(graph.maxParticles), 0);
    if (budget.lights > graph.maxLights)
        addIssue("warning", "OVER_BUDGET_LIGHTS",
                 "light budget 超過: " + std::to_string(budget.lights) + " / "
                     + std::to_string(graph.maxLights), 0);
    if (budget.audioVoices > graph.maxAudioVoices)
        addIssue("warning", "OVER_BUDGET_AUDIO",
                 "audio budget 超過: " + std::to_string(budget.audioVoices) + " / "
                     + std::to_string(graph.maxAudioVoices), 0);

    // 6. 公開パラメーターとバインドの整合。型不一致は保存できてしまうので明示する。
    for (const auto& binding : graph.bindings) {
        const auto* definition = asset::FindVFXParameter(graph, binding.paramName);
        if (definition == nullptr) {
            addIssue("error", "UNKNOWN_PARAM",
                     "binding が未定義のパラメーターを参照しています: " + binding.paramName,
                     binding.nodeId);
            continue;
        }
        const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&binding](const asset::VFXGraphNode& item) { return item.id == binding.nodeId; });
        if (node == graph.nodes.end()) {
            addIssue("error", "BINDING_NODE_MISSING",
                     "binding の nodeId が存在しません: " + binding.paramName, binding.nodeId);
            continue;
        }
        reflection::ResolvedProperty resolved;
        if (!reflection::ResolveProperty(asset::GetVFXNodeSchema(), &*node, binding.schemaPath, resolved)
            || resolved.property == nullptr) {
            addIssue("error", "BINDING_PATH_INVALID",
                     "schemaPath を解決できません: " + binding.schemaPath, binding.nodeId);
        }
    }

    // 7. 「検証は通るが見た目が壊れる」設定。
    // WHY: これらは AI が最も踏みやすく、かつスクリーンショットからは原因を特定できない類
    //      (回転×非等方でスプライトがせん断される / bind 済みフィールドの編集が実行時に死ぬ)。
    //      Editor の警告 banner と同じ CollectVFXGraphWarnings をそのまま使うことで、
    //      「人が見る面」と「AI が読む面」が一致し続ける (別実装にすると必ずドリフトする)。
    //      参照切れだけは上の checkAsset がプロジェクト/エンジン両ルートを見る分だけ賢いので、
    //      そちらへ任せて二重報告を避ける。
    for (const auto& warning : asset::CollectVFXGraphWarnings(graph, /*checkAssetReferences=*/false))
        addIssue("warning", warning.code.c_str(), warning.message, warning.nodeId);

    // 8. 何も出ないグラフ (Entry と Delay しかない)
    const bool hasVisual = std::any_of(graph.nodes.begin(), graph.nodes.end(),
        [](const asset::VFXGraphNode& node) {
            return node.type != asset::VFXNodeType::Entry
                && node.type != asset::VFXNodeType::Delay;
        });
    if (!hasVisual)
        addIssue("warning", "NO_OUTPUT", "実体を持つノードが1つもありません", 0);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("name", JsonValue(graph.name));
    result.Set("duration", JsonValue(scheduled ? duration : 0.0));
    result.Set("nodeCount", JsonValue(static_cast<int>(graph.nodes.size())));
    result.Set("linkCount", JsonValue(static_cast<int>(graph.links.size())));
    result.Set("errors", JsonValue(errorCount));
    result.Set("warnings", JsonValue(warningCount));
    result.Set("issues", std::move(issues));
    return Outcome::Ok(std::move(result));
}

// vfx.diff — 2つの .vfx の差分を構造化して返す。
// WHY: AI が編集の前後を比べるとき、ファイル全文を2回読ませると
//      文脈を食い潰すうえ「どこが変わったか」の判断自体を毎回やり直すことになる。
//      ノード/リンク/パラメーターの単位で差分だけを返す。
Outcome DoVFXDiff(const JsonValue& payload)
{
    const std::string basePath = StringField(payload, "base");
    const std::string targetPath = StringField(payload, "target");
    if (basePath.empty() || targetPath.empty())
        return Outcome::Err("BAD_ARG", "base と target が必要です");
    asset::VFXGraphAsset base;
    asset::VFXGraphAsset target;
    std::string error;
    if (!asset::ParseVFXGraphAsset(basePath, base, &error))
        return Outcome::Err("VFX_INVALID", "base: " + error);
    if (!asset::ParseVFXGraphAsset(targetPath, target, &error))
        return Outcome::Err("VFX_INVALID", "target: " + error);

    const auto findNode = [](const asset::VFXGraphAsset& graph, int id) -> const asset::VFXGraphNode* {
        const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [id](const asset::VFXGraphNode& node) { return node.id == id; });
        return it == graph.nodes.end() ? nullptr : &*it;
    };

    JsonValue addedNodes = JsonValue::MakeArray();
    JsonValue removedNodes = JsonValue::MakeArray();
    JsonValue changedNodes = JsonValue::MakeArray();
    for (const auto& node : target.nodes) {
        const auto* previous = findNode(base, node.id);
        if (previous == nullptr) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("nodeId", JsonValue(node.id));
            item.Set("name", JsonValue(node.name));
            item.Set("nodeType", JsonValue(std::string(asset::VFXNodeTypeName(node.type))));
            addedNodes.Push(std::move(item));
            continue;
        }
        // 値の差分はスキーマを総なめし、変わったプロパティのパスと新旧値を並べる。
        JsonValue fields = JsonValue::MakeArray();
        std::vector<std::string> paths;
        // 走査対象は編集後 (target) のノード。配列要素が増えた分も差分に出したいため。
        CollectVFXSchemaPaths(asset::GetVFXNodeSchema(), &node, {}, paths);
        for (const std::string& schemaPath : paths) {
            reflection::ResolvedProperty before;
            reflection::ResolvedProperty after;
            if (!reflection::ResolveProperty(asset::GetVFXNodeSchema(), &node, schemaPath, after)
                || after.property == nullptr) continue;
            // base 側で解決できない = 配列要素が増えた (bursts[2] が新設された等)。
            // ここで continue すると「Burst を足した」変更が差分から丸ごと消えるため、
            // 追加として明示する。
            const bool existedBefore =
                reflection::ResolveProperty(asset::GetVFXNodeSchema(), previous, schemaPath, before)
                && before.property != nullptr;
            const std::string beforeText = existedBefore
                ? VFXSchemaValueToText(before.property->type, before.property->get(before.constOwner))
                : std::string("<absent>");
            const std::string afterText = VFXSchemaValueToText(after.property->type,
                                                               after.property->get(after.constOwner));
            if (beforeText == afterText) continue;
            JsonValue field = JsonValue::MakeObject();
            field.Set("schemaPath", JsonValue(schemaPath));
            field.Set("before", JsonValue(beforeText));
            field.Set("after", JsonValue(afterText));
            fields.Push(std::move(field));
        }
        if (previous->name != node.name) {
            JsonValue field = JsonValue::MakeObject();
            field.Set("schemaPath", JsonValue(std::string("name")));
            field.Set("before", JsonValue(previous->name));
            field.Set("after", JsonValue(node.name));
            fields.Push(std::move(field));
        }
        if (fields.AsArray().empty()) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("name", JsonValue(node.name));
        item.Set("fields", std::move(fields));
        changedNodes.Push(std::move(item));
    }
    for (const auto& node : base.nodes) {
        if (findNode(target, node.id) != nullptr) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("name", JsonValue(node.name));
        item.Set("nodeType", JsonValue(std::string(asset::VFXNodeTypeName(node.type))));
        removedNodes.Push(std::move(item));
    }

    const auto linkKey = [](const asset::VFXGraphLink& link) {
        return std::to_string(link.fromNode) + "->" + std::to_string(link.toNode) + ":"
            + std::to_string(static_cast<int>(link.trigger));
    };
    JsonValue addedLinks = JsonValue::MakeArray();
    JsonValue removedLinks = JsonValue::MakeArray();
    std::vector<std::string> baseKeys;
    for (const auto& link : base.links) baseKeys.push_back(linkKey(link));
    std::vector<std::string> targetKeys;
    for (const auto& link : target.links) targetKeys.push_back(linkKey(link));
    for (std::size_t index = 0; index < target.links.size(); ++index) {
        if (std::find(baseKeys.begin(), baseKeys.end(), targetKeys[index]) != baseKeys.end()) continue;
        addedLinks.Push(JsonValue(targetKeys[index]));
    }
    for (std::size_t index = 0; index < base.links.size(); ++index) {
        if (std::find(targetKeys.begin(), targetKeys.end(), baseKeys[index]) != targetKeys.end()) continue;
        removedLinks.Push(JsonValue(baseKeys[index]));
    }

    JsonValue parameterChanges = JsonValue::MakeArray();
    for (const auto& parameter : target.parameters) {
        if (asset::FindVFXParameter(base, parameter.name) == nullptr)
            parameterChanges.Push(JsonValue("added: " + parameter.name));
    }
    for (const auto& parameter : base.parameters) {
        if (asset::FindVFXParameter(target, parameter.name) == nullptr)
            parameterChanges.Push(JsonValue("removed: " + parameter.name));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("base", JsonValue(basePath));
    result.Set("target", JsonValue(targetPath));
    const bool identical = addedNodes.AsArray().empty() && removedNodes.AsArray().empty()
        && changedNodes.AsArray().empty() && addedLinks.AsArray().empty()
        && removedLinks.AsArray().empty() && parameterChanges.AsArray().empty();
    result.Set("identical", JsonValue(identical));
    result.Set("addedNodes", std::move(addedNodes));
    result.Set("removedNodes", std::move(removedNodes));
    result.Set("changedNodes", std::move(changedNodes));
    result.Set("addedLinks", std::move(addedLinks));
    result.Set("removedLinks", std::move(removedLinks));
    result.Set("parameters", std::move(parameterChanges));
    return Outcome::Ok(std::move(result));
}

// VFX ノードの Transform を返すための最小ヘルパー。
// NOTE: Scene 側の VectorToJson は GameObject 用に別の場所で定義されているため、
//       依存を持ち込まずここで完結させる。
JsonValue VFXVector3ToJson(const math::Vector3& value)
{
    JsonValue result = JsonValue::MakeArray();
    result.Push(JsonValue(value.x));
    result.Push(JsonValue(value.y));
    result.Push(JsonValue(value.z));
    return result;
}

// detail="summary" (既定) は「構造を把握する」ための最小集合だけを返し、
// detail="full" は編集に必要な全フィールド (Transform / エディタ座標 / グループ / シグナル) を返す。
//
// WHY: 以前は常に全部を返していたため、5 個のテンプレートを一巡見るだけで
//      editorPosition と transform が数百行を占め、lint が「問題なし」と言っている
//      グラフでも読むだけで context を大きく食っていた。
//      構造の把握と座標の編集は別の作業なので、要求されたときだけ後者を返す。
Outcome DoVFXGraphInspect(const JsonValue& payload)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    const std::string detail = LowerAscii(StringField(payload, "detail"));
    if (!detail.empty() && detail != "summary" && detail != "full")
        return Outcome::Err("BAD_ARG", "detail は summary / full のいずれかです");
    const bool full = detail == "full";
    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::ParseVFXGraphAsset(path, graph, &error))
        return Outcome::Err("VFX_INVALID", error);
    const bool valid = asset::ValidateVFXGraphAsset(graph, &error);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("detail", JsonValue(full ? "full" : "summary"));
    result.Set("name", JsonValue(graph.name));
    result.Set("version", JsonValue(graph.version));
    JsonValue budgetLimits = JsonValue::MakeObject();
    budgetLimits.Set("particles", JsonValue(graph.maxParticles));
    budgetLimits.Set("lights", JsonValue(graph.maxLights));
    budgetLimits.Set("audioVoices", JsonValue(graph.maxAudioVoices));
    result.Set("budgetLimits", std::move(budgetLimits));
    result.Set("valid", JsonValue(valid));
    result.Set("validationError", JsonValue(valid ? std::string{} : error));
    JsonValue nodes = JsonValue::MakeArray();
    for (const auto& node : graph.nodes) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(node.id));
        item.Set("type", JsonValue(asset::VFXNodeTypeName(node.type)));
        item.Set("name", JsonValue(node.name));
        item.Set("enabled", JsonValue(node.enabled));
        item.Set("startOffset", JsonValue(node.startOffset));
        item.Set("duration", JsonValue(node.duration));
        if (full) {
            JsonValue editorPosition = JsonValue::MakeArray();
            editorPosition.Push(JsonValue(node.editorX));
            editorPosition.Push(JsonValue(node.editorY));
            item.Set("editorPosition", std::move(editorPosition));
            // 空間の配置。link (実行の因果) とは独立した情報で、これを返さないと AI は
            // 「位置がどう決まっているか」を一切知らないままフィールドを書き換えることになる。
            JsonValue transform = JsonValue::MakeObject();
            transform.Set("position", VFXVector3ToJson(node.localPosition));
            transform.Set("rotationDegrees", VFXVector3ToJson(node.localRotationDegrees));
            transform.Set("scale", VFXVector3ToJson(node.localScale));
            item.Set("transform", std::move(transform));
        }
        // -1 = owner 直下。それ以外は親ノードの id で、その Transform が合成される。
        // 親を持つノードの localPosition は親からの相対値になるため、
        // 既定でも「親がいる」ことだけは落とさない (summary では -1 を省く)。
        if (full || node.parentNodeId != -1) item.Set("parentNodeId", JsonValue(node.parentNodeId));
        if (!node.attachBone.empty()) item.Set("attachBone", JsonValue(node.attachBone));
        if (node.type == asset::VFXNodeType::Particle) {
            item.Set("maxParticles", JsonValue(node.particle.maxParticles));
            // 「要求」と「実際に走る経路」を分けて返す。同じ値だと縮退に気づけない。
            const auto gpuFallback = scene::GetParticleGpuFallbackReason(node.particle);
            item.Set("simulation", JsonValue(node.particle.simulationMode == scene::ParticleSimulationMode::Gpu
                ? "GPU" : "CPU"));
            item.Set("effectiveSimulation",
                     JsonValue(gpuFallback == scene::ParticleGpuFallbackReason::None ? "GPU" : "CPU"));
            if (node.particle.simulationMode == scene::ParticleSimulationMode::Gpu
                && gpuFallback != scene::ParticleGpuFallbackReason::None) {
                item.Set("gpuFallbackField",
                         JsonValue(std::string(scene::ParticleGpuFallbackFieldName(gpuFallback))));
            }
            item.Set("bursts", JsonValue(static_cast<int>(node.particle.bursts.size())));
            item.Set("collision", JsonValue(static_cast<int>(node.particle.collisionMode)));
        } else if (node.type == asset::VFXNodeType::SubGraph) {
            item.Set("graphPath", JsonValue(node.subGraph.graphPath));
        }
        nodes.Push(std::move(item));
    }
    result.Set("nodes", std::move(nodes));
    JsonValue links = JsonValue::MakeArray();
    for (const auto& link : graph.links) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("from", JsonValue(link.fromNode));
        item.Set("to", JsonValue(link.toNode));
        item.Set("trigger", JsonValue(asset::VFXLinkTriggerName(link.trigger)));
        item.Set("delay", JsonValue(link.delay));
        links.Push(std::move(item));
    }
    result.Set("links", std::move(links));
    // グループは canvas 上の見た目のまとまりで、実行にも保存内容にも影響しない。
    // summary では件数だけ返し、レイアウトを編集するときだけ full で中身を読む。
    JsonValue groups = JsonValue::MakeArray();
    if (!full) result.Set("groupCount", JsonValue(static_cast<int>(graph.groups.size())));
    for (const auto& group : graph.groups) {
        if (!full) break;
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(group.id));
        item.Set("title", JsonValue(group.title));
        item.Set("note", JsonValue(group.note));
        item.Set("x", JsonValue(group.x));
        item.Set("y", JsonValue(group.y));
        item.Set("width", JsonValue(group.width));
        item.Set("height", JsonValue(group.height));
        JsonValue color = JsonValue::MakeArray();
        color.Push(JsonValue(group.color.x));
        color.Push(JsonValue(group.color.y));
        color.Push(JsonValue(group.color.z));
        color.Push(JsonValue(group.color.w));
        item.Set("color", std::move(color));
        groups.Push(std::move(item));
    }
    result.Set("groups", std::move(groups));
    JsonValue parameters = JsonValue::MakeArray();
    for (const auto& parameter : graph.parameters) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(parameter.name));
        item.Set("type", JsonValue(static_cast<int>(parameter.type)));
        item.Set("hasRange", JsonValue(parameter.hasRange));
        item.Set("minimum", JsonValue(parameter.minimum));
        item.Set("maximum", JsonValue(parameter.maximum));
        item.Set("source", JsonValue(static_cast<int>(parameter.defaultValue.source.index())));
        parameters.Push(std::move(item));
    }
    result.Set("parameters", std::move(parameters));
    JsonValue bindings = JsonValue::MakeArray();
    for (const auto& binding : graph.bindings) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("paramName", JsonValue(binding.paramName));
        item.Set("nodeId", JsonValue(binding.nodeId));
        item.Set("schemaPath", JsonValue(binding.schemaPath));
        bindings.Push(std::move(item));
    }
    result.Set("bindings", std::move(bindings));
    JsonValue variants = JsonValue::MakeArray();
    for (const auto& variant : graph.variants) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(variant.name));
        item.Set("overrideCount", JsonValue(static_cast<int>(variant.overrides.size())));
        variants.Push(std::move(item));
    }
    result.Set("variants", std::move(variants));
    JsonValue forwards = JsonValue::MakeArray();
    for (const auto& forward : graph.subGraphForwards) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(forward.nodeId));
        item.Set("parentParam", JsonValue(forward.parentParam));
        item.Set("childParam", JsonValue(forward.childParam));
        forwards.Push(std::move(item));
    }
    result.Set("subGraphForwards", std::move(forwards));
    // シグナルの演算グラフは中身を読む必要があるときだけ返す。
    // summary では「シグナルを使っているか」だけ判れば十分で、それは signalOutputs で判る。
    JsonValue signalNodes = JsonValue::MakeArray();
    if (!full) result.Set("signalNodeCount", JsonValue(static_cast<int>(graph.signalNodes.size())));
    for (const auto& signal : graph.signalNodes) {
        if (!full) break;
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(signal.id));
        item.Set("operation", JsonValue(static_cast<int>(signal.operation)));
        item.Set("inputA", JsonValue(signal.inputA));
        item.Set("inputB", JsonValue(signal.inputB));
        item.Set("valueA", JsonValue(signal.valueA));
        item.Set("valueB", JsonValue(signal.valueB));
        signalNodes.Push(std::move(item));
    }
    result.Set("signalNodes", std::move(signalNodes));
    JsonValue signalOutputs = JsonValue::MakeArray();
    for (const auto& output : graph.signalOutputs) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(output.name));
        item.Set("nodeId", JsonValue(output.nodeId));
        signalOutputs.Push(std::move(item));
    }
    result.Set("signalOutputs", std::move(signalOutputs));
    const auto cost = asset::CalculateVFXGraphBudget(graph);
    JsonValue budget = JsonValue::MakeObject();
    budget.Set("particles", JsonValue(cost.particles));
    budget.Set("particleLimit", JsonValue(graph.maxParticles));
    budget.Set("lights", JsonValue(cost.lights));
    budget.Set("lightLimit", JsonValue(graph.maxLights));
    budget.Set("audioVoices", JsonValue(cost.audioVoices));
    budget.Set("audioLimit", JsonValue(graph.maxAudioVoices));
    budget.Set("withinBudget", JsonValue(cost.particles <= graph.maxParticles
        && cost.lights <= graph.maxLights && cost.audioVoices <= graph.maxAudioVoices));
    result.Set("budget", std::move(budget));
    if (!full) {
        result.Set("hint", JsonValue(std::string(
            "detail=\"summary\" のため、ノードの transform / editorPosition、group、"
            "signalNode の中身は返していません。空間配置やキャンバス配置を編集するときだけ "
            "detail=\"full\" で読み直してください。"
            "個々のフィールドの現在値は vfx.nodeField (vfx_node_get_field) で 1 つずつ引けます。")));
    }
    return Outcome::Ok(std::move(result));
}

// vfx.nodeField — vfx.node.setField の対になる読み出し。
//
// WHY: 設定を書く手段 (setField) はあるのに読む手段が無く、blendMode / texturePath /
//      colorGradient に「今何が入っているか」を確認できなかった。
//      vfx.graph は構造しか返さず、node.components は Scene のノード用で
//      VFX Graph のノード id とは別空間なので使えない。
//      現在値を知らないまま書くと、変更が効いたのかどうかもプレビュー画像からしか
//      判断できず、反復が「変えて見る」の繰り返しになる。
//
// schemaPath 省略時はそのノードの全 leaf を返す。prefix を渡せば
// "particle." のように部分木だけへ絞れる (全 leaf は Particle で 100 個を超えるため)。
Outcome DoVFXNodeField(const JsonValue& payload)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    const JsonValue* nodeIdValue = payload.Find("nodeId");
    if (nodeIdValue == nullptr || !nodeIdValue->IsNumber())
        return Outcome::Err("BAD_ARG", "nodeId が必要です");
    const int nodeId = nodeIdValue->AsInt();

    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::LoadVFXGraphAsset(path, graph, &error)) return Outcome::Err("VFX_INVALID", error);
    const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
    if (node == graph.nodes.end())
        return Outcome::Err("BAD_ARG", "nodeId が存在しません: " + std::to_string(nodeId));

    // 読み出し専用なので const オーバーロードを選ばせる。
    // 非 const で解決すると set を持たないプロパティが leaf として弾かれ、
    // 「読めるはずの値が読めない」という書き込み側の制約を読み出しへ持ち込んでしまう。
    const asset::VFXGraphNode* const target = &*node;

    // 値 1 つを JSON 化する共通処理。setField の value と同じ表現で返すことが要件。
    const auto readField = [target](const std::string& schemaPath, JsonValue& item) -> bool {
        reflection::ResolvedProperty resolved;
        if (!reflection::ResolveProperty(asset::GetVFXNodeSchema(), target, schemaPath, resolved)
            || resolved.property == nullptr) return false;
        item.Set("schemaPath", JsonValue(schemaPath));
        item.Set("type", JsonValue(std::string(PropertyTypeName(resolved.property->type))));
        item.Set("value", SchemaValueToJson(resolved.property->type,
                                            resolved.property->get(resolved.constOwner)));
        // enum は数値のままだと意味が読めない。選択肢と現在の名前を添える。
        if (!resolved.property->enumNames.empty()) {
            const std::any raw = resolved.property->get(resolved.constOwner);
            if (const auto* index = std::any_cast<int>(&raw);
                index != nullptr && *index >= 0
                && static_cast<std::size_t>(*index) < resolved.property->enumNames.size())
                item.Set("enumName", JsonValue(std::string(resolved.property->enumNames[
                    static_cast<std::size_t>(*index)])));
        }
        return true;
    };

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("nodeId", JsonValue(nodeId));
    result.Set("nodeType", JsonValue(std::string(asset::VFXNodeTypeName(node->type))));
    result.Set("name", JsonValue(node->name));

    if (const std::string schemaPath = StringField(payload, "schemaPath"); !schemaPath.empty()) {
        JsonValue item = JsonValue::MakeObject();
        if (!readField(schemaPath, item))
            return Outcome::Err("UNKNOWN_FIELD",
                                "schemaPath を解決できません: " + schemaPath
                                + " (有効な path は vfx.schema、または schemaPath を省略して"
                                  "このノードの leaf 一覧を取得してください)");
        result.Set("field", std::move(item));
        return Outcome::Ok(std::move(result));
    }

    // 全 leaf。走査対象は「このノードの実インスタンス」なので、bursts[N] は実要素数ぶん出る。
    const std::string prefix = StringField(payload, "prefix");
    std::vector<std::string> paths;
    CollectVFXSchemaPaths(asset::GetVFXNodeSchema(), target, {}, paths);
    JsonValue fields = JsonValue::MakeArray();
    int skipped = 0;
    for (const std::string& schemaPath : paths) {
        if (!prefix.empty() && schemaPath.rfind(prefix, 0) != 0) { ++skipped; continue; }
        JsonValue item = JsonValue::MakeObject();
        if (readField(schemaPath, item)) fields.Push(std::move(item));
    }
    result.Set("fields", std::move(fields));
    result.Set("filteredOut", JsonValue(skipped));
    result.Set("hint", JsonValue(std::string(
        "value は vfx.node.setField (vfx_node_set_field) の value へそのまま渡せる形です。"
        "Curve / Gradient は {interp, keys} で返るので、keys を書き換えて渡せば往復します。"
        "全 leaf は Particle ノードだけで 100 個を超えます。"
        "prefix=\"particle.\" のように絞るか、schemaPath を指定して 1 つだけ読んでください。")));
    return Outcome::Ok(std::move(result));
}

Outcome DoVFXParams(editor::EditorContext& ctx, const JsonValue& payload)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::LoadVFXGraphAsset(path, graph, &error)) return Outcome::Err("VFX_INVALID", error);

    scene::VFXGraphComponent fallback;
    fallback.graphPath = path;
    scene::VFXGraphComponent* instance = nullptr;
    const std::string id = StringField(payload, "id");
    if (!id.empty()) {
        GameObject* object = ctx.activeScene != nullptr ? ctx.activeScene->FindByGuid(id) : nullptr;
        instance = object != nullptr ? object->GetComponent<scene::VFXGraphComponent>() : nullptr;
        if (instance == nullptr) return Outcome::Err("NOT_PRESENT", "対象にVFXGraphComponentがありません");
    }
    const scene::VFXGraphComponent& sourceComponent = instance != nullptr ? *instance : fallback;
    const float normalizedTime = instance != nullptr && instance->graphDuration > 0.0f
        ? std::clamp(instance->playTime / instance->graphDuration, 0.0f, 1.0f) : 0.0f;

    JsonValue parameters = JsonValue::MakeArray();
    for (const auto& definition : graph.parameters) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(definition.name));
        item.Set("type", JsonValue(VFXParamTypeName(definition.type)));
        item.Set("default", VFXParamValueJson(graph, definition.defaultValue, normalizedTime, 0, definition.name));
        item.Set("hasRange", JsonValue(definition.hasRange));
        if (definition.hasRange) {
            item.Set("minimum", JsonValue(definition.minimum));
            item.Set("maximum", JsonValue(definition.maximum));
        }
        const bool overridden = std::any_of(sourceComponent.parameterOverrides.begin(),
            sourceComponent.parameterOverrides.end(), [&](const auto& value) { return value.paramName == definition.name; });
        item.Set("overridden", JsonValue(overridden));
        if (const auto* effective = asset::ResolveVFXParamSource(graph, sourceComponent, definition))
            item.Set("effective", VFXParamValueJson(graph, *effective, normalizedTime, 0, definition.name));
        JsonValue bindings = JsonValue::MakeArray();
        for (const auto& binding : graph.bindings) {
            if (binding.paramName != definition.name) continue;
            JsonValue bindingJson = JsonValue::MakeObject();
            bindingJson.Set("nodeId", JsonValue(binding.nodeId));
            bindingJson.Set("schemaPath", JsonValue(binding.schemaPath));
            bindings.Push(std::move(bindingJson));
        }
        item.Set("bindings", std::move(bindings));
        parameters.Push(std::move(item));
    }
    JsonValue staleOverrides = JsonValue::MakeArray();
    for (const auto& value : sourceComponent.parameterOverrides) {
        if (asset::FindVFXParameter(graph, value.paramName) == nullptr) staleOverrides.Push(JsonValue(value.paramName));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("instanceId", JsonValue(id));
    result.Set("variant", JsonValue(sourceComponent.variant));
    result.Set("normalizedTime", JsonValue(normalizedTime));
    result.Set("parameters", std::move(parameters));
    result.Set("staleOverrides", std::move(staleOverrides));
    return Outcome::Ok(std::move(result));
}

// AI へ提示するノード型の一覧。VFXNodeType の全値をここで列挙する。
// WHY: 個別に書き並べると型を足したときに必ず追従漏れが出て、
//      「エンジンにはあるのに AI からは存在しないノード」が生まれる。
//      末尾の static_assert が、列挙漏れをビルド時に落とす。
constexpr asset::VFXNodeType ALL_VFX_NODE_TYPES[] = {
    asset::VFXNodeType::Entry, asset::VFXNodeType::Delay, asset::VFXNodeType::Particle,
    asset::VFXNodeType::Trail, asset::VFXNodeType::MeshTrail, asset::VFXNodeType::Light,
    asset::VFXNodeType::Audio, asset::VFXNodeType::Decal, asset::VFXNodeType::SubGraph,
    asset::VFXNodeType::ForceField, asset::VFXNodeType::Mesh, asset::VFXNodeType::ScreenEffect,
    asset::VFXNodeType::CameraShake, asset::VFXNodeType::TimeScale, asset::VFXNodeType::Wind,
    asset::VFXNodeType::Reroute, asset::VFXNodeType::AnimatedMesh,
};
static_assert(std::size(ALL_VFX_NODE_TYPES)
                  == static_cast<std::size_t>(asset::VFXNodeType::AnimatedMesh) + 1,
              "VFXNodeType を追加したら ALL_VFX_NODE_TYPES と ParseVFXNodeType も更新すること");

Outcome DoVFXSchema()
{
    JsonValue fields = JsonValue::MakeArray();
    AppendSchemaProperties(fields, asset::GetVFXNodeSchema(), {});
    JsonValue nodeTypes = JsonValue::MakeArray();
    for (const auto type : ALL_VFX_NODE_TYPES)
        nodeTypes.Push(JsonValue(asset::VFXNodeTypeName(type)));
    JsonValue result = JsonValue::MakeObject();
    result.Set("schema", JsonValue(std::string(asset::GetVFXNodeSchema().TypeName())));
    result.Set("nodeTypes", std::move(nodeTypes));
    result.Set("fields", std::move(fields));
    return Outcome::Ok(std::move(result));
}

// vfx.curvePresets — 名前付きカーブプリセットの目録を返す。
// WHY: エフェクトが AAA に見えるかは時間曲線の形で決まるが、AI が (time,value) を
//      生で並べても意図した形になった保証が無く、外したときも画像から逆算できない。
//      Editor の UI と同じ表 (ParticleCurvePresets.hpp) を返すことで、
//      AI は "Spike" を選ぶだけで正しい形から始められ、微調整だけを画像評価に回せる。
Outcome DoVFXCurvePresets()
{
    JsonValue presets = JsonValue::MakeArray();
    for (const asset::ParticleCurvePreset& preset : asset::ParticleCurvePresets()) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(std::string(preset.name)));
        item.Set("description", JsonValue(std::string(preset.description)));
        item.Set("interpolation", JsonValue(static_cast<int>(preset.interpolation)));
        item.Set("keyCount", JsonValue(static_cast<int>(preset.keyCount)));
        JsonValue keys = JsonValue::MakeArray();
        for (std::uint32_t index = 0; index < preset.keyCount; ++index) {
            JsonValue key = JsonValue::MakeArray();
            key.Push(JsonValue(static_cast<double>(preset.keys[index][0])));
            key.Push(JsonValue(static_cast<double>(preset.keys[index][1])));
            keys.Push(std::move(key));
        }
        item.Set("keys", std::move(keys));
        presets.Push(std::move(item));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("presets", std::move(presets));
    result.Set("usage", JsonValue(std::string(
        "vfx.node.setField の value に {\"preset\":\"Spike\",\"scale\":1.0} を渡すと適用されます。"
        "生のキーを指定する場合は {\"interp\":0|1|2,\"keys\":[[t,v],...]} (最大 8 キー)。"
        "interp は 0=Linear 1=Step 2=Smooth。")));
    return Outcome::Ok(std::move(result));
}

// vfx.guide — このエンジンで「見られるエフェクト」を作るための規約を機械可読で返す。
//
// WHY: AI がゼロから .vfx を組むと、DAG 検証も lint も通るのに見た目が破綻する、という
//      失敗の仕方をする。実際、同梱テンプレート 5 件のうち 4 件が
//      「回転×非等方でせん断」「lightingStrength>1 で煙が黒く潰れる」
//      「bind 済みフィールドの編集が実行時に死ぬ」を踏んでいた。人間が作っても同じである。
//      これらは画像を見ても原因が分からない類なので、反復では収束しない。
//      作る前に規約を渡し、作った後に lint で同じ規約を検査する二段構えにする。
//
// rule 各項目の lintCode は、その規約を機械的に検査している vfx.lint の issue code。
// 空文字は「検査できないが守るべき設計原則」で、AI 側の判断に委ねる部分を明示する。
Outcome DoVFXGuide()
{
    struct Rule {
        const char* topic;
        const char* rule;
        const char* why;
        const char* lintCode;
    };
    static constexpr Rule kRules[] = {
        { "material",
          "テクスチャを割り当てる前に必ず vfx.textureAnalyze でその素材を解析する。"
          "blendMode / alphaSource / spriteColumns / spriteRows / softParticles は"
          "素材の中身で正解が変わり、ファイル名やパスからは判断できない。",
          "素材を見ずに設定を決めると、アルファが機能していない素材で矩形の板が描かれる、"
          "事前乗算素材の縁が黒く縁取られる、アトラスをコマ割りせず 1 枚絵として貼る、"
          "といった失敗をする。いずれもプレビュー画像から原因を特定できないため、"
          "反復しても収束しない。観測すれば機械的に決まる項目を推測に任せないこと。", "" },
        { "material",
          "materialPath を設定した Emitter では、blendMode は実行時に .mat の blend_mode で"
          "上書きされる。ブレンドを変えるなら Emitter ではなく .mat を編集する。"
          ".mat を割り当てている場合は vfx.materialAnalyze で実際に効く設定を確認する。",
          "materialPath が描画設定の単一の信頼元になる設計なので、Emitter 側の blendMode は"
          "保存はされても実行時に使われない。「Additive にしたのに Alpha で描かれる」という"
          "形でしか現れず、値を見ても原因が判らない。", "" },
        { "material",
          "シェーダー変数名を要求する設定 (.mat の params / Mesh ノードの animatedParam) は、"
          "書く前に shader.inspect で実在する名前を確認する。",
          "存在しない名前を書いても保存は通り、実行時は黙って無視される。"
          "「値を変えても絵が変わらない」としか見えず、綴り違いに最後まで気付けない。"
          "未使用変数はコンパイル時に消えるため、HLSL の宣言を読むだけでは不十分で、"
          "コンパイル済みバイトコードのリフレクション結果を見る必要がある。",
          "UNKNOWN_SHADER_PARAM" },
        { "material",
          "解析の alpha.isMeaningful=false なら alphaSource=1 (Luminance) が必須。"
          "alpha.likelyPremultiplied=true なら blendMode=2 (Premultiplied)。",
          "アルファチャンネルが全画素 1.0 の素材は珍しくない (RGB だけで作られた炎など)。"
          "TextureAlpha のままでは全面不透明として描かれ、粒子が矩形の板になる。", "" },
        { "blending",
          "炎の本体は Alpha (blendMode=1) + sortMode=1、発光する芯だけ Additive (blendMode=0)。"
          "芯の renderPriority を本体より大きくして手前に描く。",
          "全レイヤーを加算にすると数十枚の重なりが白飽和し、輪郭の無い光の玉になる。"
          "物体として見えるには背景を隠す不透明な body が要る。", "" },
        { "blending",
          "Alpha / Premultiplied のエミッターは必ず sortMode=1 (BackToFront)。",
          "半透明の重なりは描画順で結果が変わり、None だとフレームごとにちらつく。",
          "ALPHA_NO_SORT" },
        { "motion",
          "上向きの加速度は emitVelocity・gravity・ForceField のうち 1 つだけが持つ。"
          "推奨は ForceField (Wind) に集約し、各エミッターの gravity は 0。",
          "三重に計上すると発生半径の 10 倍以上吹き上がり、焚き火ではなくガスの噴流になる。"
          "1 か所に集約すると、そこを触るだけで全レイヤーの伸びが揃って変わる。", "" },
        { "motion",
          "火の粉など一部の粒子には下向き gravity を与え、上昇風の外へ抜けて落ちるようにする。",
          "全部が上がりっぱなしだと「立ち上る点の列」にしか見えず、熱気の境界が出ない。", "" },
        { "sprite",
          "回転 (angularVelocity / rotationCurve) と非等方 sizeAxisScale は排他。"
          "縦に伸ばす層は回さない、回す層は sizeAxisScale=[1,1,1]。",
          "Particle.hlsl は回転の後に軸倍率を掛けるため、両立させるとスプライトが"
          "平行四辺形へせん断される。両立させる方法は無い。", "SHEARED_SPRITE" },
        { "sprite",
          "sizeEnd は sizeStart の 4〜6 割程度に留め、0 付近にしない。",
          "0 へ収束させると上がるほど点になり、炎の舌や煙の広がりにならない。"
          "尖らせるのは寿命とアルファの役目。", "" },
        { "color",
          "colorGradient のアルファは 0 で始まり 0 で終える (中間にピークを置く)。",
          "端が 0 でないと粒子が発生・消滅する瞬間にポップして、板ポリの出入りが見える。", "" },
        { "lighting",
          "sixWayLighting を使う場合 lightingStrength は 1.0 以下 (推奨 0.5〜0.8)。",
          "シェーダー側で saturate されるため 1.0 超は「元の色を捨てて ambient+N·L で塗る」"
          "意味しか持たず、暗い環境で煙が黒く潰れる。", "LIGHTING_SATURATED" },
        { "layering",
          "renderPriority は 煙(0) < 炎本体(20) < 芯(30) < 火の粉(40) < 歪み(100) の順。",
          "歪み (distortion) は背景を屈折させるため必ず最後。煙が炎より手前に来ると"
          "光っているはずの炎が濁る。", "" },
        { "hierarchy",
          "link は「いつ動くか」だけを決める。位置の入れ子は parentNodeId で別に指定する。"
          "衝撃波から link で繋いだ煙は、衝撃波の位置を継承しない。"
          "一緒に動かしたいなら vfx.node.setParent で親子にすること。",
          "この 2 つは別の軸で、link 1 本に兼任させると「傾けたいだけなのに発火順まで変わる」"
          "形で必ず破綻する。逆に、親子にしただけでは発火順は変わらない。", "" },
        { "hierarchy",
          "親に指定できるのは実体を持つノードだけ。Entry と Delay は時間だけのノードなので"
          "親にすると実行時に owner 直下へ落ちる。attachBone を指定したノードは"
          "socket 追従が優先され、parentNodeId は無視される。",
          "どちらも「設定したのに効かない」という形でしか現れず、"
          "プレビュー画像からは原因を特定できない。",
          "PARENT_HAS_NO_TRANSFORM" },
        { "hierarchy",
          "親ノードの実行時エンベロープ (Mesh の膨張スケールなど) は子へ波及しない。"
          "継承されるのはオーサリング値の Position/Rotation/Scale だけ。",
          "実体同士を直接親子にすると親の再生終了で子も消えるため、ランタイムは"
          "Transform だけを持つグループを間に挟んでいる。"
          "「衝撃波が 40 倍に膨らむと煙も 40 倍になる」ことは無い。", "" },
        { "parameters",
          "公開パラメーターに bind した leaf は、ノード側ではなく parameter の default を編集する。"
          "1 つの param が複数ノードを駆動する場合、全ノードの値を揃えておく。",
          "bind 済み leaf は生成時に必ず param 値で上書きされるため、"
          "ノード側の編集は保存されても実行時には使われない。", "BOUND_FIELD_OVERRIDDEN" },
        { "curves",
          "時間曲線は vfx.curvePresets の名前付きプリセットから始め、そこから微調整する。"
          "setField の value に {\"preset\":\"Spike\"} を渡す。",
          "エフェクトの質を最も左右するのは時間曲線の形。生のキー列を書くと"
          "意図した形になったかを画像からしか確認できず、反復が収束しない。", "" },
        { "budget",
          "粒子数の budget 内に収めたうえで、vfx.runtime の cost を見る。"
          "particlePassGpuMs が 1ms を超えていて overdraw.meanLayers も大きいなら原因は fill rate で、"
          "粒子数ではなく vfx.optimize(strategy=\"fillRate\") で「粒を大きくして枚数を減らす」を選ぶ。",
          "実際のボトルネックは粒子数ではなく fill rate であることが多く、"
          "大きな半透明板の重なりは粒子数からは見えない。"
          "fill rate が原因のときに粒子数を減らすのは効きが悪く、見た目だけが痩せる。",
          "OVER_BUDGET_PARTICLES" },
        { "budget",
          "simulationMode = Gpu にしただけでは GPU で回るとは限らない。"
          "simulationSpace=Local / per-particle Trail / SubEmitter / prewarm / "
          "flipbookFrameBlending / selfShadowStrength > 0 / Depth 以外の collision の"
          "いずれかがあると黙って CPU へ縮退する。"
          "sortMode と meshParticlePath は GPU と併用できる (GPU ソートとインスタンス描画で対応済み)。"
          "vfx.lint の GPU_FALLBACK と vfx.runtime の simulation.effective で必ず確認する。",
          "縮退に気づかないまま粒子数だけ増やすと、性能は一切使われないまま CPU 負荷だけが上がる。",
          "GPU_FALLBACK" },
        { "workflow",
          "vfx.assetSurvey で手持ち素材を棚卸し → vfx.template.apply で骨格を複製 → "
          "使う素材を vfx.textureAnalyze (.mat 割当時は vfx.materialAnalyze) で解析 → "
          "recommendations を setField で適用 → param で調整 → vfx.lint → "
          "vfx.previewEnsure で Preview World を起動 → "
          "vfx.previewMetrics で issues が空になるまで直す → "
          "vfx.previewCurve で時間の形 (立ち上がり・ピーク位置・消え際) を評価 → "
          "残った「らしさ」だけを vfx.preview の画像で詰める → 反復。",
          "ゼロからノードを並べるより、検証済みテンプレートを出発点にする方が失敗率が低い。"
          "棚卸しを先にするのは、recipe が要求する層を作れる素材が手元にあるとは限らないため。"
          "無い素材を前提にしたグラフを組んでも、後から代替を探し直すことになる。"
          "素材の解析を先に済ませると、blendMode や alphaSource のような"
          "「画像を見ても原因が判らない」種類の誤りが最初から入らない。"
          "lint は画像に写らない破綻を文章で返すので、capture より先に必ず通す。"
          "Preview World は Editor 本体ではなく独立プロセス FBZZVFXEditor が所有するため、"
          "起動していなければ preview 系は全て NO_PREVIEW_WORLD になる。"
          "vfx.previewEnsure がその起動と初期化完了までを引き受ける。", "" },
        { "expression",
          "太い帯 (剣閃・魔法の軌跡・リボン状の炎) は particle.trailRibbon = true にする。"
          "per-particle Trail の既定はビルボードを履歴点へ並べる方式で、太くすると"
          "必ず粒の連なりが露見する。帯の幅は trailRibbonWidth (0 で粒子サイズ)。",
          "「線に見えるまで点を細かく打つ」のは細い軌跡までしか通用しない。"
          "帯が主役の表現では、履歴点をポリラインとみなして 1 枚の面を張るしかない。", "" },
        { "expression",
          "厚みのある煙・雲には particle.selfShadowStrength を入れる。"
          "受け影 (receiveShadows) は他の物体が落とす影しか扱わないため、"
          "これが無いと粒子をいくら重ねても光の当たり方が一様で平坦な塊に見える。",
          "自己影は光源側の密度から減衰させる近似で、CPU 頂点バッファを要求するため"
          "GPU シミュレーションとは併用できない (vfx.lint の GPU_FALLBACK が名指しする)。"
          "volumetric と併用すると、雲を貫く光の筋 (光の柱) がボリューム内部に現れる。", "" },
        { "expression",
          "歪み (distortion) を複数重ねるときは、重ねる順に renderPriority を付ける。"
          "背景の退避は歪みエミッターの描画直前に取り直されるため、"
          "順序が決まっていれば後ろの歪みが手前の歪みへ正しく伝わる。",
          "同一エミッター内で重なる粒子は 1 DrawCall なので同じ背景を共有する。"
          "粒子単位の前後関係が要るなら、歪みを別エミッターへ分けるしかない。", "" },
        { "expression",
          "焼け跡・血痕・着弾痕の Decal は angleFadeStrength を 0 にしない。"
          "既定 (1.0 / 70 度) のままなら、壁と床の角をまたいだ部分が自動的に消える。",
          "OBB 投影は投影軸に対して斜めな面へ当てるとテクスチャが引き伸ばされ、"
          "「伸びた汚れ」として露見する。角度で薄めれば破綻する範囲がそのまま消える。", "" },
        { "diagnosis",
          "ノードが画に出ないときは、画像を睨む前に vfx.runtime で実行状態を見る。"
          "active=false かつ waitingForEvent=true なら OnCollision / OnDeath 待ちで、"
          "その事象が起きない限り永久に起動しない。",
          "「出ない」原因は起動していない / イベント待ち / 起動しているが見えない の 3 通りで、"
          "画像からは区別できない。前 2 つは実行状態を見れば即断でき、"
          "残った 1 つだけが画像で判断すべき問題になる。", "" },
        { "diagnosis",
          "見た目の原因が画像から分からないときは vfx.preview の view を切り替える。"
          "gizmos = 力場の半径・向きとエミッター形状/初速、overdraw = 重なり枚数。",
          "力場もエミッター形状も見えない体積なので、通常の絵からは"
          "「半径が足りないのか強さが足りないのか」を切り分けられない。", "" },
        { "evaluation",
          "画像を目で見て直す前に vfx.previewMetrics の issues を空にする。"
          "BLOWN_OUT / SCREEN_FLOODED / EMPTY_FRAME / STATIC_FRAME / OFF_CENTER は"
          "機械的に判る破綻で、これが残っているうちは画像を睨んでも意味がない。",
          "同じ画から毎回違う結論が出るのは「少し暗い」の“少し”に基準が無いため。"
          "先に数値で潰せる破綻を潰すと、残った判断だけを画像へ委ねられ、反復が収束する。"
          "指標が良くても「炎に見えない」ことはあるので、置き換えではなく前段として使う。", "" },
        { "evaluation",
          "エフェクトの質は静止画ではなく時間の形で決まる。vfx.previewCurve で"
          "peakNormalized (ピーク位置) と tailRatio (消え際) を見る。"
          "爆発はピークが 0.15 より手前、煙や炎は中盤〜後半が目安。"
          "tailRatio が 0.5 を超えていれば再生終了時点で消えていない。",
          "立ち上がりの速さ・ピークの位置・消え際の粘りは、t=0/peak/end の 3 枚を見ても判定できない。", "" },
        { "evaluation",
          "ビルボードは横から見ると平面なので、シルエットの破綻は vfx.preview の"
          "camera に yaw:90 を渡さない限り絶対に判らない。"
          "ゲーム内距離で読めるかは vfx.previewSweep で距離を振って確認する。",
          "近接で作り込んだディテールは 10m 先では消え、逆に近くで見ると板が透けているのが判る。"
          "lodNearDistance / lodFarDistance を設定しても、距離を変えなければ切り替わりを一度も見られない。",
          "" },
    };

    JsonValue rules = JsonValue::MakeArray();
    for (const Rule& item : kRules) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("topic", JsonValue(std::string(item.topic)));
        entry.Set("rule", JsonValue(std::string(item.rule)));
        entry.Set("why", JsonValue(std::string(item.why)));
        // 検査可能な規約はどの lint code で落ちるかを明示する。
        // 「守れているか」を AI 自身が確認できる形にしないと、規約は読まれて終わる。
        if (item.lintCode[0] != '\0')
            entry.Set("lintCode", JsonValue(std::string(item.lintCode)));
        rules.Push(std::move(entry));
    }
    // 代表的な層構成。ゼロから積むより、この骨格に沿わせた方が確実に「らしく」なる。
    // 表の実体は VFXRecipeLibrary が持つ。Editor の Recipe ウィザードも同じ表を読むため、
    // 「guide は煙を要求するがウィザードは作らない」という食い違いが起きない。
    JsonValue recipes = JsonValue::MakeArray();
    for (const VFXRecipe& item : GetVFXRecipes()) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(std::string(item.name)));
        entry.Set("summary", JsonValue(std::string(item.summary)));
        entry.Set("layers", JsonValue(std::string(item.layers)));
        // 必要な素材ロール。vfx.assetSurvey の coverage と同じ語彙なので突き合わせられる。
        JsonValue roles = JsonValue::MakeArray();
        for (const auto& role : CollectRecipeRoles(item)) roles.Push(JsonValue(role));
        entry.Set("requiredRoles", std::move(roles));
        recipes.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("rules", std::move(rules));
    result.Set("recipes", std::move(recipes));
    result.Set("note", JsonValue(std::string(
        "lintCode を持つ規約は vfx.lint が機械的に検査する。"
        "持たない規約は検査できないため、適用したかどうかは AI 側で担保すること。"
        "recipe の requiredRoles は vfx.assetSurvey の coverage と同じ語彙なので、"
        "着手前に手持ち素材で足りるかを突き合わせられる。")));
    return Outcome::Ok(std::move(result));
}

// vfx.templateCatalog — 人間と AI が同じプロジェクト固有 Template 集合を参照する。
// WHY: 生成結果を Template へ昇格しても AI が列挙できなければ、次の制作で再利用されず
//      「知識化」にならない。UI と同じ Catalog service を唯一の信頼元として返す。
Outcome DoVFXTemplateCatalog(editor::EditorContext& ctx, const JsonValue& payload)
{
    VFXTemplateCatalog catalog;
    catalog.Scan(ctx, true);

    const std::string query = LowerAscii(StringField(payload, "query"));
    int limit = 64;
    if (const JsonValue* value = payload.Find("limit"); value != nullptr && value->IsNumber())
        limit = std::clamp(value->AsInt(), 1, 256);

    JsonValue templates = JsonValue::MakeArray();
    int matched = 0;
    // 絞り込みは Catalog の searchKey が唯一の正本 (Editor の検索欄と同じ集合を返すため)。
    for (const GraphTemplateEntry* entry : catalog.Filter(query)) {
        if (matched >= limit) break;

        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(entry->name));
        item.Set("path", JsonValue(entry->path));
        item.Set("category", JsonValue(entry->category));
        item.Set("origin", JsonValue(std::string(TemplateOriginName(entry->origin))));
        item.Set("summary", JsonValue(entry->summary));
        // 説明は graph.name への相乗りをやめ、description として持つようにした。
        item.Set("description", JsonValue(entry->description));
        JsonValue tags = JsonValue::MakeArray();
        for (const auto& tag : entry->tags) tags.Push(JsonValue(tag));
        item.Set("tags", std::move(tags));
        JsonValue roles = JsonValue::MakeArray();
        for (const auto& role : entry->requiredRoles) roles.Push(JsonValue(role));
        item.Set("requiredRoles", std::move(roles));
        JsonValue variants = JsonValue::MakeArray();
        for (const auto& variant : entry->variants) variants.Push(JsonValue(variant.name));
        item.Set("variants", std::move(variants));
        // 層 = 部分取り込みの単位。groups に id を渡せばその層だけを merge できる。
        JsonValue layers = JsonValue::MakeArray();
        for (const auto& layer : entry->layers) {
            JsonValue layerItem = JsonValue::MakeObject();
            layerItem.Set("groupId", JsonValue(layer.groupId));
            layerItem.Set("title", JsonValue(layer.title));
            layerItem.Set("note", JsonValue(layer.note));
            layerItem.Set("nodeCount", JsonValue(layer.nodeCount));
            layers.Push(std::move(layerItem));
        }
        item.Set("layers", std::move(layers));
        JsonValue missing = JsonValue::MakeArray();
        for (const auto& path : entry->missingAssets) missing.Push(JsonValue(path));
        item.Set("missingAssets", std::move(missing));
        JsonValue budget = JsonValue::MakeObject();
        budget.Set("particles", JsonValue(entry->particleBudget));
        budget.Set("lights", JsonValue(entry->lightBudget));
        budget.Set("audioVoices", JsonValue(entry->audioBudget));
        item.Set("budget", std::move(budget));
        item.Set("nodeCount", JsonValue(entry->nodeCount));
        item.Set("linkCount", JsonValue(entry->linkCount));
        item.Set("duration", JsonValue(entry->duration));
        item.Set("valid", JsonValue(entry->valid));
        if (!entry->thumbnailPath.empty()) item.Set("thumbnail", JsonValue(entry->thumbnailPath));
        templates.Push(std::move(item));
        ++matched;
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("templates", std::move(templates));
    result.Set("matched", JsonValue(matched));
    result.Set("total", JsonValue(static_cast<int>(catalog.entries.size())));
    result.Set("status", JsonValue(catalog.status));
    result.Set("hint", JsonValue(std::string(
        "採択済み候補は vfx_knowledge_promote で Assets/VFX/Templates へ昇格すると、"
        "以後の vfx_candidate_fork から再利用できます。"
        "既存グラフへ層だけを足すなら vfx_template_apply の mode=merge と groups を使います。"
        "missingAssets が空でない Template は、適用しても該当ノードが描画されません。")));
    return Outcome::Ok(std::move(result));
}

// projectRoot 配下のファイルだけを許す解決。定義はこのファイルの後方にあるため前方宣言する。
bool ResolveProjectFile(const editor::EditorContext& ctx, const std::string& requested,
                        std::filesystem::path& outPath, std::string& outRelative);

// ── Behavior Tree ───────────────────────────────────────────────────────────
// WHY VFX と同じ形にするか: AI から見ると「アセットを読む → 構造を知る → 規約を読む →
//     編集する → 検証する」という流れは VFX グラフと同一で、面の作り方を変える理由が無い。
//     bt.tree / bt.guide / bt.lint / bt.node.* を vfx.* と同じ語彙で揃える。

// .behaviortree を読む。壊れていても構造は返す (AI が直せなければ意味が無い)。
bool LoadBehaviorTreeForAi(editor::EditorContext& ctx, const JsonValue& payload,
                           fbzz::ai::BehaviorTreeAsset& outAsset, std::string& outRelative,
                           std::filesystem::path& outPath, Outcome& outError)
{
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), outPath, outRelative)
        || !std::filesystem::is_regular_file(outPath)) {
        outError = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return false;
    }
    std::string error;
    if (!fbzz::ai::ParseBehaviorTreeAsset(outPath.generic_string(), outAsset, &error)) {
        outError = Outcome::Err("BT_PARSE_FAILED", error);
        return false;
    }
    fbzz::ai::EnsureReservedBlackboardKeys(outAsset);
    return true;
}

JsonValue BehaviorTreeNodeJson(const fbzz::ai::BTNodeDef& node)
{
    JsonValue item = JsonValue::MakeObject();
    item.Set("id", JsonValue(node.id));
    item.Set("parentId", JsonValue(node.parentId));
    // order = 優先度そのもの。これを返さないと AI は「なぜこの枝が先に走るか」を
    // 木の形だけから推測することになり、必ず取り違える。
    item.Set("order", JsonValue(node.order));
    item.Set("type", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
    item.Set("name", JsonValue(node.name));
    item.Set("category", JsonValue(std::string(
        fbzz::ai::BTNodeIsComposite(node.type) ? "composite"
        : fbzz::ai::BTNodeIsDecorator(node.type) ? "decorator"
        : fbzz::ai::BTNodeIsPureCondition(node.type) ? "condition" : "action")));
    item.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(node.type)));
    if (node.abortMode != fbzz::ai::AbortMode::None) {
        const char* names[] = { "none", "self", "lowerPriority", "both" };
        item.Set("abortMode", JsonValue(std::string(names[static_cast<int>(node.abortMode)])));
    }
    if (!node.keyName.empty()) item.Set("keyName", JsonValue(node.keyName));
    if (!node.moveTargetKey.empty()) item.Set("moveTargetKey", JsonValue(node.moveTargetKey));
    if (!node.animatorTrigger.empty()) item.Set("animatorTrigger", JsonValue(node.animatorTrigger));
    if (!node.scriptMethod.empty()) item.Set("scriptMethod", JsonValue(node.scriptMethod));
    if (!node.soundPath.empty()) item.Set("soundPath", JsonValue(node.soundPath));
    item.Set("duration", JsonValue(node.duration));
    item.Set("editorX", JsonValue(node.editorX));
    item.Set("editorY", JsonValue(node.editorY));
    return item;
}

// bt.tree — 木の構造・Blackboard・検証結果をまとめて返す。
Outcome DoBehaviorTree(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    JsonValue nodes = JsonValue::MakeArray();
    // order 順に並べて返す。配列の順序が優先度と一致していないと、
    // AI が「上から順に読めば優先順位」という自然な読み方をした瞬間に間違える。
    std::vector<const fbzz::ai::BTNodeDef*> sorted;
    for (const auto& node : asset.nodes) sorted.push_back(&node);
    std::sort(sorted.begin(), sorted.end(),
        [](const fbzz::ai::BTNodeDef* a, const fbzz::ai::BTNodeDef* b) {
            if (a->parentId != b->parentId) return a->parentId < b->parentId;
            return a->order < b->order;
        });
    for (const fbzz::ai::BTNodeDef* node : sorted) nodes.Push(BehaviorTreeNodeJson(*node));

    JsonValue blackboard = JsonValue::MakeArray();
    for (const auto& def : asset.blackboard) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(def.name));
        item.Set("type", JsonValue(std::string(fbzz::ai::BlackboardTypeName(def.type))));
        // 予約キーは PerceptionSystem 等が固定添字で書く。改名も削除もできない。
        item.Set("reserved", JsonValue(def.reserved));
        blackboard.Push(std::move(item));
    }

    JsonValue roots = JsonValue::MakeArray();
    for (const int rootId : asset.FindRootIds()) roots.Push(JsonValue(rootId));

    std::string validateError;
    const bool valid = fbzz::ai::ValidateBehaviorTreeAsset(asset, &validateError);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("name", JsonValue(asset.name));
    result.Set("description", JsonValue(asset.description));
    result.Set("nodes", std::move(nodes));
    result.Set("blackboard", std::move(blackboard));
    result.Set("roots", std::move(roots));
    result.Set("valid", JsonValue(valid));
    if (!valid) result.Set("error", JsonValue(validateError));
    result.Set("nextNodeId", JsonValue(asset.nextNodeId));
    result.Set("hint", JsonValue(std::string(
        "order が優先度そのもの。Selector では小さいほど先に試される。"
        "abortMode=lowerPriority は純粋条件ノードにのみ設定でき、"
        "これが無いと「巡回中に敵を見つけても着くまで反応しない」AI になる。")));
    return Outcome::Ok(std::move(result));
}

// bt.lint — 保存は通るが意図どおりに動かない構成を返す。
Outcome DoBehaviorTreeLint(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    JsonValue issues = JsonValue::MakeArray();
    for (const auto& warning : fbzz::ai::CollectBehaviorTreeWarnings(asset)) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(warning.nodeId));
        item.Set("code", JsonValue(warning.code));
        item.Set("message", JsonValue(warning.message));
        issues.Push(std::move(item));
    }
    std::string validateError;
    const bool valid = fbzz::ai::ValidateBehaviorTreeAsset(asset, &validateError);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("issues", std::move(issues));
    result.Set("valid", JsonValue(valid));
    if (!valid) result.Set("error", JsonValue(validateError));
    result.Set("note", JsonValue(std::string(
        "valid=false は保存が拒否される致命的な不整合 (ルートが 0/2 個・循環・子数超過)。"
        "issues は保存できるが意図どおり動かない構成。")));
    return Outcome::Ok(std::move(result));
}

// bt.guide — 木を組む前に読む規約。VFX の vfx.guide と同じ位置づけ。
Outcome DoBehaviorTreeGuide()
{
    struct Rule { const char* topic; const char* rule; const char* why; };
    static constexpr Rule kRules[] = {
        { "structure",
          "Selector の子は「やりたいことの優先順位」で並べる。order が小さいほど先に試される。"
          "戦闘 → 追跡 → 巡回 → 待機 のように、緊急度の高い枝を必ず左 (小さい order) へ置く。",
          "BT の挙動は木の形ではなく order で決まる。並べ替えを怠ると、"
          "「巡回が先に Success して戦闘へ入らない」という形で静かに壊れる。" },
        { "structure",
          "Sequence は AND、Selector は OR。「条件を確かめてから行動する」は "
          "Sequence(条件, 行動) で書く。",
          "Selector で書くと条件が Failure でも行動が実行され、条件の意味が消える。" },
        { "abort",
          "割り込みたい条件には abortMode=lowerPriority を付ける。"
          "付けられるのは純粋条件ノード (HasTarget / IsTargetInRange / BlackboardCondition 等) だけ。",
          "これが BT が FSM に対して優位を持つ最大の理由。無いと「巡回中にプレイヤーを"
          "発見しても、現在のウェイポイントに着くまで反応しない」鈍い AI になる。"
          "副作用のあるノードへ付けると、中断チェックのたびに世界が変わり木が非決定的になるため"
          "Validate が拒否する。" },
        { "blackboard",
          "キーは bt.tree の blackboard に載っているものだけを使う。存在しない名前を書いても"
          "保存は通り、Compile 時に解決できず実行時は黙って無視される。",
          "「値を変えても行動が変わらない」としか見えず、綴り違いに最後まで気付けない。" },
        { "blackboard",
          "reserved=true のキーは PerceptionSystem 等が固定添字で書き込む。"
          "改名も削除もしてはならない。",
          "固定添字が前提なので、順序が変わると別のキーへ書かれる。" },
        { "timing",
          "Wait / Cooldown には durationRandom を入れる。",
          "同時にスポーンした敵の待機が完全に同期すると、群れが機械的に見える。" },
        { "structure",
          "未実装の枝は AlwaysSucceed / AlwaysFail で栓をしてから木を組む。",
          "空の Composite は「子が 0 個」として即座に結果が確定し、"
          "組み立て途中の木が意図しない結果を返す。" },
    };

    // 代表的な骨格。ゼロから積むより、この形に沿わせたほうが確実に動く。
    struct Recipe { const char* name; const char* layers; };
    static constexpr Recipe kRecipes[] = {
        { "Guard",
          "Selector[ Sequence(HasTarget[abort=lowerPriority], Selector(Sequence(IsTargetInRange, LookAt, PlayAnimation), MoveTo)), "
          "Sequence(Patrol, Wait) ]" },
        { "Chaser",
          "Selector[ Sequence(IsHealthBelow[abort=lowerPriority], MoveTo(逃走点)), "
          "Sequence(HasLineOfSight[abort=lowerPriority], MoveTo(chase=true)), Wait ]" },
        { "Turret",
          "Selector[ Sequence(IsTargetInRange[abort=lowerPriority], LookAt, Cooldown(PlayAnimation)), LookAt(初期方向) ]" },
    };

    JsonValue rules = JsonValue::MakeArray();
    for (const Rule& item : kRules) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("topic", JsonValue(std::string(item.topic)));
        entry.Set("rule", JsonValue(std::string(item.rule)));
        entry.Set("why", JsonValue(std::string(item.why)));
        rules.Push(std::move(entry));
    }
    JsonValue recipes = JsonValue::MakeArray();
    for (const Recipe& item : kRecipes) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(std::string(item.name)));
        entry.Set("layers", JsonValue(std::string(item.layers)));
        recipes.Push(std::move(entry));
    }
    JsonValue nodeTypes = JsonValue::MakeArray();
    for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
        const auto type = static_cast<fbzz::ai::BTNodeType>(index);
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("type", JsonValue(std::string(fbzz::ai::BTNodeTypeName(type))));
        entry.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(type)));
        // abortMode を付けられるかを型ごとに返す。付けられない型へ付けると
        // Validate が保存を拒否するので、試行錯誤ではなく参照で決められるようにする。
        entry.Set("canAbort", JsonValue(fbzz::ai::BTNodeIsPureCondition(type)
                                        || type == fbzz::ai::BTNodeType::BlackboardCondition));
        nodeTypes.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("rules", std::move(rules));
    result.Set("recipes", std::move(recipes));
    result.Set("nodeTypes", std::move(nodeTypes));
    return Outcome::Ok(std::move(result));
}

// vfx.textureAnalyze — 素材テクスチャの中身を観測し、そこから決まる設定を返す。
//
// WHY: これまで素材は AI にとってパス文字列でしかなく、中身を知る手段が無かった。
//      結果として blendMode も alphaSource も spriteColumns もファイル名からの推測になり、
//      プレビューが変になってから初めて誤りに気づく (しかも画像からは原因が判らない)。
//      画素を数えれば機械的に決まる項目は多く、そこを観測に置き換えるだけで
//      「見た目が破綻したまま反復が収束しない」という失敗の大半が消える。
Outcome DoVFXTextureAnalyze(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path textureFile;
    std::string texturePath;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), textureFile, texturePath)
        || !fs::is_regular_file(textureFile))
        return Outcome::Err("TEXTURE_NOT_FOUND", "projectRoot 配下のテクスチャを指定してください");

    const asset::TextureAnalysis analysis =
        asset::AnalyzeTexture(textureFile.generic_string());
    if (!analysis.success) return Outcome::Err("TEXTURE_INVALID", analysis.message);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(texturePath));
    result.Set("summary", JsonValue(analysis.message));
    result.Set("classification", JsonValue(analysis.classification));
    result.Set("width", JsonValue(analysis.width));
    result.Set("height", JsonValue(analysis.height));
    result.Set("powerOfTwo", JsonValue(analysis.powerOfTwo));

    JsonValue alpha = JsonValue::MakeObject();
    alpha.Set("hasChannel", JsonValue(analysis.hasAlphaChannel));
    // 「チャンネルはあるが全部 1.0」は実質アルファ無し。両方返さないと判断できない。
    alpha.Set("isMeaningful", JsonValue(analysis.alphaIsMeaningful));
    alpha.Set("min", JsonValue(analysis.alphaMin));
    alpha.Set("max", JsonValue(analysis.alphaMax));
    alpha.Set("mean", JsonValue(analysis.alphaMean));
    alpha.Set("transparentRatio", JsonValue(analysis.transparentRatio));
    alpha.Set("opaqueRatio", JsonValue(analysis.opaqueRatio));
    alpha.Set("likelyPremultiplied", JsonValue(analysis.likelyPremultiplied));
    result.Set("alpha", std::move(alpha));

    JsonValue appearance = JsonValue::MakeObject();
    appearance.Set("luminanceMean", JsonValue(analysis.luminanceMean));
    appearance.Set("luminanceMax", JsonValue(analysis.luminanceMax));
    appearance.Set("saturationMean", JsonValue(analysis.saturationMean));
    JsonValue color = JsonValue::MakeArray();
    for (const float channel : analysis.averageColor) color.Push(JsonValue(channel));
    appearance.Set("averageColor", std::move(color));
    // 中心と外周の輝度比。2 を超えると「発光する芯」を持つ素材とみなせる。
    appearance.Set("coreHotspot", JsonValue(analysis.coreHotspot));
    appearance.Set("coverage", JsonValue(analysis.coverage));
    appearance.Set("radialSymmetry", JsonValue(analysis.radialSymmetry));
    appearance.Set("edgeHardness", JsonValue(analysis.edgeHardness));
    appearance.Set("seamlessScore", JsonValue(analysis.seamlessScore));
    result.Set("appearance", std::move(appearance));

    JsonValue flipbook = JsonValue::MakeArray();
    for (const auto& candidate : analysis.flipbookCandidates) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("columns", JsonValue(candidate.columns));
        item.Set("rows", JsonValue(candidate.rows));
        item.Set("frames", JsonValue(candidate.columns * candidate.rows));
        item.Set("seamScore", JsonValue(candidate.seamScore));
        item.Set("uniformity", JsonValue(candidate.uniformity));
        flipbook.Push(std::move(item));
    }
    result.Set("flipbookCandidates", std::move(flipbook));

    // 推奨設定。schemaPath と value をそのまま vfx.node.setField へ渡せる形にする。
    JsonValue recommendations = JsonValue::MakeArray();
    for (const auto& recommendation : analysis.recommendations) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("schemaPath", JsonValue(recommendation.schemaPath));
        item.Set("value", JsonValue(recommendation.value));
        item.Set("reason", JsonValue(recommendation.reason));
        recommendations.Push(std::move(item));
    }
    result.Set("recommendations", std::move(recommendations));
    result.Set("hint", JsonValue(std::string(
        "recommendations は観測から機械的に決まる設定です。value は JSON 表記なので "
        "vfx_node_set_field の value へそのまま渡せます。"
        "flipbookCandidates が空でなければアトラス素材で、先頭が最有力の候補です。"
        "層構成のどこへ置くかは classification (glow=発光する芯 / smoke=背景を隠す body / "
        "spark=火の粉 / flipbook=アニメーション素材) を vfx_guide の recipe と突き合わせてください。")));
    return Outcome::Ok(std::move(result));
}

// シェーダーの変数目録を JSON にする。descriptor が無効なら空配列を返す。
// WHY: ShaderDescriptor は PS バイトコードのリフレクション結果で、
//      「このシェーダーに何を書けるか」の唯一の正本。これを返さない限り、
//      .mat の params も VFX Mesh ノードの animatedParam も名前を推測するしかない。
JsonValue ShaderVarsToJson(const renderer::ShaderDescriptor& descriptor)
{
    const auto typeName = [](renderer::ShaderVarType type) -> const char* {
        switch (type) {
        case renderer::ShaderVarType::Int:  return "int";
        case renderer::ShaderVarType::UInt: return "uint";
        case renderer::ShaderVarType::Bool: return "bool";
        case renderer::ShaderVarType::Float:
        default:                            return "float";
        }
    };
    JsonValue vars = JsonValue::MakeArray();
    for (const auto& var : descriptor.vars) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(var.name));
        // components が「値をいくつ渡すか」を決める。float3 に 1 個渡すと残りが 0 になる。
        item.Set("components", JsonValue(static_cast<int>(var.columns)));
        item.Set("type", JsonValue(std::string(typeName(var.varType))));
        item.Set("sizeBytes", JsonValue(static_cast<int>(var.size)));
        vars.Push(std::move(item));
    }
    return vars;
}

// shader.inspect — シェーダーが公開する変数とテクスチャスロットの目録を返す。
//
// WHY: .mat の params も VFX Mesh ノードの animatedParam も「シェーダー変数名」を要求するが、
//      その名前の一覧を知る手段が今まで無かった。HLSL を読ませるのは現実的でないうえ、
//      実際に効くのはコンパイル済みバイトコードのリフレクション結果であって
//      ソース上の宣言ではない (未使用変数は最適化で消える)。
//      存在しない名前を書いても保存は通り、実行時に黙って無視されるため、
//      「設定したのに動かない」の原因が最後まで判らなかった。
Outcome DoShaderInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.resources == nullptr) return Outcome::Err("NO_RENDERER", "ResourceManager がありません");
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");

    // guid: 参照も含めて AssetManager に解決させる (.mat の shader フィールドは guid 形式)。
    const std::string resolved = asset::AssetManager::ResolveAssetPath(path);
    const auto handle = ctx.resources->LoadShader(resolved.empty() ? path : resolved);
    const renderer::IShader* shader = handle.IsValid() ? ctx.resources->Get(handle) : nullptr;
    if (shader == nullptr)
        return Outcome::Err("SHADER_NOT_FOUND", "シェーダーを読み込めません: " + path);

    const renderer::ShaderDescriptor& descriptor = shader->GetDescriptor();
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("valid", JsonValue(descriptor.IsValid()));
    result.Set("cbufferSize", JsonValue(static_cast<int>(descriptor.cbufferSize)));
    result.Set("vars", ShaderVarsToJson(descriptor));

    JsonValue postProcess = JsonValue::MakeArray();
    for (const auto& var : descriptor.postProcessVars) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(var.name));
        item.Set("components", JsonValue(static_cast<int>(var.columns)));
        postProcess.Push(std::move(item));
    }
    result.Set("postProcessVars", std::move(postProcess));

    JsonValue textures = JsonValue::MakeArray();
    for (const auto& texture : descriptor.textures) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(texture.name));
        item.Set("slot", JsonValue(static_cast<int>(texture.slot)));
        textures.Push(std::move(item));
    }
    result.Set("textures", std::move(textures));
    result.Set("hint", JsonValue(std::string(
        "vars がこのシェーダーへ書ける変数の全てです。ここに無い名前は .mat の params へ書いても "
        "VFX Mesh ノードの animatedParam へ指定しても、実行時に黙って無視されます。"
        "components は渡す値の個数で、float3 の変数へ 1 個だけ渡すと残りは 0 になります。"
        "未使用変数はコンパイル時に消えるため、HLSL に書いてあってもここに出ないことがあります。")));
    return Outcome::Ok(std::move(result));
}

// VFXEditorの表示と同じ正本から、シェーダーコンパイル診断をAIへ返す。
Outcome DoShaderCompileDiagnostics()
{
    const auto diagnostics = renderer::GetShaderCompileDiagnostics();
    JsonValue result = JsonValue::MakeObject();
    JsonValue items = JsonValue::MakeArray();
    int errorCount = 0;
    int warningCount = 0;
    for (const auto& diagnostic : diagnostics) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("sequence", JsonValue(static_cast<double>(diagnostic.sequence)));
        item.Set("severity", JsonValue(diagnostic.isError ? "error" : "warning"));
        item.Set("path", JsonValue(diagnostic.path));
        item.Set("entryPoint", JsonValue(diagnostic.entryPoint));
        item.Set("target", JsonValue(diagnostic.target));
        item.Set("message", JsonValue(diagnostic.message));
        items.Push(std::move(item));
        if (diagnostic.isError) ++errorCount;
        else ++warningCount;
    }
    result.Set("errorCount", JsonValue(errorCount));
    result.Set("warningCount", JsonValue(warningCount));
    result.Set("diagnostics", std::move(items));
    return Outcome::Ok(std::move(result));
}

// vfx.materialAnalyze — .mat とその albedo テクスチャを併せて見る。
//
// WHY: ParticleEmitter に materialPath があると、ParticlePass は .mat の blend_mode で
//      emitter.blendMode を上書きする。つまり .mat を割り当てた時点で、Inspector や AI が
//      設定した blendMode は実行時に使われない。テクスチャ単体の解析だけを見ていると
//      「推奨どおり Additive にしたのに Alpha で描かれる」という噛み合わなさが起きる。
//      .mat まで読んで初めて「この素材をこの設定で描くと何が起きるか」が言える。
Outcome DoVFXMaterialAnalyze(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path materialFile;
    std::string materialPath;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), materialFile, materialPath)
        || !fs::is_regular_file(materialFile))
        return Outcome::Err("MATERIAL_NOT_FOUND", "projectRoot 配下の .mat を指定してください");

    const asset::MaterialAnalysis analysis =
        asset::AnalyzeMaterial(materialFile.generic_string());
    if (!analysis.success) return Outcome::Err("MATERIAL_INVALID", analysis.message);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(materialPath));
    result.Set("summary", JsonValue(analysis.message));
    result.Set("shaderPath", JsonValue(analysis.shaderPath));
    result.Set("blendMode", JsonValue(analysis.blendMode));
    result.Set("renderPath", JsonValue(analysis.renderPath));
    result.Set("doubleSided", JsonValue(analysis.doubleSided));
    result.Set("depthWrite", JsonValue(analysis.depthWrite));
    result.Set("renderQueue", JsonValue(analysis.renderQueue));
    result.Set("albedoTexturePath", JsonValue(analysis.albedoTexturePath));
    result.Set("hasAlbedoTexture", JsonValue(analysis.hasAlbedoTexture));
    // テクスチャ側の分類だけ再掲する。詳細が要るなら vfx.textureAnalyze を直接呼べばよい。
    if (analysis.hasAlbedoTexture && analysis.albedoAnalysis.success) {
        JsonValue albedo = JsonValue::MakeObject();
        albedo.Set("classification", JsonValue(analysis.albedoAnalysis.classification));
        albedo.Set("summary", JsonValue(analysis.albedoAnalysis.message));
        albedo.Set("alphaIsMeaningful", JsonValue(analysis.albedoAnalysis.alphaIsMeaningful));
        albedo.Set("likelyPremultiplied", JsonValue(analysis.albedoAnalysis.likelyPremultiplied));
        albedo.Set("coreHotspot", JsonValue(analysis.albedoAnalysis.coreHotspot));
        albedo.Set("coverage", JsonValue(analysis.albedoAnalysis.coverage));
        result.Set("albedo", std::move(albedo));
    }
    result.Set("blendModeConflictsWithTexture", JsonValue(analysis.blendModeConflictsWithTexture));

    JsonValue findings = JsonValue::MakeArray();
    for (const auto& finding : analysis.findings) findings.Push(JsonValue(finding));

    // シェーダー変数との照合。.mat の params にシェーダーへ存在しない名前があると、
    // 保存はされるが実行時に無視される (Material が名前で束縛するため)。
    // WHY: これは「値を変えても絵が変わらない」という形でしか現れず、
    //      .mat を眺めても綴り違いに気付けない。シェーダー側の目録と突き合わせて初めて判る。
    if (ctx.resources != nullptr && !analysis.shaderPath.empty()) {
        const std::string resolvedShader = asset::AssetManager::ResolveAssetPath(analysis.shaderPath);
        const auto handle = ctx.resources->LoadShader(
            resolvedShader.empty() ? analysis.shaderPath : resolvedShader);
        if (const renderer::IShader* shader = handle.IsValid() ? ctx.resources->Get(handle) : nullptr) {
            const renderer::ShaderDescriptor& descriptor = shader->GetDescriptor();
            result.Set("shaderVars", ShaderVarsToJson(descriptor));
            asset::MaterialAsset material;
            if (asset::LoadMaterialAssetFromFile(materialFile.generic_string(), material)) {
                for (const auto& [name, values] : material.params) {
                    const renderer::ShaderVarDesc* var = descriptor.FindVar(name);
                    if (var == nullptr) {
                        findings.Push(JsonValue("params の \"" + name
                            + "\" はこのシェーダーに存在しません (実行時に無視されます)。"
                              "shaderVars に載っている名前を使ってください"));
                        continue;
                    }
                    // 要素数が足りないと残りが 0 で埋まる。色が黒くなる典型的な原因。
                    if (values.size() < static_cast<std::size_t>(var->columns))
                        findings.Push(JsonValue("params の \"" + name + "\" は "
                            + std::to_string(var->columns) + " 要素必要ですが "
                            + std::to_string(values.size()) + " 個しかありません (残りは 0 になります)"));
                }
            }
        }
    }
    result.Set("findings", std::move(findings));

    JsonValue recommendations = JsonValue::MakeArray();
    for (const auto& recommendation : analysis.recommendations) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("schemaPath", JsonValue(recommendation.schemaPath));
        item.Set("value", JsonValue(recommendation.value));
        item.Set("reason", JsonValue(recommendation.reason));
        recommendations.Push(std::move(item));
    }
    result.Set("recommendations", std::move(recommendations));
    result.Set("hint", JsonValue(std::string(
        "findings は .mat 自体を直すべき問題です (blend_mode / render_path / albedo 未設定)。"
        "recommendations は .mat では表現できず Emitter 側にしか無い設定 (alphaSource / sprite / sortMode) で、"
        "vfx_node_set_field へそのまま渡せます。"
        "materialPath を設定した Emitter では blendMode が .mat から上書きされるため、"
        "ブレンドを変えたい場合は Emitter ではなく .mat を編集してください。")));
    return Outcome::Ok(std::move(result));
}

// vfx.assetSurvey — プロジェクトの素材を分類し、狙う表現に対して何が足りないかを返す。
//
// WHY: vfx.guide の recipe は「煙 / 外炎 / 芯 / 火の粉 / 陽炎」のような層構成を示すが、
//      その層を作れる素材が手元にあるかは別問題。AI は素材を 1 枚ずつ解析して
//      初めて種類が判るため、何を持っているかを知らないまま recipe に沿おうとして
//      「無い素材を前提にしたグラフ」を組んでしまう。先に棚卸しを返す。
// 解析済みテクスチャの分類キャッシュ。キーはファイルパス、有効性はサイズ + 最終更新時刻で判定する。
//
// WHY: 解析は 1 枚あたり数十 ms かかるうえ、棚卸しは同じプロジェクトで何度も走る。
//      素材は編集中もほぼ変わらないのに、毎回 120 枚を解析し直していた。
//      内容ハッシュではなくサイズ + mtime で判定するのは、判定自体が解析より
//      桁違いに安く、テクスチャを書き換えれば必ずどちらかが動くため。
struct TextureSurveyCacheEntry {
    std::uintmax_t size = 0;
    std::int64_t   writeTime = 0;
    std::string    classification;
    std::string    summary;
};

std::unordered_map<std::string, TextureSurveyCacheEntry>& GetTextureSurveyCache()
{
    // プロセス生存中だけ保持する。Editor を再起動すれば当然作り直しになる。
    static std::unordered_map<std::string, TextureSurveyCacheEntry> cache;
    return cache;
}

Outcome DoVFXAssetSurvey(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");

    // 走査範囲。既定は Assets/Textures 配下だが、指定があればそこを見る。
    const std::string requested = StringField(payload, "directory");
    fs::path scanRoot = fs::path(ctx.projectRoot) / (requested.empty() ? "Assets" : requested);
    std::error_code ec;
    if (!fs::is_directory(scanRoot, ec))
        return Outcome::Err("DIR_NOT_FOUND", "走査対象のディレクトリがありません: " + scanRoot.generic_string());
    // projectRoot の外へ出る指定を弾く。
    const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    const fs::path target = fs::weakly_canonical(scanRoot, ec);
    if (ec || target.string().rfind(root.string(), 0) != 0)
        return Outcome::Err("BAD_ARG", "projectRoot の外は走査できません");

    // 解析は 1 枚あたり数十 ms かかる。棚卸しは枚数が多いので、統計用の縮小を強めにし、
    // 上限も設ける (全走査で分単位かかると、AI が最初の 1 手で詰まる)。
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr ? limitValue->AsInt() : 120, 1, 400);
    // detail="summary" (既定) は 1 枚あたり path だけを返す。
    // WHY: 棚卸しで知りたいのは「どの役割の素材が何枚あるか」であって、
    //      120 枚ぶんの解析文まで読む必要は無い。個別の中身は vfx.textureAnalyze で見る。
    const std::string detail = LowerAscii(StringField(payload, "detail"));
    if (!detail.empty() && detail != "summary" && detail != "full")
        return Outcome::Err("BAD_ARG", "detail は summary / full のいずれかです");
    const bool full = detail == "full";
    // 素材を差し替えたのに分類が古いままになるのを避けるための強制再解析。
    const JsonValue* refreshValue = payload.Find("refresh");
    if (refreshValue != nullptr && refreshValue->AsBool()) GetTextureSurveyCache().clear();

    struct Entry {
        std::string path;
        std::string classification;
        std::string summary;
    };
    std::vector<Entry> entries;
    auto& cache = GetTextureSurveyCache();
    int analyzedCount = 0;
    int cachedCount = 0;
    bool truncated = false;
    for (const auto& file : fs::recursive_directory_iterator(
             scanRoot, fs::directory_options::skip_permission_denied, ec)) {
        if (!file.is_regular_file(ec)) { ec.clear(); continue; }
        const std::string extension = LowerAscii(file.path().extension().string());
        if (extension != ".png" && extension != ".tga" && extension != ".dds"
            && extension != ".jpg" && extension != ".jpeg") continue;
        if (static_cast<int>(entries.size()) >= limit) { truncated = true; break; }

        // サイズと更新時刻が読めなければキャッシュは使わない (毎回解析する)。
        // 読めない状態を「変化なし」と同一視すると、古い分類を返し続けることになる。
        const std::string absolute = file.path().generic_string();
        const std::uintmax_t size = fs::file_size(file.path(), ec);
        bool statOk = !ec;
        ec.clear();
        const std::int64_t writeTime = statOk
            ? static_cast<std::int64_t>(fs::last_write_time(file.path(), ec).time_since_epoch().count())
            : 0;
        if (ec) statOk = false;
        ec.clear();

        const auto cached = cache.find(absolute);
        if (statOk && cached != cache.end() && cached->second.size == size
            && cached->second.writeTime == writeTime) {
            ++cachedCount;
            entries.push_back({ fs::relative(file.path(), root, ec).generic_string(),
                                cached->second.classification, cached->second.summary });
            ec.clear();
            continue;
        }

        const asset::TextureAnalysis analysis = asset::AnalyzeTexture(absolute, 128);
        ++analyzedCount;
        if (!analysis.success) continue;
        if (statOk) cache[absolute] = { size, writeTime, analysis.classification, analysis.message };
        entries.push_back({
            fs::relative(file.path(), root, ec).generic_string(),
            analysis.classification,
            analysis.message,
        });
        ec.clear();
    }

    // 分類ごとにまとめる。AI は「glow が 3 枚ある」と知りたいのであって、
    // 全ファイルの詳細を一度に読みたいわけではない。
    JsonValue byClassification = JsonValue::MakeObject();
    const auto collect = [&](const char* classification) {
        JsonValue list = JsonValue::MakeArray();
        for (const auto& entry : entries) {
            if (entry.classification != classification) continue;
            if (!full) { list.Push(JsonValue(entry.path)); continue; }
            JsonValue item = JsonValue::MakeObject();
            item.Set("path", JsonValue(entry.path));
            item.Set("summary", JsonValue(entry.summary));
            list.Push(std::move(item));
        }
        const int count = static_cast<int>(list.AsArray().size());
        byClassification.Set(classification, std::move(list));
        return count;
    };
    const int glowCount = collect("glow");
    const int smokeCount = collect("smoke");
    const int sparkCount = collect("spark");
    const int flipbookCount = collect("flipbook");
    collect("mask");
    collect("unknown");

    // 層構成に必要な素材が揃っているか。役割の表は VFXRecipeLibrary が唯一の正本。
    // WHY: 判定基準を guide / recipe と別に持つと「guide は煙を要求するが survey は
    //      要求しない」のような食い違いが生まれる。語彙も代替案も 1 か所から読む。
    const auto availableFor = [&](const char* classification) {
        const std::string name = classification;
        if (name == "glow") return glowCount;
        if (name == "smoke") return smokeCount;
        if (name == "spark") return sparkCount;
        if (name == "flipbook") return flipbookCount;
        return 0;
    };
    JsonValue coverage = JsonValue::MakeArray();
    JsonValue missing = JsonValue::MakeArray();
    for (const editor::VFXAssetRoleInfo& info : editor::GetVFXAssetRoles()) {
        const int available = availableFor(info.classification);
        JsonValue item = JsonValue::MakeObject();
        item.Set("role", JsonValue(std::string(info.role)));
        item.Set("classification", JsonValue(std::string(info.classification)));
        item.Set("available", JsonValue(available));
        item.Set("purpose", JsonValue(std::string(info.purpose)));
        // 無い場合の代替案。「無い」とだけ返されても次の一手が決まらない。
        item.Set("fallback", JsonValue(std::string(info.fallback)));
        if (available == 0) missing.Push(JsonValue(std::string(info.role)));
        coverage.Push(std::move(item));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("directory", JsonValue(scanRoot.generic_string()));
    result.Set("detail", JsonValue(full ? "full" : "summary"));
    result.Set("analyzed", JsonValue(static_cast<int>(entries.size())));
    // 今回実際に画素を読んだ枚数と、キャッシュから復元した枚数。
    // 2 回目以降が速い理由 (と、refresh=true が要る場面) をここで見せる。
    result.Set("freshlyAnalyzed", JsonValue(analyzedCount));
    result.Set("fromCache", JsonValue(cachedCount));
    result.Set("truncated", JsonValue(truncated));
    result.Set("byClassification", std::move(byClassification));
    result.Set("coverage", std::move(coverage));
    result.Set("missingRoles", std::move(missing));
    result.Set("hint", JsonValue(std::string(
        "missingRoles にある役割は、手持ちの素材だけでは作れません。"
        "core が無ければ加算で光る層を、body が無ければ背景を隠す層を作れないため、"
        "その recipe をそのまま再現しようとしても破綻します。"
        "代替として: core が無い場合は body 素材を Additive で小さく使う、"
        "body が無い場合は glow 素材のアルファを寝かせて使う、で近似できます。"
        "truncated=true なら limit を上げるか directory を絞ってください。"
        "detail=\"summary\" では 1 枚あたり path だけを返します。"
        "個別の素材の中身は vfx.textureAnalyze で見てください。"
        "分類はサイズと最終更新時刻でキャッシュされます。"
        "外部ツールで素材を差し替えたのに分類が変わらない場合だけ refresh=true を渡してください。")));
    return Outcome::Ok(std::move(result));
}

// Preview World が無いときの応答。
//
// WHY: 以前は "VFX Preview Worldがありません" の 1 行しか返らず、
//      未初期化なのか、独立プロセスが落ちているのか、GPU デバイスを取れていないのかを
//      切り分けられなかった。しかもコンソールログにも残らないため、
//      Editor 側を見に行っても何も手掛かりが無い状態だった。
//      原因の切り分けに要る事実 (誰が World を所有しているか / 次に何を呼べばいいか) を
//      メッセージへ入れ、同時にログにも残して console.logs から追えるようにする。
Outcome MakeNoPreviewWorldOutcome(const char* requestName)
{
    FBZZ_LOG_WARN("AI Bus: %s は VFX Preview World を必要としますが、"
                  "このプロセスは Preview World を所有していません", requestName);
    return Outcome::Err("NO_PREVIEW_WORLD",
        std::string(requestName) + " は VFX Preview World を必要としますが、"
        "このプロセスは Preview World を所有していません。"
        "Preview World は独立プロセス FBZZVFXEditor.exe が所有します "
        "(Editor 本体は VFX の描画リソースを持ちません)。"
        "vfx.previewEnsure (vfx_preview_ensure) を呼ぶと起動と初期化完了までを待って"
        "状態を返します。それでも失敗する場合は console.logs で "
        "\"VFXEditorLauncher\" を含む行を確認してください。");
}

// vfx.previewEnsure — Preview World を起動し、プレビュー系を呼べる状態か確認する。
//
// WHY: vfx.preview / vfx.previewMetrics / vfx.previewCurve / vfx.runtime は
//      すべて Preview World の存在が前提だが、それを「作る」「在るか確かめる」手段が
//      公開されていなかった。結果として vfx.guide が勧める
//      lint → previewMetrics → previewCurve の後半 2 つが、環境によっては
//      原理的に実行できないのに、その理由も分からないという状態になっていた。
//      要求はメイン Editor 側で独立プロセスへ転送されるため、
//      この関数へ到達した時点で World は既に在る (= 起動は転送側の責務)。
Outcome DoVFXPreviewEnsure(editor::EditorContext& ctx)
{
    if (ctx.vfxPreviewScene == nullptr) return MakeNoPreviewWorldOutcome("vfx.previewEnsure");

    JsonValue result = JsonValue::MakeObject();
    result.Set("previewWorld", JsonValue(true));
    result.Set("owner", JsonValue("FBZZVFXEditor"));
    // 描画まで到達できるか。renderer が無ければ preview は組めても画が返らない。
    const bool rendererReady = ctx.renderer != nullptr;
    result.Set("rendererReady", JsonValue(rendererReady));

    GameObject* root = ctx.vfxPreviewScene->Find("__VFX_AI_PREVIEW_ROOT");
    auto* component = root != nullptr ? root->GetComponent<scene::VFXGraphComponent>() : nullptr;
    JsonValue prepared = JsonValue::MakeObject();
    prepared.Set("hasGraph", JsonValue(component != nullptr));
    if (component != nullptr) {
        prepared.Set("path", JsonValue(component->graphPath));
        prepared.Set("initialized", JsonValue(component->initialized));
        prepared.Set("playTime", JsonValue(component->playTime));
        prepared.Set("duration", JsonValue(component->graphDuration));
        prepared.Set("nodeCount", JsonValue(static_cast<int>(component->runtimeNodes.size())));
    }
    result.Set("prepared", std::move(prepared));
    result.Set("readyForPreview", JsonValue(rendererReady));
    result.Set("hint", JsonValue(std::string(
        "readyForPreview=true なら vfx.preview / vfx.previewMetrics / vfx.runtime を呼べます。"
        "prepared.hasGraph=false は「まだ 1 度も vfx.preview を実行していない」だけで、"
        "異常ではありません (vfx.runtime はその状態では NO_VFX_RUNTIME を返します)。"
        "rendererReady=false のときは描画デバイスを取得できていないため、"
        "画像を返す系は失敗します。console.logs でシェーダーコンパイルの失敗を確認してください。")));
    return Outcome::Ok(std::move(result));
}

// vfx.runtime — 直前の vfx.preview が組んだ実行状態を、その時刻のまま読み出す。
//
// WHY: 「このノードが画に出てこない」は AI の最頻出の詰まり方だが、原因は 3 通りあって
//      画像からは区別できない:
//        (1) 起動していない       — Entry から未接続 / 無効化されている
//        (2) 起動待ちのまま       — OnCollision / OnDeath 待ちで、その事象が起きていない
//        (3) 起動しているが見えない — スケールが 0、色が透明、カメラ外、他の粒子に隠れている
//      vfx.lint は静的解析なので (1) しか見えない。(2) はスケジュール表に位置を持たないため
//      時刻からも推測できず、ここを返さない限り AI は画像を睨んで推測を続けることになる。
//      実行状態を返せば (1)(2) は即断でき、残った (3) だけが画像で見るべき問題になる。
//
// NOTE: 参照するのは AI capture 用 Preview World であって、担当者が操作している
//       VFX Editor の World ではない。表示設定や手動スクラブが混ざると再現しなくなるため。
Outcome DoVFXRuntime(editor::EditorContext& ctx)
{
    if (ctx.vfxPreviewScene == nullptr) return MakeNoPreviewWorldOutcome("vfx.runtime");
    GameObject* root = ctx.vfxPreviewScene->Find("__VFX_AI_PREVIEW_ROOT");
    auto* component = root != nullptr ? root->GetComponent<scene::VFXGraphComponent>() : nullptr;
    if (component == nullptr)
        return Outcome::Err("NO_VFX_RUNTIME",
                            "先に vfx.preview を実行してください (実行状態はその結果として作られます)");
    if (!component->initialized)
        return Outcome::Err("VFX_NOT_READY",
                            "グラフがまだ構築されていません。vfx.preview の readyAfterFrame を待ってください");

    // authoring 名を引くために元グラフも読む (runtimeNodes は id しか持たない)。
    asset::VFXGraphAsset graph;
    std::string loadError;
    const bool hasGraph = asset::LoadVFXGraphAsset(component->graphPath, graph, &loadError);

    JsonValue nodes = JsonValue::MakeArray();
    int activeCount = 0;
    int waitingCount = 0;
    int gpuFallbackCount = 0;
    int totalParticles = 0;
    int totalVisibleParticles = 0;
    for (const auto& state : component->runtimeNodes) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(state.nodeId));
        if (hasGraph) {
            const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                [&state](const asset::VFXGraphNode& candidate) { return candidate.id == state.nodeId; });
            if (node != graph.nodes.end()) {
                item.Set("name", JsonValue(node->name));
                item.Set("type", JsonValue(asset::VFXNodeTypeName(node->type)));
                item.Set("enabled", JsonValue(node->enabled));
            }
        }
        item.Set("active", JsonValue(state.active));
        if (state.active) ++activeCount;
        // startTime が有限でない = イベント待ち。スケジュール上の位置を持たないので、
        // scheduledStartTime (予定) と並べて「待っている」ことを明示する。
        const bool waiting = !(state.startTime < (std::numeric_limits<float>::max)());
        item.Set("waitingForEvent", JsonValue(waiting));
        if (waiting) ++waitingCount;
        else {
            item.Set("startTime", JsonValue(state.startTime));
            item.Set("endTime", JsonValue(state.endTime));
        }
        item.Set("scheduledStartTime", JsonValue(state.scheduledStartTime));
        item.Set("scheduledEndTime", JsonValue(state.scheduledEndTime));
        // 実体を持たないノード (Entry / Delay / 無効) は GameObject を作らない。
        item.Set("hasInstance", JsonValue(state.entity.IsValid()));

        // 実際に GPU で回っているか。simulationMode = Gpu にしても条件を 1 つ外すと
        // 黙って CPU へ落ちるため、要求ではなく「結果」を返さないと事故に気づけない。
        GameObject* instance = state.entity.IsValid()
            ? ctx.vfxPreviewScene->GetGameObject(state.entity) : nullptr;
        if (auto* emitter = instance != nullptr ? instance->GetComponent<scene::ParticleEmitter>() : nullptr) {
            const auto reason = scene::GetParticleGpuFallbackReason(*emitter);
            const bool requestedGpu = emitter->simulationMode == scene::ParticleSimulationMode::Gpu;
            const bool effectiveGpu = reason == scene::ParticleGpuFallbackReason::None;
            JsonValue simulation = JsonValue::MakeObject();
            simulation.Set("requested", JsonValue(requestedGpu ? "Gpu" : "Cpu"));
            simulation.Set("effective", JsonValue(effectiveGpu ? "Gpu" : "Cpu"));
            if (requestedGpu && !effectiveGpu) {
                ++gpuFallbackCount;
                simulation.Set("fallbackField",
                               JsonValue(std::string(scene::ParticleGpuFallbackFieldName(reason))));
                simulation.Set("fallbackReason",
                               JsonValue(std::string(scene::ParticleGpuFallbackDescription(reason))));
            }
            item.Set("simulation", std::move(simulation));
            const int alive = effectiveGpu
                ? emitter->visibleParticleCount : static_cast<int>(emitter->particles.size());
            item.Set("particleCount", JsonValue(alive));
            item.Set("visibleParticleCount", JsonValue(emitter->visibleParticleCount));
            item.Set("culled", JsonValue(emitter->isCulledThisFrame));
            totalParticles += alive;
            totalVisibleParticles += emitter->visibleParticleCount;
        }
        nodes.Push(std::move(item));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(component->graphPath));
    result.Set("playTime", JsonValue(component->playTime));
    result.Set("duration", JsonValue(component->graphDuration));
    result.Set("activeCount", JsonValue(activeCount));
    result.Set("waitingForEventCount", JsonValue(waitingCount));
    result.Set("gpuFallbackCount", JsonValue(gpuFallbackCount));
    result.Set("nodes", std::move(nodes));

    // 実測コスト。budget は粒子数でしか測れないが、実際のボトルネックはほぼ fill rate で、
    // 「粒子は budget 内なのに重い」はここを見ない限り検知できない。
    // 計測は RenderSystem が全パスへ自動で仕込んでいるので、ここは結果を読むだけ。
    JsonValue cost = JsonValue::MakeObject();
    cost.Set("totalParticles", JsonValue(totalParticles));
    cost.Set("visibleParticles", JsonValue(totalVisibleParticles));
    bool measured = false;
    if (ctx.renderer != nullptr) {
        for (const auto& profile : ctx.renderer->GpuProfGetResults()) {
            if (profile.name != "Particle") continue;
            cost.Set("particlePassGpuMs", JsonValue(profile.gpuMs));
            measured = true;
            break;
        }
    }
    cost.Set("measured", JsonValue(measured));
    // 重なり枚数。vfx.preview を view="overdraw" で実行したときだけ計測される。
    // WHY: 「粒子は budget 内なのに particlePassGpuMs が伸びている」ときの原因はほぼこれで、
    //      枚数が判れば「粒を大きくして枚数を減らす」という正しい手を選べる。
    if (const auto& overdraw = scene::GetLastParticleOverdrawStats(); overdraw.valid) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("measuredAtFrame", JsonValue(static_cast<std::int64_t>(overdraw.frame)));
        item.Set("coveredRatio", JsonValue(overdraw.coveredRatio));
        item.Set("meanLayers", JsonValue(overdraw.meanLayers));
        item.Set("maxLayers", JsonValue(overdraw.maxLayers));
        item.Set("heavyRatio", JsonValue(overdraw.heavyRatio));
        item.Set("overdrawFactor", JsonValue(overdraw.overdrawFactor));
        cost.Set("overdraw", std::move(item));
    } else {
        cost.Set("overdrawNote", JsonValue(std::string(
            "重なり枚数はまだ計測していません。vfx.preview を view=\"overdraw\" で実行すると"
            "その回だけ計測されます (読み戻しを伴うため常時計測はしません)")));
    }
    if (!measured) {
        cost.Set("note", JsonValue(std::string(
            "GPU 計測結果がまだ確定していません (数フレーム遅れて出ます)。"
            "vfx.preview の直後ではなく、少し待ってから再度呼んでください")));
    }
    result.Set("cost", std::move(cost));

    result.Set("hint", JsonValue(std::string(
        "active=false かつ waitingForEvent=true のノードは OnCollision / OnDeath 待ちです。"
        "source 側の Particle が衝突・死亡していない限り永久に起動しません "
        "(collisionMode の設定、または OnComplete への trigger 変更を検討してください)。"
        "active=true なのに画に出ない場合は、サイズ・色・カメラ画角の側を疑ってください。"
        "simulation.effective が Cpu なのに requested が Gpu のノードは黙って縮退しています。"
        "fallbackField が原因の設定名で、そこを直さない限り粒子数を増やしても GPU には載りません。"
        "cost.particlePassGpuMs は Particle パス全体の実測時間です。"
        "60fps の 1 フレームは 16.6ms なので、エフェクト単体で 1ms を超えたら重い部類。"
        "粒子数が budget 内なのにここが伸びていれば原因は fill rate (重なり) で、"
        "粒子数を減らすより「粒を大きくして枚数を減らす」ほうが効きます。")));
    return Outcome::Ok(std::move(result));
}

Outcome DoVFXPreview(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.vfxPreviewScene == nullptr) return MakeNoPreviewWorldOutcome("vfx.preview");
    const std::string path = StringField(payload, "path");
    const JsonValue* timeValue = payload.Find("time");
    if (path.empty() || timeValue == nullptr || !timeValue->IsNumber())
        return Outcome::Err("BAD_ARG", "pathとtimeが必要です");

    // 診断ビュー。既定 (normal) は従来どおりクリーンな評価画。
    // WHY: 通常の絵だけでは「なぜこう見えるのか」が分からない局面がある
    //      (白飛びの原因が粒子数か emissive か / 炎が上がらない原因が力場の半径か強さか)。
    //      AI がそこで手詰まりにならないよう、明示的に要求したときだけ診断表示を許す。
    //      評価用の既定を変えないことが重要で、これを常時 on にすると
    //      「決定論プレビューで同一時刻が同一画になる」前提そのものが崩れる。
    const std::string view = LowerAscii(StringField(payload, "view"));
    if (!view.empty() && view != "normal" && view != "overdraw" && view != "gizmos")
        return Outcome::Err("BAD_ARG", "view は normal / overdraw / gizmos のいずれかです");
    ctx.vfxAiPreviewOverdraw = view == "overdraw";
    ctx.vfxAiPreviewGizmos   = view == "gizmos";

    // カメラ。省略時は従来どおり操作用プレビューの視点をそのまま使う。
    // WHY: 常に同じ 1 方向・1 距離からしか見られないと、ゲーム内距離での可読性も
    //      ビルボードのシルエットも LOD の切り替わりも一度も検証できない。
    //      preset は「代表 3 視点」を短く指定するための糖衣で、明示指定があればそちらが勝つ。
    if (const JsonValue* cameraValue = payload.Find("camera");
        cameraValue != nullptr && cameraValue->IsObject()) {
        editor::EditorContext::VFXAiPreviewCamera camera;
        camera.valid = true;
        const std::string preset = LowerAscii(StringField(*cameraValue, "preset"));
        if (!preset.empty()) {
            if      (preset == "front") { camera.yaw = 0.0f;   camera.pitch = 5.0f;  }
            else if (preset == "angle") { camera.yaw = 35.0f;  camera.pitch = 25.0f; }
            else if (preset == "side")  { camera.yaw = 90.0f;  camera.pitch = 0.0f;  }
            else if (preset == "top")   { camera.yaw = 0.0f;   camera.pitch = 80.0f; }
            else return Outcome::Err("BAD_ARG", "camera.preset は front / angle / side / top のいずれかです");
        }
        const auto readNumber = [&cameraValue](const char* key, float& target) {
            if (const JsonValue* value = cameraValue->Find(key); value != nullptr && value->IsNumber())
                target = static_cast<float>(value->AsNumber());
        };
        readNumber("distance", camera.distance);
        readNumber("yaw", camera.yaw);
        readNumber("pitch", camera.pitch);
        readNumber("fovY", camera.fovY);
        if (const JsonValue* target = cameraValue->Find("target");
            target != nullptr && target->IsArray() && target->AsArray().size() == 3) {
            const auto& components = target->AsArray();
            if (!components[0].IsNumber() || !components[1].IsNumber() || !components[2].IsNumber())
                return Outcome::Err("BAD_ARG", "camera.target は数値 3 要素の配列です");
            camera.targetX = static_cast<float>(components[0].AsNumber());
            camera.targetY = static_cast<float>(components[1].AsNumber());
            camera.targetZ = static_cast<float>(components[2].AsNumber());
        }
        // 極端な値は行列を壊す (distance=0 で LookAt が不定、pitch=±90 で up と一致)。
        camera.distance = std::clamp(camera.distance, 0.05f, 500.0f);
        camera.pitch    = std::clamp(camera.pitch, -89.0f, 89.0f);
        camera.fovY     = std::clamp(camera.fovY, 5.0f, 120.0f);
        ctx.vfxAiPreviewCamera = camera;
    } else {
        ctx.vfxAiPreviewCamera = {};
    }

    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::LoadVFXGraphAsset(path, graph, &error) || !asset::ValidateVFXGraphAsset(graph, &error))
        return Outcome::Err("VFX_INVALID", error);

    std::vector<float> starts;
    float duration = 0.0f;
    if (!asset::BuildVFXGraphSchedule(graph, starts, duration, &error)) return Outcome::Err("VFX_INVALID", error);
    const float requestedTime = static_cast<float>(timeValue->AsNumber());
    const float previewTime = std::clamp(requestedTime, 0.0f, (std::max)(duration, 0.0f));

    // Preview Worldは保存対象外なので、静止画評価ごとに完全初期化してrandomSeedから再シミュレートする。
    ctx.vfxPreviewScene->Clear();
    GameObject& root = ctx.vfxPreviewScene->CreateGameObject("__VFX_AI_PREVIEW_ROOT");
    scene::VFXGraphComponent component;
    component.graphPath = path;
    component.playOnAwake = false;
    component.playing = false;
    component.editorScrubTime = previewTime;
    component.editorPreviewFrame = Time::frameCount + 8;
    root.AddComponent<scene::VFXGraphComponent>(std::move(component));
    ctx.vfxAiPreviewUntilFrame = Time::frameCount + 8;

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("time", JsonValue(previewTime));
    result.Set("duration", JsonValue(duration));
    result.Set("preparedAtFrame", JsonValue(static_cast<std::int64_t>(Time::frameCount)));
    result.Set("readyAfterFrame", JsonValue(static_cast<std::int64_t>(Time::frameCount + 2)));
    // 実際に使う視点を返す。preset とクランプ後の値が判らないと、AI は距離を振ったときに
    // 「指定が効いたのか丸められたのか」を区別できない。
    JsonValue camera = JsonValue::MakeObject();
    camera.Set("overridden", JsonValue(ctx.vfxAiPreviewCamera.valid));
    if (ctx.vfxAiPreviewCamera.valid) {
        camera.Set("distance", JsonValue(ctx.vfxAiPreviewCamera.distance));
        camera.Set("yaw", JsonValue(ctx.vfxAiPreviewCamera.yaw));
        camera.Set("pitch", JsonValue(ctx.vfxAiPreviewCamera.pitch));
        camera.Set("fovY", JsonValue(ctx.vfxAiPreviewCamera.fovY));
        JsonValue target = JsonValue::MakeArray();
        target.Push(JsonValue(ctx.vfxAiPreviewCamera.targetX));
        target.Push(JsonValue(ctx.vfxAiPreviewCamera.targetY));
        target.Push(JsonValue(ctx.vfxAiPreviewCamera.targetZ));
        camera.Set("target", std::move(target));
    }
    result.Set("camera", std::move(camera));
    return Outcome::Ok(std::move(result));
}

// vfx.previewMetrics — 直前に描いたプレビュー画を「絵」ではなく「数値」として読む。
//
// WHY: vfx.preview は PNG しか返さないため、良し悪しの判断が全て視覚に委ねられていた。
//      同じ画から毎回違う結論が出るので直す量が決められず、反復が振動する。
//      機械的に判る破綻 (白飛び・覆いすぎ・動いていない・画角ずれ) をここで先に潰し、
//      残った「炎に見えるか」だけを画像と vfx_guide の規約へ委ねる、という切り分けにする。
//      これは vfx.textureAnalyze が blendMode の推測を消したのと同じ構図。
Outcome DoVFXPreviewMetrics(editor::EditorContext& ctx,
                            renderer::ResourceHandle<renderer::RenderTargetTag> rt,
                            const std::vector<float>* previousLuminance,
                            std::vector<float>& outLuminance)
{
    if (ctx.vfxPreviewScene == nullptr) return MakeNoPreviewWorldOutcome("vfx.previewMetrics");
    if (ctx.renderer == nullptr || ctx.resources == nullptr) {
        FBZZ_LOG_WARN("AI Bus: vfx.previewMetrics — レンダラー/ResourceManager が未初期化です");
        return Outcome::Err("NO_RENDERER",
                            "レンダラーが未初期化です。VFX Preview を所有するプロセスが"
                            "描画デバイスを取得できていません "
                            "(console.logs でシェーダーコンパイルの失敗を確認してください)");
    }
    if (!rt.IsValid()) {
        FBZZ_LOG_WARN("AI Bus: vfx.previewMetrics — VFX Preview RT が未生成です");
        return Outcome::Err("NO_VIEWPORT",
                            "VFX Preview RT が未生成です。先に vfx.preview を 1 度実行してください "
                            "(RT は最初の vfx.preview で要求サイズぶん生成されます)");
    }

    std::vector<float> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!ctx.renderer->CaptureRenderTargetToLinearRGBA(rt, *ctx.resources, rgba, width, height)
        || width == 0 || height == 0) {
        FBZZ_LOG_WARN("AI Bus: vfx.previewMetrics — Preview RT の読み戻しに失敗しました "
                      "(width=%u height=%u)", width, height);
        return Outcome::Err("CAPTURE_FAILED",
                            "プレビューRTを数値として読み戻せませんでした "
                            "(このレンダラーバックエンドは未対応の可能性があります)。"
                            "vfx.previewEnsure で rendererReady を確認してください");
    }

    // 占有判定の基準は RenderAiPreview / RenderVFXPreview が使う Clear 色と同じ値。
    // ここがずれると背景そのものを「描かれた画素」と数えてしまう。
    static constexpr float kPreviewBackground[3] = { 0.018f, 0.021f, 0.028f };
    const PreviewMetrics metrics = ComputePreviewMetrics(
        rgba, width, height, kPreviewBackground, previousLuminance, &outLuminance);

    JsonValue exposure = JsonValue::MakeObject();
    exposure.Set("luminanceMean", JsonValue(metrics.luminanceMean));
    exposure.Set("coveredLuminanceMean", JsonValue(metrics.coveredLuminanceMean));
    exposure.Set("luminanceP99", JsonValue(metrics.luminanceP99));
    exposure.Set("luminanceMax", JsonValue(metrics.luminanceMax));
    exposure.Set("clippedRatio", JsonValue(metrics.clippedRatio));
    exposure.Set("blownOutRatio", JsonValue(metrics.blownOutRatio));
    JsonValue histogram = JsonValue::MakeArray();
    for (float bucket : metrics.histogram) histogram.Push(JsonValue(bucket));
    exposure.Set("histogram", std::move(histogram));

    JsonValue occupancy = JsonValue::MakeObject();
    occupancy.Set("coverage", JsonValue(metrics.coverage));
    occupancy.Set("centroidX", JsonValue(metrics.centroidX));
    occupancy.Set("centroidY", JsonValue(metrics.centroidY));
    JsonValue bounds = JsonValue::MakeArray();
    bounds.Push(JsonValue(metrics.boundsMinX));
    bounds.Push(JsonValue(metrics.boundsMinY));
    bounds.Push(JsonValue(metrics.boundsMaxX));
    bounds.Push(JsonValue(metrics.boundsMaxY));
    occupancy.Set("bounds", std::move(bounds));

    JsonValue motion = JsonValue::MakeObject();
    motion.Set("comparedWithPrevious", JsonValue(metrics.hasPrevious));
    if (metrics.hasPrevious) {
        motion.Set("meanLuminanceDelta", JsonValue(metrics.motion));
        motion.Set("changedRatio", JsonValue(metrics.changedRatio));
    }

    JsonValue issues = JsonValue::MakeArray();
    for (const std::string& issue : DescribePreviewMetricIssues(metrics)) issues.Push(JsonValue(issue));

    JsonValue result = JsonValue::MakeObject();
    result.Set("width", JsonValue(static_cast<int>(width)));
    result.Set("height", JsonValue(static_cast<int>(height)));
    result.Set("exposure", std::move(exposure));
    result.Set("occupancy", std::move(occupancy));
    result.Set("motion", std::move(motion));
    result.Set("issues", std::move(issues));
    result.Set("hint", JsonValue(std::string(
        "issues が空なら機械的に判る破綻は無い、という意味であって「良い絵」という意味ではない。"
        "そこから先 (炎に見えるか、狙った勢いか) は画像と vfx_guide の規約で判断すること。"
        "histogram は log2 輝度 [-8,+8] の 16 等分で、後ろの階級ほど明るい。"
        "motion は直前に測ったフレームとの比較なので、時間を進めながら連続で呼ぶと意味を持つ。")));
    return Outcome::Ok(std::move(result));
}

} // namespace

// ── 応答生成の内部ヘルパ ─────────────────────────────────────────────────────
namespace {
using editor::CompositeCommand;
using editor::ICommand;
using editor::LambdaCommand;

// dryRun 応答: 変更せず「何をする予定か」を返す。
Outcome DryRunPreview(const std::string& type)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("dryRun", JsonValue(true));
    result.Set("would", JsonValue(type));
    return Outcome::Ok(std::move(result));
}

// 2つの JSON 値が同じ大分類 (数値/真偽/文字列/配列/オブジェクト) かを判定する (component.set の型チェック)。
bool CompatibleJsonType(const JsonValue& a, const JsonValue& b)
{
    const auto category = [](const JsonValue& v) {
        if (v.IsNumber()) return 0;
        if (v.IsBool())   return 1;
        if (v.IsString()) return 2;
        if (v.IsArray())  return 3;
        if (v.IsObject()) return 4;
        return 5; // null
    };
    return category(a) == category(b);
}

// asset.list — projectRoot 相対の dir を非再帰列挙する。projectRoot 外は拒否する。
Outcome DoAssetList(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");

    std::error_code ec;
    fs::path base = fs::path(ctx.projectRoot);
    const std::string dir = StringField(payload, "dir");
    if (!dir.empty()) base /= fs::path(dir);

    const fs::path canonicalBase = fs::weakly_canonical(base, ec);
    const fs::path canonicalRoot = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    if (ec) return Outcome::Err("BAD_PATH", "パス解決に失敗しました");

    // projectRoot の外 (.. を含む) は列挙させない。
    const std::string baseStr = canonicalBase.generic_string();
    const std::string rootStr = canonicalRoot.generic_string();
    if (baseStr.rfind(rootStr, 0) != 0) return Outcome::Err("BAD_PATH", "projectRoot の外は列挙できません");
    if (!fs::is_directory(canonicalBase, ec)) return Outcome::Err("NOT_A_DIR", "ディレクトリではありません: " + dir);

    JsonValue entries = JsonValue::MakeArray();
    for (const auto& item : fs::directory_iterator(canonicalBase, fs::directory_options::skip_permission_denied, ec)) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(item.path().filename().generic_string()));
        entry.Set("dir", JsonValue(item.is_directory(ec)));
        entries.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("dir", JsonValue(dir));
    result.Set("entries", std::move(entries));
    return Outcome::Ok(std::move(result));
}

bool ResolveProjectFile(const editor::EditorContext& ctx, const std::string& requested,
                        std::filesystem::path& outPath, std::string& outRelative)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty() || requested.empty()) return false;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    outPath = fs::weakly_canonical(root / fs::path(requested), ec);
    const fs::path relative = fs::relative(outPath, root, ec);
    if (ec || relative.empty() || relative.begin()->generic_string() == "..") return false;
    outRelative = relative.generic_string();
    return true;
}

bool IsTextAssetExtension(const std::string& extension)
{
    static const std::unordered_set<std::string> extensions = {
        ".scene", ".prefab", ".mat", ".tex", ".terrain", ".animcontroller", ".vfx",
        ".toml", ".json", ".meta", ".hpp", ".cpp", ".hlsl", ".hlsli"
    };
    return extensions.contains(LowerAscii(extension));
}

bool ReadSmallTextFile(const std::filesystem::path& path, std::string& outText)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > 2u * 1024u * 1024u || !IsTextAssetExtension(path.extension().string())) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    outText.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> ExtractAssetReferences(const std::string& text)
{
    std::vector<std::string> references;
    const std::string lower = LowerAscii(text);
    size_t position = 0;
    while ((position = lower.find("assets/", position)) != std::string::npos) {
        size_t end = position;
        while (end < text.size()) {
            const char character = text[end];
            if (character == '"' || character == '\'' || character == '\r' || character == '\n'
                || character == ']' || character == '}' || character == ',') break;
            ++end;
        }
        std::string reference = text.substr(position, end - position);
        std::replace(reference.begin(), reference.end(), '\\', '/');
        while (!reference.empty() && std::isspace(static_cast<unsigned char>(reference.back()))) reference.pop_back();
        if (!reference.empty()
            && std::find(references.begin(), references.end(), reference) == references.end()) {
            references.push_back(std::move(reference));
        }
        position = end > position ? end : position + 1;
    }
    return references;
}

Outcome DoAssetInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path path;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), path, relative)) {
        return Outcome::Err("BAD_PATH", "projectRoot 配下のアセットを指定してください");
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return Outcome::Err("ASSET_NOT_FOUND", "アセットが見つかりません: " + relative);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("extension", JsonValue(LowerAscii(path.extension().string())));
    result.Set("sizeBytes", JsonValue(static_cast<std::int64_t>(fs::file_size(path, ec))));
    result.Set("modifiedTicks", JsonValue(static_cast<std::int64_t>(fs::last_write_time(path, ec).time_since_epoch().count())));
    const std::string extension = LowerAscii(path.extension().string());
    result.Set("directThumbnail", JsonValue(extension == ".png" || extension == ".jpg" || extension == ".jpeg"));

    std::string text;
    JsonValue references = JsonValue::MakeArray();
    if (ReadSmallTextFile(path, text)) {
        for (const std::string& reference : ExtractAssetReferences(text)) references.Push(JsonValue(reference));
    }
    result.Set("references", std::move(references));

    JsonValue referencedBy = JsonValue::MakeArray();
    int totalReferrers = 0;
    const std::string needle = LowerAscii(relative);
    const fs::path assetsRoot = fs::path(ctx.projectRoot) / "Assets";
    for (fs::recursive_directory_iterator iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
         iterator != end; iterator.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!iterator->is_regular_file(ec) || iterator->path() == path) continue;
        std::string candidateText;
        if (!ReadSmallTextFile(iterator->path(), candidateText)
            || LowerAscii(candidateText).find(needle) == std::string::npos) continue;
        ++totalReferrers;
        if (referencedBy.AsArray().size() < 100) {
            referencedBy.Push(JsonValue(fs::relative(iterator->path(), fs::path(ctx.projectRoot), ec).generic_string()));
        }
    }
    result.Set("referencedBy", std::move(referencedBy));
    result.Set("referrerCount", JsonValue(totalReferrers));
    return Outcome::Ok(std::move(result));
}

Outcome DoAssetFindUnused(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 100, 1, 500);
    const fs::path projectRoot(ctx.projectRoot);
    const fs::path assetsRoot = projectRoot / "Assets";
    std::error_code ec;
    std::unordered_set<std::string> referenced;
    std::vector<fs::path> candidates;
    static const std::unordered_set<std::string> candidateExtensions = {
        ".mat", ".mesh", ".tex", ".prefab", ".animcontroller", ".vfx", ".wav", ".ogg", ".fbx", ".png", ".jpg", ".dds"
    };
    for (fs::recursive_directory_iterator iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
         iterator != end; iterator.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!iterator->is_regular_file(ec)) continue;
        const std::string extension = LowerAscii(iterator->path().extension().string());
        if (candidateExtensions.contains(extension)) candidates.push_back(iterator->path());
        std::string text;
        if (!ReadSmallTextFile(iterator->path(), text)) continue;
        for (const std::string& reference : ExtractAssetReferences(text)) referenced.insert(LowerAscii(reference));
    }

    JsonValue unused = JsonValue::MakeArray();
    int total = 0;
    for (const fs::path& candidate : candidates) {
        const std::string relative = fs::relative(candidate, projectRoot, ec).generic_string();
        if (referenced.contains(LowerAscii(relative))) continue;
        ++total;
        if (static_cast<int>(unused.AsArray().size()) < limit) unused.Push(JsonValue(relative));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("assets", std::move(unused));
    result.Set("count", JsonValue(total));
    result.Set("truncated", JsonValue(total > limit));
    result.Set("heuristic", JsonValue(true));
    return Outcome::Ok(std::move(result));
}

Outcome DoAssetThumbnail(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path path;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), path, relative)) {
        return Outcome::Err("BAD_PATH", "projectRoot 配下の画像を指定してください");
    }
    const std::string extension = LowerAscii(path.extension().string());
    const char* mime = extension == ".png" ? "image/png"
        : (extension == ".jpg" || extension == ".jpeg" ? "image/jpeg" : nullptr);
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (mime == nullptr || ec || size == 0 || size > 16u * 1024u * 1024u) {
        return Outcome::Err("NO_DIRECT_THUMBNAIL", "PNG/JPEG の直接サムネイルだけを取得できます");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Outcome::Err("ASSET_READ_FAILED", "画像を読み取れません: " + relative);
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>{});
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue(mime));
    result.Set("base64", JsonValue(Base64Encode(bytes)));
    result.Set("path", JsonValue(relative));
    return Outcome::Ok(std::move(result));
}

// viewport.capture — 指定ViewのRTを PNG(base64) にして返す。実RTサイズを width/height に載せる。
Outcome DoViewportCapture(editor::EditorContext& ctx, renderer::ResourceHandle<renderer::RenderTargetTag> rt)
{
    if (ctx.renderer == nullptr || ctx.resources == nullptr) return Outcome::Err("NO_RENDERER", "レンダラーが未初期化です");
    if (!rt.IsValid()) return Outcome::Err("NO_VIEWPORT", "Scene View RT が未生成です");

    std::vector<uint8_t> png;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!ctx.renderer->CaptureRenderTargetToPng(rt, *ctx.resources, png, width, height) || png.empty()) {
        return Outcome::Err("CAPTURE_FAILED", "viewport のキャプチャに失敗しました");
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue("image/png"));
    result.Set("base64", JsonValue(Base64Encode(png)));
    result.Set("width", JsonValue(static_cast<int>(width)));
    result.Set("height", JsonValue(static_cast<int>(height)));
    return Outcome::Ok(std::move(result));
}

const char* PlayStateName(const editor::PlayModeController& playMode)
{
    if (playMode.IsPlaying()) return "playing";
    if (playMode.IsPaused()) return "paused";
    return "editor";
}

Outcome DoEditorState(const editor::EditorContext& ctx)
{
    if (ctx.playMode == nullptr) return Outcome::Err("NO_PLAY_MODE", "PlayModeController が未設定です");
    JsonValue result = JsonValue::MakeObject();
    result.Set("playState", JsonValue(PlayStateName(*ctx.playMode)));
    result.Set("restorePending", JsonValue(ctx.playMode->HasPendingRestore()));
    result.Set("scene", JsonValue(ctx.currentScenePath));
    result.Set("sceneDirty", JsonValue(ctx.sceneDirty));
    result.Set("scriptReloadBusy", JsonValue(ctx.scriptReloadBusy));
    result.Set("frameIndex", JsonValue(static_cast<std::int64_t>(Time::frameCount)));
    return Outcome::Ok(std::move(result));
}

// Undo/Redo スタックの状態を返す (read+)。
// WHY: 自律ループで「自分の編集が期待どおりのラベル付きエントリとして残ったか」を検証でき、
//      何回 undo すれば直前の意図した状態へ戻れるかを AI が判断できる。
Outcome DoUndoHistory(const editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(
        limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 50, 1, 128);

    const auto history = ctx.undoStack->GetHistory();
    const int total = static_cast<int>(history.size());
    // 新しい順 (末尾から) に最大 limit 件返す。index は履歴内の実位置を保つ。
    JsonValue entries = JsonValue::MakeArray();
    for (int i = total - 1; i >= 0 && static_cast<int>(entries.AsArray().size()) < limit; --i) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("index", JsonValue(i));
        entry.Set("description", JsonValue(history[static_cast<size_t>(i)].description));
        entry.Set("applied", JsonValue(history[static_cast<size_t>(i)].applied));
        entries.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("canUndo", JsonValue(ctx.undoStack->CanUndo()));
    result.Set("canRedo", JsonValue(ctx.undoStack->CanRedo()));
    result.Set("cursor", JsonValue(static_cast<int>(ctx.undoStack->GetCursor())));
    result.Set("count", JsonValue(total));
    if (ctx.undoStack->CanUndo()) result.Set("undoDescription", JsonValue(ctx.undoStack->GetUndoDescription()));
    if (ctx.undoStack->CanRedo()) result.Set("redoDescription", JsonValue(ctx.undoStack->GetRedoDescription()));
    result.Set("entries", std::move(entries));
    return Outcome::Ok(std::move(result));
}

int LogLevelRank(core::LogLevel level)
{
    return static_cast<int>(level);
}

const char* LogLevelName(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return "debug";
    case core::LogLevel::INFO:      return "info";
    case core::LogLevel::WARNING:   return "warning";
    case core::LogLevel::LOG_ERROR: return "error";
    }
    return "unknown";
}

Outcome DoConsoleLogs(const editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.consoleSink == nullptr) return Outcome::Err("NO_CONSOLE", "ConsoleSink が未設定です");
    const std::string requestedLevel = StringField(payload, "minLevel");
    int minimumRank = 0;
    if (requestedLevel == "info") minimumRank = 1;
    else if (requestedLevel == "warning") minimumRank = 2;
    else if (requestedLevel == "error") minimumRank = 3;
    const std::string contains = LowerAscii(StringField(payload, "contains"));
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 100, 1, 512);
    const JsonValue* afterSequenceValue = payload.Find("afterSequence");
    constexpr double MAX_SAFE_JSON_INTEGER = 9007199254740991.0;
    const double requestedAfterSequence = afterSequenceValue != nullptr && afterSequenceValue->IsNumber()
        ? afterSequenceValue->AsNumber() : 0.0;
    const std::uint64_t afterSequence = static_cast<std::uint64_t>(
        std::clamp(requestedAfterSequence, 0.0, MAX_SAFE_JSON_INTEGER));

    JsonValue entries = JsonValue::MakeArray();
    int matched = 0;
    const auto& source = ctx.consoleSink->GetEntries();
    std::uint64_t sequence = ctx.consoleSink->GetLatestSequence();
    for (auto iterator = source.rbegin(); iterator != source.rend(); ++iterator) {
        const std::uint64_t entrySequence = sequence--;
        if (entrySequence <= afterSequence) continue;
        if (LogLevelRank(iterator->level) < minimumRank) continue;
        if (!contains.empty() && LowerAscii(iterator->message).find(contains) == std::string::npos) continue;
        ++matched;
        if (static_cast<int>(entries.AsArray().size()) >= limit) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("sequence", JsonValue(static_cast<std::int64_t>(entrySequence)));
        entry.Set("level", JsonValue(LogLevelName(iterator->level)));
        entry.Set("message", JsonValue(iterator->message));
        entries.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("entries", std::move(entries));
    result.Set("count", JsonValue(matched));
    result.Set("truncated", JsonValue(matched > limit));
    result.Set("order", JsonValue("newest-first"));
    result.Set("cursor", JsonValue(static_cast<std::int64_t>(ctx.consoleSink->GetLatestSequence())));
    result.Set("oldestSequence", JsonValue(static_cast<std::int64_t>(ctx.consoleSink->GetOldestSequence())));
    result.Set("dropped", JsonValue(afterSequence > 0
        && afterSequence + 1 < ctx.consoleSink->GetOldestSequence()));
    return Outcome::Ok(std::move(result));
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

JsonValue VectorToJson(const math::Vector3& value)
{
    JsonValue result = JsonValue::MakeArray();
    result.Push(JsonValue(value.x));
    result.Push(JsonValue(value.y));
    result.Push(JsonValue(value.z));
    return result;
}

JsonValue QuaternionToJson(const math::Quaternion& value)
{
    JsonValue result = JsonValue::MakeArray();
    result.Push(JsonValue(value.x));
    result.Push(JsonValue(value.y));
    result.Push(JsonValue(value.z));
    result.Push(JsonValue(value.w));
    return result;
}

// Scene の比較に必要な安定 ID・Transform・公開 Component を正規化して返す。
JsonValue BuildSceneSnapshot(scene::Scene& activeScene)
{
    JsonValue nodes = JsonValue::MakeArray();
    int generatedCount = 0;
    for (GameObject& go : activeScene.GameObjects()) {
        // ランタイム生成物は snapshot の対象外。snapshot は「編集の前後を比べる」ためのもので、
        // 保存されない GO を含めると、再生状態が違うだけで毎回差分が出て比較にならない。
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
    // 除外した件数は出す。0 でないのに snapshot に無いことを不整合と読ませないため。
    if (generatedCount > 0) result.Set("excludedGenerated", JsonValue(generatedCount));
    return result;
}

// Game Viewの実RTと同じアスペクト比・カメラ解決規則で、意味付き投影用Cameraを再構成する。
// WHY: Scene Viewカメラや独自のCameraComponent選択を使うと、描画画像とpixel座標が一致しない。
bool BuildGameViewportCamera(editor::EditorContext& ctx,
                             renderer::ResourceHandle<renderer::RenderTargetTag> rt,
                             renderer::Camera& camera)
{
    if (ctx.activeScene == nullptr || ctx.editorCamera == nullptr || ctx.resources == nullptr || !rt.IsValid()) {
        return false;
    }
    const auto* renderTarget = ctx.resources->Get(rt);
    if (renderTarget == nullptr || renderTarget->GetHeight() == 0) return false;
    const float aspect = static_cast<float>(renderTarget->GetWidth())
        / static_cast<float>(renderTarget->GetHeight());
    camera = scene::ResolveEditorGameCamera(*ctx.activeScene, *ctx.editorCamera, aspect);
    return true;
}

// PNG に各 GameObject のワールド座標と投影ピクセル座標を添える。
Outcome DoSemanticViewportCapture(editor::EditorContext& ctx,
                                  renderer::ResourceHandle<renderer::RenderTargetTag> rt,
                                  const renderer::Camera& camera,
                                  const char* viewName)
{
    Outcome capture = DoViewportCapture(ctx, rt);
    if (!capture.ok) return capture;
    if (ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* widthValue = capture.result.Find("width");
    const JsonValue* heightValue = capture.result.Find("height");
    const float width = widthValue != nullptr ? static_cast<float>(widthValue->AsNumber()) : 0.0f;
    const float height = heightValue != nullptr ? static_cast<float>(heightValue->AsNumber()) : 0.0f;
    const math::Matrix4 viewProjection = camera.GetViewProjection();

    JsonValue objects = JsonValue::MakeArray();
    for (GameObject& go : ctx.activeScene->GameObjects()) {
        const math::Vector3 worldPosition = go.transform.worldPosition;
        const math::Vector4 clip = viewProjection * math::Vector4(worldPosition, 1.0f);
        const bool inFront = clip.w > 0.00001f;
        const float ndcX = inFront ? clip.x / clip.w : 0.0f;
        const float ndcY = inFront ? clip.y / clip.w : 0.0f;
        const float ndcZ = inFront ? clip.z / clip.w : 0.0f;
        const bool visible = inFront && ndcX >= -1.0f && ndcX <= 1.0f
            && ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;

        JsonValue object = JsonValue::MakeObject();
        object.Set("id", JsonValue(go.instanceId));
        object.Set("name", JsonValue(go.name));
        object.Set("worldPosition", VectorToJson(worldPosition));
        object.Set("visible", JsonValue(visible));
        object.Set("depth", JsonValue(ndcZ));
        JsonValue pixel = JsonValue::MakeArray();
        pixel.Push(JsonValue((ndcX * 0.5f + 0.5f) * width));
        pixel.Push(JsonValue((1.0f - (ndcY * 0.5f + 0.5f)) * height));
        object.Set("pixel", std::move(pixel));
        if (GameObject* parent = go.GetParent()) object.Set("parent", JsonValue(parent->instanceId));
        objects.Push(std::move(object));
    }
    capture.result.Set("objects", std::move(objects));
    capture.result.Set("cameraPosition", VectorToJson(camera.m_position));
    capture.result.Set("view", JsonValue(viewName));
    return capture;
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

scene::AnimatorComponent* FindAnimator(editor::EditorContext& ctx, const JsonValue& payload, Outcome& error)
{
    if (ctx.activeScene == nullptr) {
        error = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
        return nullptr;
    }
    const std::string id = StringField(payload, "id");
    GameObject* go = ctx.activeScene->FindByGuid(id);
    if (go == nullptr) {
        error = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id);
        return nullptr;
    }
    scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
    if (animator == nullptr) error = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません");
    return animator;
}

Outcome DoAnimationState(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    JsonValue clips = JsonValue::MakeArray();
    for (size_t index = 0; index < animator->clips.size(); ++index) {
        const auto& clip = animator->clips[index];
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("index", JsonValue(static_cast<int>(index)));
        entry.Set("name", JsonValue(clip.name));
        entry.Set("duration", JsonValue(clip.GetDurationSeconds()));
        entry.Set("frameRate", JsonValue(clip.frameRate));
        clips.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("playing", JsonValue(animator->playing));
    result.Set("enabled", JsonValue(animator->enabled));
    result.Set("clipName", JsonValue(animator->clipName));
    result.Set("clipIndex", JsonValue(animator->clipIndex));
    result.Set("time", JsonValue(animator->time));
    result.Set("state", JsonValue(animator->currentStateName));
    result.Set("stateTime", JsonValue(animator->stateTime));
    result.Set("normalizedTime", JsonValue(animator->GetNormalizedTime()));
    result.Set("blendToState", JsonValue(animator->GetBlendToState()));
    result.Set("blendWeight", JsonValue(animator->blendWeight));
    result.Set("clips", std::move(clips));
    return Outcome::Ok(std::move(result));
}

JsonValue MatrixToJson(const math::Matrix4& matrix)
{
    JsonValue result = JsonValue::MakeArray();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) result.Push(JsonValue(matrix.m[row][column]));
    }
    return result;
}

// AnimatorSystem が直近フレームに評価したノード行列を、Skeleton の名前と親子情報付きで返す。
Outcome DoAnimationPose(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    const std::string id = StringField(payload, "id");
    GameObject* go = ctx.activeScene->FindByGuid(id);
    scene::SkinnedMeshRenderer* renderer = go->GetComponent<scene::SkinnedMeshRenderer>();
    if (renderer == nullptr) {
        for (int index = 0; index < go->GetChildCount(); ++index) {
            GameObject* child = go->GetChild(index);
            if (child != nullptr && (renderer = child->GetComponent<scene::SkinnedMeshRenderer>()) != nullptr) break;
        }
    }
    if (renderer == nullptr || renderer->model == nullptr || renderer->model->skeleton == nullptr) {
        return Outcome::Err("SKELETON_NOT_READY", "SkinnedMeshRenderer の Skeleton がまだロードされていません");
    }
    const asset::Skeleton& skeleton = *renderer->model->skeleton;
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 128, 1, 512);
    const size_t available = std::min(skeleton.nodes.size(), animator->nodeGlobalTransforms.size());
    JsonValue nodes = JsonValue::MakeArray();
    for (size_t index = 0; index < available && static_cast<int>(index) < limit; ++index) {
        const asset::SkeletonNode& skeletonNode = skeleton.nodes[index];
        JsonValue node = JsonValue::MakeObject();
        node.Set("index", JsonValue(static_cast<int>(index)));
        node.Set("name", JsonValue(skeletonNode.name));
        node.Set("parentIndex", JsonValue(skeletonNode.parentIndex));
        node.Set("boneIndex", JsonValue(skeletonNode.boneIndex));
        node.Set("globalMatrix", MatrixToJson(animator->nodeGlobalTransforms[index]));
        nodes.Push(std::move(node));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(id));
    result.Set("evaluated", JsonValue(!animator->nodeGlobalTransforms.empty()));
    result.Set("nodes", std::move(nodes));
    result.Set("count", JsonValue(static_cast<int>(std::min(available, static_cast<size_t>(limit)))));
    result.Set("total", JsonValue(static_cast<int>(available)));
    result.Set("truncated", JsonValue(available > static_cast<size_t>(limit)));
    return Outcome::Ok(std::move(result));
}

Outcome DoAnimationControl(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    const std::string action = StringField(payload, "action");
    const std::string clipName = StringField(payload, "clipName");
    const std::string stateName = StringField(payload, "state");
    const JsonValue* clipIndexValue = payload.Find("clipIndex");
    const JsonValue* timeValue = payload.Find("time");
    const JsonValue* frameValue = payload.Find("frame");
    if (action != "play" && action != "pause" && action != "stop" && action != "seek") {
        return Outcome::Err("BAD_ARG", "未知の animation action: " + action);
    }
    if (!stateName.empty() && (!clipName.empty() || clipIndexValue != nullptr)) {
        return Outcome::Err("BAD_ARG", "state と clipName/clipIndex は同時指定できません");
    }
    if (action == "seek" && (timeValue == nullptr || !timeValue->IsNumber())
        && (frameValue == nullptr || !frameValue->IsNumber())) {
        return Outcome::Err("BAD_ARG", "seek には time または frame が必要です");
    }
    if (!stateName.empty()) {
        const auto iterator = std::find_if(animator->states.begin(), animator->states.end(), [&](const scene::AnimationState& state) {
            return state.name == stateName;
        });
        if (iterator == animator->states.end()) return Outcome::Err("STATE_NOT_FOUND", "Animator state が見つかりません: " + stateName);
    }
    if (!clipName.empty() && !animator->clips.empty()) {
        const bool found = std::any_of(animator->clips.begin(), animator->clips.end(), [&](const asset::AnimationClip& clip) {
            return clip.name == clipName;
        });
        if (!found) return Outcome::Err("CLIP_NOT_FOUND", "Animation clip が見つかりません: " + clipName);
    }
    if (clipIndexValue != nullptr && clipIndexValue->IsNumber() && !animator->clips.empty()) {
        const int clipIndex = clipIndexValue->AsInt();
        if (clipIndex < 0 || clipIndex >= static_cast<int>(animator->clips.size())) {
            return Outcome::Err("CLIP_NOT_FOUND", "clipIndex がロード済みclip範囲外です");
        }
    }
    if (dryRun) return DryRunPreview("animation.control:" + action);

    if (!clipName.empty()) animator->clipName = clipName;
    if (clipIndexValue != nullptr && clipIndexValue->IsNumber()) animator->clipIndex = clipIndexValue->AsInt();
    if (!stateName.empty()) {
        animator->currentStateName = stateName;
        animator->stateTime = 0.0f;
        animator->blendToState.clear();
        animator->blendToTime = 0.0f;
        animator->blendWeight = 0.0f;
    }

    if (action == "play") animator->playing = true;
    else if (action == "pause") animator->playing = false;
    else if (action == "stop") {
        animator->playing = false;
        animator->time = 0.0f;
        animator->stateTime = 0.0f;
    } else if (action == "seek") {
        float seconds = timeValue != nullptr && timeValue->IsNumber() ? static_cast<float>(timeValue->AsNumber()) : -1.0f;
        if (frameValue != nullptr && frameValue->IsNumber()) {
            const asset::AnimationClip* clip = nullptr;
            if (!animator->clipName.empty()) {
                for (const auto& candidate : animator->clips) if (candidate.name == animator->clipName) { clip = &candidate; break; }
            }
            if (clip == nullptr && animator->clipIndex >= 0 && animator->clipIndex < static_cast<int>(animator->clips.size())) {
                clip = &animator->clips[static_cast<size_t>(animator->clipIndex)];
            }
            if (clip == nullptr || clip->frameRate <= 0.0f) return Outcome::Err("CLIP_NOT_READY", "frame seek にはロード済み clip が必要です");
            seconds = static_cast<float>(frameValue->AsNumber()) / clip->frameRate;
        }
        if (seconds < 0.0f) return Outcome::Err("BAD_ARG", "seek には time または frame が必要です");
        animator->time = seconds;
        animator->stateTime = seconds;
    }
    return DoAnimationState(ctx, payload);
}

// ── Animator ステートマシン照会・パラメーター駆動 ─────────────────────────────
// WHY: 既存の animation.state は再生状態、animation.pose は骨行列に限られ、
//      「どのパラメーターがどの遷移を発火させるか」という構造を AI が把握できなかった。
//      グラフ構造の照会とパラメーター駆動を足し、AI が遷移や BlendTree を自律検証できるようにする。

const char* AnimatorParamTypeName(scene::ParamType type)
{
    switch (type) {
    case scene::ParamType::Float:   return "float";
    case scene::ParamType::Int:     return "int";
    case scene::ParamType::Bool:    return "bool";
    case scene::ParamType::Trigger: return "trigger";
    }
    return "unknown";
}

const char* AnimatorConditionOpName(scene::ConditionOp op)
{
    switch (op) {
    case scene::ConditionOp::Greater:  return "greater";
    case scene::ConditionOp::Less:     return "less";
    case scene::ConditionOp::Equal:    return "equal";
    case scene::ConditionOp::NotEqual: return "notEqual";
    case scene::ConditionOp::True:     return "true";
    case scene::ConditionOp::False:    return "false";
    }
    return "unknown";
}

// AnimatorConditionOpName の逆変換。未知文字列は false を返す。
bool ParseConditionOp(const std::string& name, scene::ConditionOp& out)
{
    if (name == "greater")  { out = scene::ConditionOp::Greater;  return true; }
    if (name == "less")     { out = scene::ConditionOp::Less;     return true; }
    if (name == "equal")    { out = scene::ConditionOp::Equal;    return true; }
    if (name == "notEqual") { out = scene::ConditionOp::NotEqual; return true; }
    if (name == "true")     { out = scene::ConditionOp::True;     return true; }
    if (name == "false")    { out = scene::ConditionOp::False;    return true; }
    return false;
}

// Bool/Trigger 用 (True/False) は閾値を使わない。それ以外 (Float/Int 比較) は threshold が有効。
bool ConditionOpUsesThreshold(scene::ConditionOp op)
{
    return op != scene::ConditionOp::True && op != scene::ConditionOp::False;
}

const char* AnimationStateModeName(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return "clip";
    case scene::AnimationStateMode::BlendTree1D: return "blendTree1D";
    case scene::AnimationStateMode::BlendTree2D: return "blendTree2D";
    }
    return "unknown";
}

JsonValue TransitionToJson(const scene::AnimationTransition& transition)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("toState", JsonValue(transition.toStateName));
    entry.Set("hasExitTime", JsonValue(transition.hasExitTime));
    entry.Set("exitTime", JsonValue(transition.exitTime));
    entry.Set("fixedDuration", JsonValue(transition.fixedDuration));
    entry.Set("transitionDuration", JsonValue(transition.transitionDuration));
    JsonValue conditions = JsonValue::MakeArray();
    for (const auto& condition : transition.conditions) {
        JsonValue conditionJson = JsonValue::MakeObject();
        conditionJson.Set("parameter", JsonValue(condition.paramName));
        conditionJson.Set("op", JsonValue(AnimatorConditionOpName(condition.op)));
        conditionJson.Set("threshold", JsonValue(condition.threshold));
        conditions.Push(std::move(conditionJson));
    }
    entry.Set("conditions", std::move(conditions));
    return entry;
}

// パラメーターの宣言型に応じた現在値を JSON へ書き込む (Trigger は bool として扱う)。
void SetAnimatorParamValueJson(JsonValue& target, const scene::AnimatorParameter& param)
{
    switch (param.type) {
    case scene::ParamType::Float:   target.Set("value", JsonValue(param.floatValue)); break;
    case scene::ParamType::Int:     target.Set("value", JsonValue(param.intValue)); break;
    case scene::ParamType::Bool:
    case scene::ParamType::Trigger: target.Set("value", JsonValue(param.boolValue)); break;
    }
}

// AnimatorComponent のステートマシン全体 (states / transitions / parameters / anyState) を、
// 現在の再生ステートやパラメーターのライブ値付きで返す。
// レイヤー 1 件の要約 JSON。マスク・加算設定・ステート数・実行中ステートを含める。
// WHY: レイヤーは書けるが読めない状態だと、AI は自分が作った構成を確認できず
//      重複作成や存在しないレイヤーへの操作を繰り返す。書き込み API と対になる読み出しを用意する。
JsonValue AnimationLayerToJson(const scene::AnimationLayer& layer)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("name", JsonValue(layer.name));
    entry.Set("enabled", JsonValue(layer.enabled));
    entry.Set("weight", JsonValue(layer.weight));
    entry.Set("mode", JsonValue(std::string(
        layer.mode == scene::AnimationLayerMode::Additive ? "additive" : "override")));
    entry.Set("maskPath", JsonValue(layer.mask.path));
    entry.Set("maskLoaded", JsonValue(layer.mask.loaded));

    // 独自ステートマシンの有無で、どちらの再生方式かが決まる。
    entry.Set("hasOwnStateMachine", JsonValue(!layer.states.empty()));
    entry.Set("stateCount", JsonValue(static_cast<int>(layer.states.size())));
    entry.Set("defaultState", JsonValue(layer.defaultStateName));
    entry.Set("stateName", JsonValue(layer.stateName));
    entry.Set("currentState", JsonValue(
        layer.states.empty() ? layer.stateName : layer.runtime.currentStateName));
    entry.Set("blendToState", JsonValue(layer.runtime.blendToState));

    if (layer.mode == scene::AnimationLayerMode::Additive) {
        JsonValue additive = JsonValue::MakeObject();
        additive.Set("sourcePath", JsonValue(layer.additiveReference.sourcePath));
        additive.Set("clipName", JsonValue(layer.additiveReference.clipName));
        additive.Set("time", JsonValue(layer.additiveReference.time));
        entry.Set("additiveReference", std::move(additive));
    }

    JsonValue slot = JsonValue::MakeObject();
    slot.Set("active", JsonValue(layer.slot.active));
    slot.Set("stopping", JsonValue(layer.slot.stopping));
    slot.Set("weight", JsonValue(layer.slot.weight));
    slot.Set("clipName", JsonValue(layer.slot.clipName));
    slot.Set("sourcePath", JsonValue(layer.slot.sourcePath));
    entry.Set("slot", std::move(slot));
    return entry;
}

// ── Avatar Mask (.mask) ─────────────────────────────────────────────────────
// WHY: レイヤーに maskPath を割り当てられても、マスク自体を作れなければ
//      MCP から上半身/下半身の出し分けを完結できない。アセット単位で読み書きする。

// プロジェクト相対パスを解決し、.mask かどうかを検証する。
bool ResolveAvatarMaskPath(editor::EditorContext& ctx, const JsonValue& payload,
                           std::string& outAbsPath, Outcome& error)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) { error = Outcome::Err("BAD_ARG", "path が必要です"); return false; }
    const std::string lower = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
    if (lower != ".mask") {
        error = Outcome::Err("BAD_ARG", "path は .mask である必要があります: " + path);
        return false;
    }
    // "Assets/..." 相対も絶対も受ける。projectRoot 配下へ閉じ込める。
    outAbsPath = util::FileSystem::IsChildPathText(path, ctx.projectRoot)
        ? path
        : util::FileSystem::PathToUtf8(
              util::FileSystem::PathFromUtf8(ctx.projectRoot) /
              util::FileSystem::PathFromUtf8(path));
    return true;
}

JsonValue AvatarMaskToJson(const asset::AvatarMaskAsset& mask, const std::string& path)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("name", JsonValue(mask.name));
    result.Set("defaultInclude", JsonValue(mask.defaultInclude));
    result.Set("skeletonSourcePath", JsonValue(mask.skeletonSourcePath));
    JsonValue entries = JsonValue::MakeArray();
    for (const auto& e : mask.entries) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("bone", JsonValue(e.bonePath));
        entry.Set("weight", JsonValue(e.weight));
        entry.Set("includeChildren", JsonValue(e.includeChildren));
        entry.Set("blendDepth", JsonValue(e.blendDepth));
        entries.Push(std::move(entry));
    }
    result.Set("entries", std::move(entries));
    return result;
}

Outcome DoAvatarMaskGet(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::string absPath;
    Outcome error;
    if (!ResolveAvatarMaskPath(ctx, payload, absPath, error)) return error;
    asset::AvatarMaskAsset mask;
    if (!asset::LoadAvatarMaskAsset(absPath, mask))
        return Outcome::Err("NOT_FOUND", "マスクを読み込めません: " + absPath);
    return Outcome::Ok(AvatarMaskToJson(mask, StringField(payload, "path")));
}

// マスクの作成と編集。Undo スタックには載せずファイルへ直接書く。
// WHY: 他のアセット生成 (Controller / VFX) と同じ扱い。シーン状態ではないため
//      UndoStack (シーン編集用) に混ぜると Undo の意味が食い違う。
Outcome DoAvatarMaskWrite(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    std::string absPath;
    Outcome error;
    if (!ResolveAvatarMaskPath(ctx, payload, absPath, error)) return error;

    const std::string action = StringField(payload, "action");
    static constexpr const char* kActions = "create/setBone/removeBone/clear/setDefaultInclude";
    if (action.empty())
        return Outcome::Err("BAD_ARG", std::string("action が必要です: ") + kActions);

    const bool exists = util::FileSystem::Exists(absPath);
    if (action == "create") {
        if (exists && !(payload.Find("overwrite") && payload.Find("overwrite")->IsBool()
                        && payload.Find("overwrite")->AsBool()))
            return Outcome::Err("ALREADY_EXISTS",
                                "既に存在します (overwrite=true で上書き): " + absPath);
    } else if (!exists) {
        return Outcome::Err("NOT_FOUND", "マスクが見つかりません: " + absPath);
    }

    asset::AvatarMaskAsset mask;
    if (exists && action != "create") {
        if (!asset::LoadAvatarMaskAsset(absPath, mask))
            return Outcome::Err("LOAD_FAILED", "マスクを読み込めません: " + absPath);
    }

    if (action == "create") {
        mask = asset::AvatarMaskAsset{};
        mask.name = util::FileSystem::GetFilename(absPath);
        if (const JsonValue* v = payload.Find("name"); v && v->IsString()) mask.name = v->AsString();
        if (const JsonValue* v = payload.Find("defaultInclude"); v && v->IsBool())
            mask.defaultInclude = v->AsBool();
        if (const JsonValue* v = payload.Find("skeletonSourcePath"); v && v->IsString())
            mask.skeletonSourcePath = v->AsString();
    } else if (action == "setDefaultInclude") {
        const JsonValue* v = payload.Find("defaultInclude");
        if (v == nullptr || !v->IsBool())
            return Outcome::Err("BAD_ARG", "defaultInclude (真偽) が必要です");
        mask.defaultInclude = v->AsBool();
    } else if (action == "clear") {
        mask.entries.clear();
    } else if (action == "setBone" || action == "removeBone") {
        const std::string bone = StringField(payload, "bone");
        if (bone.empty()) return Outcome::Err("BAD_ARG", "bone が必要です");

        auto it = std::find_if(mask.entries.begin(), mask.entries.end(),
            [&](const asset::AvatarMaskEntry& e) { return e.bonePath == bone; });

        if (action == "removeBone") {
            if (it == mask.entries.end())
                return Outcome::Err("BONE_NOT_FOUND", "エントリが見つかりません: " + bone);
            mask.entries.erase(it);
        } else {
            // setBone は upsert。既存があれば指定キーだけ更新する。
            asset::AvatarMaskEntry prototype;
            if (it != mask.entries.end()) prototype = *it;
            prototype.bonePath = bone;
            if (const JsonValue* v = payload.Find("weight"); v && v->IsNumber())
                prototype.weight = std::clamp(static_cast<float>(v->AsNumber()), 0.0f, 1.0f);
            if (const JsonValue* v = payload.Find("includeChildren"); v && v->IsBool())
                prototype.includeChildren = v->AsBool();
            if (const JsonValue* v = payload.Find("blendDepth"); v && v->IsNumber())
                prototype.blendDepth = (std::max)(0, v->AsInt());
            if (it != mask.entries.end()) *it = prototype;
            else mask.entries.push_back(std::move(prototype));
        }
    } else {
        return Outcome::Err("BAD_ARG", std::string("未知の action: ") + action +
                                       " (" + kActions + ")");
    }

    if (dryRun) return DryRunPreview("avatarMask." + action);

    util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(absPath));
    if (!asset::SaveAvatarMaskAsset(absPath, mask))
        return Outcome::Err("SAVE_FAILED", "マスクを保存できません: " + absPath);
    ctx.requestAssetBrowserRefresh = true;
    return Outcome::Ok(AvatarMaskToJson(mask, StringField(payload, "path")));
}

Outcome DoAnimationGraph(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    // layer 指定があれば、そのレイヤーの独自ステートマシンを返す。省略で Base Layer。
    const std::string layerName = StringField(payload, "layer");
    const scene::AnimationLayer* targetLayer = nullptr;
    if (!layerName.empty()) {
        targetLayer = animator->FindLayer(layerName);
        if (targetLayer == nullptr)
            return Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
    }

    const std::vector<scene::AnimationState>& targetStates =
        targetLayer ? targetLayer->states : animator->states;
    const std::vector<scene::AnimationTransition>& targetAnyState =
        targetLayer ? targetLayer->anyStateTransitions : animator->anyStateTransitions;
    const std::string& targetDefaultName =
        targetLayer ? targetLayer->defaultStateName : animator->defaultStateName;
    const std::string& targetCurrentName =
        targetLayer ? targetLayer->runtime.currentStateName : animator->currentStateName;

    JsonValue parameters = JsonValue::MakeArray();
    for (const auto& param : animator->parameters) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(param.name));
        entry.Set("type", JsonValue(AnimatorParamTypeName(param.type)));
        SetAnimatorParamValueJson(entry, param);
        parameters.Push(std::move(entry));
    }

    // 実効デフォルトステート名 (未指定時は先頭)。Entry リンクの解決規則と一致させる。
    std::string defaultState = targetDefaultName;
    if (defaultState.empty() && !targetStates.empty())
        defaultState = targetStates.front().name;

    JsonValue states = JsonValue::MakeArray();
    for (const auto& state : targetStates) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(state.name));
        entry.Set("mode", JsonValue(AnimationStateModeName(state.mode)));
        entry.Set("clipName", JsonValue(state.clipName));
        entry.Set("sourcePath", JsonValue(state.sourcePath));
        entry.Set("speed", JsonValue(state.speed));
        entry.Set("loop", JsonValue(state.loop));
        entry.Set("ikWeight", JsonValue(state.ikWeight));
        entry.Set("isDefault", JsonValue(state.name == defaultState));
        entry.Set("isCurrent", JsonValue(state.name == targetCurrentName));
        // BlendTree は駆動パラメーターとモーション数を要約表示する。
        if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            entry.Set("blendParameter", JsonValue(state.blendTree1D.paramName));
            entry.Set("motionCount", JsonValue(static_cast<int>(state.blendTree1D.motions.size())));
        } else if (state.mode == scene::AnimationStateMode::BlendTree2D) {
            entry.Set("blendParameterX", JsonValue(state.blendTree2D.paramX));
            entry.Set("blendParameterY", JsonValue(state.blendTree2D.paramY));
            entry.Set("motionCount", JsonValue(static_cast<int>(state.blendTree2D.motions.size())));
        }
        JsonValue transitions = JsonValue::MakeArray();
        for (const auto& transition : state.transitions)
            transitions.Push(TransitionToJson(transition));
        entry.Set("transitions", std::move(transitions));
        states.Push(std::move(entry));
    }

    JsonValue anyStateTransitions = JsonValue::MakeArray();
    for (const auto& transition : targetAnyState)
        anyStateTransitions.Push(TransitionToJson(transition));

    // layers はどのグラフを見ているかに関わらず常に返す。
    // WHY: 「Base Layer を見に来たが、実は上半身レイヤーがあった」を 1 回の問い合わせで気付ける。
    JsonValue layers = JsonValue::MakeArray();
    for (const auto& layer : animator->layers)
        layers.Push(AnimationLayerToJson(layer));

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("controllerPath", JsonValue(animator->controllerPath));
    // どのグラフを返したか。空なら Base Layer。
    result.Set("layer", JsonValue(layerName));
    result.Set("defaultState", JsonValue(defaultState));
    result.Set("currentState", JsonValue(targetCurrentName));
    result.Set("blendToState", JsonValue(
        targetLayer ? targetLayer->runtime.blendToState : animator->blendToState));
    result.Set("blendWeight", JsonValue(
        targetLayer ? targetLayer->runtime.blendWeight : animator->blendWeight));
    result.Set("baseLayerMaskPath", JsonValue(animator->baseLayerMask.path));
    result.Set("parameters", std::move(parameters));
    result.Set("states", std::move(states));
    result.Set("anyStateTransitions", std::move(anyStateTransitions));
    result.Set("layers", std::move(layers));
    return Outcome::Ok(std::move(result));
}

// Animator パラメーターを名前で設定する。宣言型に合わせて value を解釈し、
// AnimatorSystem が次フレームに遷移や BlendTree へ反映する。
// WHY: input_inject 経由の間接操作より直接的で、遷移条件や BlendTree の自律検証に使える。
Outcome DoAnimationSetParameter(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    const std::string name = StringField(payload, "name");
    if (name.empty()) return Outcome::Err("BAD_ARG", "パラメーター name が必要です");

    scene::AnimatorParameter* param = nullptr;
    for (auto& candidate : animator->parameters)
        if (candidate.name == name) { param = &candidate; break; }
    if (param == nullptr)
        return Outcome::Err("PARAM_NOT_FOUND", "Animator パラメーターが見つかりません: " + name);

    const JsonValue* valueField = payload.Find("value");

    // 型ごとの入力検証。Trigger だけは value 省略で「発火」を許可する。
    switch (param->type) {
    case scene::ParamType::Float:
    case scene::ParamType::Int:
        if (valueField == nullptr || !valueField->IsNumber())
            return Outcome::Err("BAD_ARG", "数値 value が必要です (" + std::string(AnimatorParamTypeName(param->type)) + ")");
        break;
    case scene::ParamType::Bool:
        if (valueField == nullptr || !valueField->IsBool())
            return Outcome::Err("BAD_ARG", "真偽 value が必要です (bool)");
        break;
    case scene::ParamType::Trigger:
        // 省略 = 発火 (true)。明示指定は bool のみ許可する (false でリセット可能)。
        if (valueField != nullptr && !valueField->IsBool())
            return Outcome::Err("BAD_ARG", "trigger の value は真偽のみ指定できます");
        break;
    }

    if (dryRun) return DryRunPreview("animation.setParameter:" + name);

    switch (param->type) {
    case scene::ParamType::Float:   param->floatValue = static_cast<float>(valueField->AsNumber()); break;
    case scene::ParamType::Int:     param->intValue = valueField->AsInt(); break;
    case scene::ParamType::Bool:    param->boolValue = valueField->AsBool(); break;
    case scene::ParamType::Trigger: param->boolValue = (valueField == nullptr) ? true : valueField->AsBool(); break;
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("name", JsonValue(param->name));
    result.Set("type", JsonValue(AnimatorParamTypeName(param->type)));
    SetAnimatorParamValueJson(result, *param);
    return Outcome::Ok(std::move(result));
}

// 指定ステートの BlendTree 構成を掘り下げて返す。
// animation.graph はモーション数の要約に留めるが、こちらは各 Motion の source/clip/座標/speed/IK と、
// 直近フレームのランタイム Weight まで含めて、BlendTree の中身を AI が精査できるようにする。
Outcome DoAnimationBlendTree(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    const std::string stateName = StringField(payload, "state");
    if (stateName.empty()) return Outcome::Err("BAD_ARG", "state 名が必要です");

    const scene::AnimationState* state = nullptr;
    for (const auto& candidate : animator->states)
        if (candidate.name == stateName) { state = &candidate; break; }
    if (state == nullptr)
        return Outcome::Err("STATE_NOT_FOUND", "Animator state が見つかりません: " + stateName);
    if (state->mode == scene::AnimationStateMode::Clip)
        return Outcome::Err("NOT_BLEND_TREE", "このステートは BlendTree ではありません (mode=clip): " + stateName);

    // 直近フレームの clipName→weight を引けるようにする (Motion への逆引き用)。
    auto runtimeWeight = [&](const std::string& clipName) -> const float* {
        for (const auto& entry : animator->currentBlendWeights)
            if (entry.first == clipName) return &entry.second;
        return nullptr;
    };

    auto motionToJson = [&](const scene::BlendTreeMotion& motion, bool is2D) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("sourcePath", JsonValue(motion.sourcePath));
        entry.Set("clipName", JsonValue(motion.clipName));
        entry.Set("clipIndex", JsonValue(motion.clipIndex));
        if (is2D) {
            entry.Set("posX", JsonValue(motion.posX));
            entry.Set("posY", JsonValue(motion.posY));
        } else {
            entry.Set("threshold", JsonValue(motion.threshold));
        }
        entry.Set("speed", JsonValue(motion.speed));
        entry.Set("ikWeight", JsonValue(motion.ikWeight));
        if (const float* weight = runtimeWeight(motion.clipName))
            entry.Set("runtimeWeight", JsonValue(*weight));
        return entry;
    };

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("state", JsonValue(stateName));
    result.Set("mode", JsonValue(AnimationStateModeName(state->mode)));
    result.Set("isCurrent", JsonValue(stateName == animator->currentStateName));

    JsonValue motions = JsonValue::MakeArray();
    if (state->mode == scene::AnimationStateMode::BlendTree1D) {
        const auto& tree = state->blendTree1D;
        result.Set("parameter", JsonValue(tree.paramName));
        result.Set("parameterValue", JsonValue(animator->GetFloat(tree.paramName)));
        result.Set("dampTime", JsonValue(tree.dampTime));
        result.Set("syncNormalizedTime", JsonValue(tree.syncNormalizedTime));
        for (const auto& motion : tree.motions) motions.Push(motionToJson(motion, false));
    } else {
        const auto& tree = state->blendTree2D;
        result.Set("parameterX", JsonValue(tree.paramX));
        result.Set("parameterY", JsonValue(tree.paramY));
        result.Set("parameterValueX", JsonValue(animator->GetFloat(tree.paramX)));
        result.Set("parameterValueY", JsonValue(animator->GetFloat(tree.paramY)));
        result.Set("blendType",
            JsonValue(tree.type == scene::BlendTree2DType::SimpleDirectional
                ? "simpleDirectional" : "freeformCartesian"));
        for (const auto& motion : tree.motions) motions.Push(motionToJson(motion, true));
    }
    result.Set("motions", std::move(motions));
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
        if (auto* vfx = go.GetComponent<scene::VFXGraphComponent>(); vfx != nullptr && !vfx->graphPath.empty()) {
            fs::path graphPath;
            std::string relative;
            if (!ResolveProjectFile(ctx, vfx->graphPath, graphPath, relative) || !fs::is_regular_file(graphPath)) {
                AddValidationIssue(issues, "error", "BROKEN_VFX_GRAPH", go,
                    "VFX Graph が見つかりません: " + vfx->graphPath);
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

Outcome DoProfilerSnapshot(editor::EditorContext& ctx, const JsonValue& payload)
{
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 20, 1, 100);
    const auto& records = profiler::Profiler::GetLastFrameRecords();
    std::vector<const profiler::ProfileRecord*> sorted;
    sorted.reserve(records.size());
    double frameMs = 0.0;
    for (const auto& record : records) {
        sorted.push_back(&record);
        if (record.depth == 0) frameMs += record.elapsedMs;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->elapsedMs > right->elapsedMs;
    });
    JsonValue samples = JsonValue::MakeArray();
    for (int index = 0; index < std::min(limit, static_cast<int>(sorted.size())); ++index) {
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("name", JsonValue(sorted[index]->name));
        sample.Set("category", JsonValue(sorted[index]->category));
        sample.Set("elapsedMs", JsonValue(sorted[index]->elapsedMs));
        sample.Set("depth", JsonValue(static_cast<int>(sorted[index]->depth)));
        samples.Push(std::move(sample));
    }
    const auto& rendering = renderer::RenderDebugOverlay::GetLastSnapshot();
    JsonValue result = JsonValue::MakeObject();
    result.Set("profilerEnabled", JsonValue(profiler::Profiler::IsEnabled()));
    result.Set("frameIndex", JsonValue(static_cast<std::int64_t>(profiler::Profiler::GetLastFrameIndex())));
    const double actualFrameMs = static_cast<double>(Time::unscaledDeltaTime) * 1000.0;
    result.Set("frameMs", JsonValue(actualFrameMs));
    result.Set("fps", JsonValue(actualFrameMs > 0.0 ? 1000.0 / actualFrameMs : 0.0));
    result.Set("cpuProfiledMs", JsonValue(frameMs));
    result.Set("drawCalls", JsonValue(rendering.renderStats.drawCalls));
    result.Set("triangles", JsonValue(rendering.renderStats.triangleCount));
    result.Set("vertices", JsonValue(rendering.renderStats.vertexCount));
    result.Set("totalObjects", JsonValue(rendering.renderStats.totalObjects));
    result.Set("frustumCulled", JsonValue(rendering.renderStats.frustumCulled));
    result.Set("occlusionCulled", JsonValue(rendering.renderStats.occlusionCulled));
    result.Set("visibleObjects", JsonValue(rendering.renderStats.totalObjects
        - rendering.renderStats.frustumCulled - rendering.renderStats.occlusionCulled));
    result.Set("topSamples", std::move(samples));
    JsonValue cpuPasses = JsonValue::MakeArray();
    for (const auto& [name, elapsedMs] : rendering.passTimings) {
        JsonValue pass = JsonValue::MakeObject();
        pass.Set("name", JsonValue(name));
        pass.Set("elapsedMs", JsonValue(elapsedMs));
        cpuPasses.Push(std::move(pass));
    }
    JsonValue gpuPasses = JsonValue::MakeArray();
    for (const auto& [name, elapsedMs] : rendering.gpuPassTimings) {
        JsonValue pass = JsonValue::MakeObject();
        pass.Set("name", JsonValue(name));
        pass.Set("elapsedMs", JsonValue(elapsedMs));
        gpuPasses.Push(std::move(pass));
    }
    result.Set("cpuRenderPasses", std::move(cpuPasses));
    result.Set("gpuRenderPasses", std::move(gpuPasses));
    if (ctx.memorySystem != nullptr) {
        const core::MemoryStats memory = ctx.memorySystem->GetTracker().GetTotalStats();
        JsonValue memoryJson = JsonValue::MakeObject();
        memoryJson.Set("used", JsonValue(static_cast<std::int64_t>(memory.used)));
        memoryJson.Set("peakUsed", JsonValue(static_cast<std::int64_t>(memory.peakUsed)));
        memoryJson.Set("activeAllocations", JsonValue(static_cast<std::int64_t>(memory.activeCount)));
        result.Set("memory", std::move(memoryJson));
    }
    return Outcome::Ok(std::move(result));
}

int VirtualKeyFromName(std::string name)
{
    name = LowerAscii(std::move(name));
    if (name.size() == 1) {
        const unsigned char character = static_cast<unsigned char>(name[0]);
        if (std::isalnum(character)) return static_cast<int>(std::toupper(character));
    }
    static const std::unordered_map<std::string, int> keys = {
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "escape", VK_ESCAPE },
        { "backspace", VK_BACK }, { "shift", VK_SHIFT }, { "ctrl", VK_CONTROL }, { "alt", VK_MENU },
        { "left", VK_LEFT }, { "right", VK_RIGHT }, { "up", VK_UP }, { "down", VK_DOWN },
        { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 },
        { "f5", VK_F5 }, { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 },
        { "f9", VK_F9 }, { "f10", VK_F10 }, { "f11", VK_F11 }, { "f12", VK_F12 }
    };
    const auto iterator = keys.find(name);
    return iterator != keys.end() ? iterator->second : -1;
}

// Animator ステートマシンの構造編集を Undo 可能な ICommand にまとめる共通ヘルパー。
// WHY: add/set-state・transition・condition・motion のどれも「states + anyStateTransitions +
//      parameters を丸ごとスナップショットして復元」で正しく戻せる (clip 実体を含まない軽量ベクトル)。
//      実行時に NodeId から再解決し、edit() で実変更を行う。復元後はランタイム遷移状態を消し、
//      消えたステートを指したまま再生が続くのを防ぐ。
std::unique_ptr<ICommand> MakeAnimatorEditCommand(
    scene::Scene* scene, std::string id, const char* description,
    std::function<void()> markDirty,
    std::function<void(scene::AnimatorComponent&)> edit)
{
    auto beforeStates = std::make_shared<std::vector<scene::AnimationState>>();
    auto beforeAny = std::make_shared<std::vector<scene::AnimationTransition>>();
    auto beforeParams = std::make_shared<std::vector<scene::AnimatorParameter>>();
    // layers もスナップショット対象に含める。
    // WHY: レイヤーは独自のステートマシン・マスク参照・加算設定を持つ編集対象になった。
    //      ここに含めないと、レイヤー操作だけ Undo が効かない歯抜けになる。
    auto beforeLayers = std::make_shared<std::vector<scene::AnimationLayer>>();
    return std::make_unique<LambdaCommand>(description,
        [scene, id, edit, beforeStates, beforeAny, beforeParams, beforeLayers, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            *beforeStates = a->states;
            *beforeAny = a->anyStateTransitions;
            *beforeParams = a->parameters;
            *beforeLayers = a->layers;
            edit(*a);
            markDirty();
        },
        [scene, id, beforeStates, beforeAny, beforeParams, beforeLayers, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            a->states = *beforeStates;
            a->anyStateTransitions = *beforeAny;
            a->parameters = *beforeParams;
            a->layers = *beforeLayers;
            a->currentStateName.clear();
            a->blendToState.clear();
            a->stateTime = 0.0f;
            a->blendWeight = 0.0f;
            markDirty();
        });
}

// ステート編集の対象グラフ (Base Layer か、指定レイヤーの独自ステートマシンか) を解決する。
// WHY: animation_add_state 等はすべて「どのグラフに対する操作か」だけが違い、中身は同じ。
//      レイヤー名の解決をここへ集約し、各ハンドラは返ってきた配列を触るだけにする。
struct AnimatorGraphTarget {
    std::vector<scene::AnimationState>*      states       = nullptr;
    std::vector<scene::AnimationTransition>* anyState     = nullptr;
    std::string*                             defaultState = nullptr;
};

AnimatorGraphTarget ResolveGraphTarget(scene::AnimatorComponent& animator,
                                       const std::string& layerName)
{
    if (layerName.empty())
        return { &animator.states, &animator.anyStateTransitions, &animator.defaultStateName };
    scene::AnimationLayer* layer = animator.FindLayer(layerName);
    if (!layer) return {};
    return { &layer->states, &layer->anyStateTransitions, &layer->defaultStateName };
}

// Undo/Redo ラムダ内から使う、レイヤー解決つきのグラフ参照。
// WHY: ラムダは実行時に GameObject を引き直すため、対象レイヤーもその場で解決し直す必要がある。
//      実行時点でレイヤーが消えていた場合は Base Layer へフォールバックする。
//      そこで落ちるより、無害な対象に落として Undo スタックを壊さない方が安全。
struct AnimatorGraphRefs {
    scene::AnimatorComponent& animator;
    const std::string&        layerName;

    [[nodiscard]] std::vector<scene::AnimationState>& StatesRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName)) return l->states;
        return animator.states;
    }
    [[nodiscard]] std::vector<scene::AnimationTransition>& AnyRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName))
                return l->anyStateTransitions;
        return animator.anyStateTransitions;
    }
    [[nodiscard]] std::string& DefaultRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName))
                return l->defaultStateName;
        return animator.defaultStateName;
    }
};

AnimatorGraphRefs GRAPH(scene::AnimatorComponent& animator, const std::string& layerName)
{
    return AnimatorGraphRefs{ animator, layerName };
}

// AnimationStateModeName の逆変換。未知文字列は false。
bool ParseAnimationStateMode(const std::string& name, scene::AnimationStateMode& out)
{
    if (name == "clip")        { out = scene::AnimationStateMode::Clip;        return true; }
    if (name == "blendTree1D") { out = scene::AnimationStateMode::BlendTree1D; return true; }
    if (name == "blendTree2D") { out = scene::AnimationStateMode::BlendTree2D; return true; }
    return false;
}

// BlendTree2D の座標解釈方式を文字列から解く。未知文字列は false。
bool ParseBlendTree2DType(const std::string& name, scene::BlendTree2DType& out)
{
    if (name == "simpleDirectional") { out = scene::BlendTree2DType::SimpleDirectional; return true; }
    if (name == "freeformCartesian") { out = scene::BlendTree2DType::FreeformCartesian; return true; }
    return false;
}

// 指定ステートを新名にリネームし、全遷移参照・defaultState・ランタイムステート名を追従させる。
void RenameAnimatorState(scene::AnimatorComponent& animator,
                         const std::string& oldName, const std::string& newName)
{
    for (auto& state : animator.states) {
        if (state.name == oldName) state.name = newName;
        for (auto& transition : state.transitions)
            if (transition.toStateName == oldName) transition.toStateName = newName;
    }
    for (auto& transition : animator.anyStateTransitions)
        if (transition.toStateName == oldName) transition.toStateName = newName;
    if (animator.defaultStateName == oldName) animator.defaultStateName = newName;
    if (animator.currentStateName == oldName) animator.currentStateName = newName;
    if (animator.blendToState == oldName) animator.blendToState = newName;
}

std::optional<asset::VFXNodeType> ParseVFXNodeType(std::string value)
{
    value = LowerAscii(value);
    if (value == "particle") return asset::VFXNodeType::Particle;
    if (value == "trail") return asset::VFXNodeType::Trail;
    if (value == "meshtrail" || value == "mesh trail") return asset::VFXNodeType::MeshTrail;
    if (value == "light") return asset::VFXNodeType::Light;
    if (value == "audio") return asset::VFXNodeType::Audio;
    if (value == "decal") return asset::VFXNodeType::Decal;
    if (value == "delay") return asset::VFXNodeType::Delay;
    if (value == "subgraph" || value == "sub graph") return asset::VFXNodeType::SubGraph;
    if (value == "forcefield" || value == "force field") return asset::VFXNodeType::ForceField;
    if (value == "mesh") return asset::VFXNodeType::Mesh;
    if (value == "screeneffect" || value == "screen effect") return asset::VFXNodeType::ScreenEffect;
    if (value == "camerashake" || value == "camera shake") return asset::VFXNodeType::CameraShake;
    if (value == "timescale" || value == "time scale") return asset::VFXNodeType::TimeScale;
    if (value == "wind") return asset::VFXNodeType::Wind;
    if (value == "reroute") return asset::VFXNodeType::Reroute;
    if (value == "animatedmesh" || value == "animated mesh")
        return asset::VFXNodeType::AnimatedMesh;
    return std::nullopt;
}


// Curve / Gradient のキー配列を JSON から読む。
// 受け付ける形:
//   キー配列そのもの         [[t,v], ...]            / [[t,r,g,b,a], ...]
//   補間モード付きオブジェクト { "interp": 0|1|2, "keys": [...] }
//   プリセット名             { "preset": "Spike", "scale": 1.0 }   ※ Curve のみ
// WHY: AI に 8 キーぶんの生の数値を書かせると、意図した形になったかを画像からしか
//      確認できず反復が長くなる。名前付きプリセットを一次面に置き、生キーは
//      「プリセットから微調整する」ための逃げ道として残す。
bool JsonToParticleCurve(const JsonValue& json, scene::ParticleCurve& output)
{
    if (const std::string preset = json.IsObject() ? StringField(json, "preset") : std::string{};
        !preset.empty()) {
        const asset::ParticleCurvePreset* found = asset::FindParticleCurvePreset(preset);
        if (found == nullptr) return false;
        const JsonValue* scale = json.Find("scale");
        asset::ApplyParticleCurvePreset(output, *found,
            scale != nullptr && scale->IsNumber() ? static_cast<float>(scale->AsNumber()) : 1.0f);
        return true;
    }
    const JsonValue* keys = json.IsArray() ? &json : json.Find("keys");
    if (keys == nullptr || !keys->IsArray() || keys->AsArray().size() < 2) return false;
    if (json.IsObject()) {
        if (const JsonValue* interp = json.Find("interp"); interp != nullptr && interp->IsNumber())
            output.interpolation = static_cast<scene::ParticleCurveInterpolation>(
                std::clamp(interp->AsInt(), 0,
                           static_cast<int>(scene::ParticleCurveInterpolation::Smooth)));
    }
    output.keyCount = static_cast<std::uint32_t>(
        (std::min)(keys->AsArray().size(), output.keys.size()));
    for (std::uint32_t index = 0; index < output.keyCount; ++index) {
        const auto& key = keys->AsArray()[index];
        if (!key.IsArray() || key.AsArray().size() < 2
            || !key.AsArray()[0].IsNumber() || !key.AsArray()[1].IsNumber()) return false;
        output.keys[index] = { static_cast<float>(key.AsArray()[0].AsNumber()),
                               static_cast<float>(key.AsArray()[1].AsNumber()) };
    }
    return true;
}

bool JsonToParticleGradient(const JsonValue& json, scene::ParticleGradient& output)
{
    const JsonValue* keys = json.IsArray() ? &json : json.Find("keys");
    if (keys == nullptr || !keys->IsArray() || keys->AsArray().size() < 2) return false;
    if (json.IsObject()) {
        if (const JsonValue* interp = json.Find("interp"); interp != nullptr && interp->IsNumber())
            output.interpolation = static_cast<scene::ParticleCurveInterpolation>(
                std::clamp(interp->AsInt(), 0,
                           static_cast<int>(scene::ParticleCurveInterpolation::Smooth)));
    }
    output.keyCount = static_cast<std::uint32_t>(
        (std::min)(keys->AsArray().size(), output.keys.size()));
    for (std::uint32_t index = 0; index < output.keyCount; ++index) {
        const auto& key = keys->AsArray()[index];
        if (!key.IsArray() || key.AsArray().size() < 5) return false;
        for (const auto& item : key.AsArray()) if (!item.IsNumber()) return false;
        output.keys[index] = {
            static_cast<float>(key.AsArray()[0].AsNumber()),
            { static_cast<float>(key.AsArray()[1].AsNumber()),
              static_cast<float>(key.AsArray()[2].AsNumber()),
              static_cast<float>(key.AsArray()[3].AsNumber()),
              static_cast<float>(key.AsArray()[4].AsNumber()) }
        };
    }
    return true;
}

bool JsonToSchemaValue(const JsonValue& json, reflection::PropertyType type, std::any& output)
{
    // Curve / Gradient は 8 キー化と補間モード追加まで setField の対象外だった。
    // そのため「スキーマには見えるのに AI からは一切編集できないフィールド」になっており、
    // 時間曲線 (エフェクトの質を最も左右する要素) を AI が触れなかった。
    if (type == reflection::PropertyType::Curve) {
        scene::ParticleCurve curve;
        if (!JsonToParticleCurve(json, curve)) return false;
        output = curve;
        return true;
    }
    if (type == reflection::PropertyType::Gradient) {
        scene::ParticleGradient gradient;
        if (!JsonToParticleGradient(json, gradient)) return false;
        output = gradient;
        return true;
    }
    if (type == reflection::PropertyType::Float && json.IsNumber()) output = static_cast<float>(json.AsNumber());
    else if (type == reflection::PropertyType::Int && json.IsNumber()) output = json.AsInt();
    // Enum は int で受け渡す (MakeEnumProperty 側が有効域へ clamp する)。
    // WHY: ここを塞がないと、スキーマへ enum を載せても setField が常に BAD_ARG で弾かれ、
    //      「スキーマには見えるのに AI から変更できないフィールド」が生まれる。
    else if (type == reflection::PropertyType::Enum && json.IsNumber()) output = json.AsInt();
    else if (type == reflection::PropertyType::Bool && json.IsBool()) output = json.AsBool();
    else if ((type == reflection::PropertyType::String || type == reflection::PropertyType::AssetRef)
             && json.IsString()) output = json.AsString();
    else if ((type == reflection::PropertyType::Vector3 || type == reflection::PropertyType::Color)
             && json.IsArray()) {
        const auto& values = json.AsArray();
        if (type == reflection::PropertyType::Vector3 && values.size() == 3
            && values[0].IsNumber() && values[1].IsNumber() && values[2].IsNumber())
            output = math::Vector3{ static_cast<float>(values[0].AsNumber()),
                                    static_cast<float>(values[1].AsNumber()),
                                    static_cast<float>(values[2].AsNumber()) };
        else if (type == reflection::PropertyType::Color && values.size() == 4
                 && std::all_of(values.begin(), values.end(), [](const JsonValue& item) { return item.IsNumber(); }))
            output = math::Vector4{ static_cast<float>(values[0].AsNumber()),
                                    static_cast<float>(values[1].AsNumber()),
                                    static_cast<float>(values[2].AsNumber()),
                                    static_cast<float>(values[3].AsNumber()) };
    }
    return output.has_value();
}

bool JsonToVFXParamValue(const JsonValue& json, asset::VFXParamType type, asset::VFXParamValue& output)
{
    if (json.IsObject()) {
        const std::string source = LowerAscii(StringField(json, "source"));
        if (source == "constant") {
            const JsonValue* constant = json.Find("value");
            return constant != nullptr && JsonToVFXParamValue(*constant, type, output);
        }
        if (source == "attribute" || source == "attributeref") {
            const std::string path = StringField(json, "path");
            if (path.empty()) return false;
            output.source = asset::VFXAttributeRef{ path };
            return true;
        }
        if (source == "signal") {
            const std::string name = StringField(json, "name");
            if (name.empty()) return false;
            output.source = asset::VFXSignalRef{ name };
            return true;
        }
        if ((source == "random" || source == "randomrange")
            && (type == asset::VFXParamType::Float || type == asset::VFXParamType::Int)) {
            const JsonValue* minimum = json.Find("minimum");
            const JsonValue* maximum = json.Find("maximum");
            if (minimum == nullptr || maximum == nullptr || !minimum->IsNumber() || !maximum->IsNumber()) return false;
            output.source = asset::VFXRandomRange{ static_cast<float>(minimum->AsNumber()),
                                                   static_cast<float>(maximum->AsNumber()) };
            return true;
        }
        if (source == "curve" && type == asset::VFXParamType::Float) {
            const JsonValue* keys = json.Find("keys");
            if (keys == nullptr || !keys->IsArray() || keys->AsArray().empty()) return false;
            asset::VFXCurveSource curve;
            curve.curve.keyCount = static_cast<std::uint32_t>(
                (std::min)(keys->AsArray().size(), curve.curve.keys.size()));
            for (std::uint32_t index = 0; index < curve.curve.keyCount; ++index) {
                const auto& key = keys->AsArray()[index];
                if (!key.IsArray() || key.AsArray().size() < 2
                    || !key.AsArray()[0].IsNumber() || !key.AsArray()[1].IsNumber()) return false;
                curve.curve.keys[index] = { static_cast<float>(key.AsArray()[0].AsNumber()),
                                            static_cast<float>(key.AsArray()[1].AsNumber()) };
            }
            output.source = curve;
            return true;
        }
        if (source == "gradient" && type == asset::VFXParamType::Color) {
            const JsonValue* keys = json.Find("keys");
            if (keys == nullptr || !keys->IsArray() || keys->AsArray().empty()) return false;
            asset::VFXGradientSource gradient;
            gradient.gradient.keyCount = static_cast<std::uint32_t>(
                (std::min)(keys->AsArray().size(), gradient.gradient.keys.size()));
            for (std::uint32_t index = 0; index < gradient.gradient.keyCount; ++index) {
                const auto& key = keys->AsArray()[index];
                if (!key.IsArray() || key.AsArray().size() < 5) return false;
                for (const auto& item : key.AsArray()) if (!item.IsNumber()) return false;
                gradient.gradient.keys[index] = {
                    static_cast<float>(key.AsArray()[0].AsNumber()),
                    { static_cast<float>(key.AsArray()[1].AsNumber()), static_cast<float>(key.AsArray()[2].AsNumber()),
                      static_cast<float>(key.AsArray()[3].AsNumber()), static_cast<float>(key.AsArray()[4].AsNumber()) }
                };
            }
            output.source = gradient;
            return true;
        }
    }
    std::any value;
    reflection::PropertyType propertyType = reflection::PropertyType::Float;
    if (type == asset::VFXParamType::Int) propertyType = reflection::PropertyType::Int;
    else if (type == asset::VFXParamType::Bool) propertyType = reflection::PropertyType::Bool;
    else if (type == asset::VFXParamType::Color) propertyType = reflection::PropertyType::Color;
    else if (type == asset::VFXParamType::Vector3) propertyType = reflection::PropertyType::Vector3;
    else if (type == asset::VFXParamType::AssetRef) propertyType = reflection::PropertyType::AssetRef;
    if (!JsonToSchemaValue(json, propertyType, value)) return false;
    if (const auto* item = std::any_cast<float>(&value)) output.source = asset::VFXConstant{ *item };
    else if (const auto* item = std::any_cast<int>(&value)) output.source = asset::VFXConstant{ *item };
    else if (const auto* item = std::any_cast<bool>(&value)) output.source = asset::VFXConstant{ *item };
    else if (const auto* item = std::any_cast<math::Vector4>(&value)) output.source = asset::VFXConstant{ *item };
    else if (const auto* item = std::any_cast<math::Vector3>(&value)) output.source = asset::VFXConstant{ *item };
    else if (const auto* item = std::any_cast<std::string>(&value)) output.source = asset::VFXConstant{ *item };
    else return false;
    return true;
}

// ── Behavior Tree の編集 Command ────────────────────────────────────────────
// WHY VFX と同じ「アセット丸ごとスナップショット」方式にするか:
//     木は最大でも数十ノードで、丸ごと持っても軽い。差分 Undo は
//     「親を付け替えたら order も変わる」ような連動を取りこぼしやすい。
std::unique_ptr<ICommand> BuildBehaviorTreeCommand(editor::EditorContext& ctx,
                                                    const std::string& type,
                                                    const JsonValue& payload,
                                                    Outcome& err)
{
    namespace fs = std::filesystem;
    fs::path absolute;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), absolute, relative)
        || !fs::is_regular_file(absolute)) {
        err = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return nullptr;
    }
    fbzz::ai::BehaviorTreeAsset oldTree;
    std::string error;
    if (!fbzz::ai::ParseBehaviorTreeAsset(absolute.generic_string(), oldTree, &error)) {
        err = Outcome::Err("BT_PARSE_FAILED", error);
        return nullptr;
    }
    fbzz::ai::EnsureReservedBlackboardKeys(oldTree);
    fbzz::ai::BehaviorTreeAsset newTree = oldTree;

    // 子の数を数える。子数上限の検査に使う。
    const auto childCount = [&newTree](int parentId) {
        return static_cast<int>(std::count_if(newTree.nodes.begin(), newTree.nodes.end(),
            [parentId](const fbzz::ai::BTNodeDef& node) { return node.parentId == parentId; }));
    };
    // childId が ancestorId の子孫か。循環の作成を防ぐ。
    const auto isDescendant = [&newTree](int ancestorId, int childId) {
        int current = childId;
        for (std::size_t guard = 0; guard <= newTree.nodes.size() && current != 0; ++guard) {
            if (current == ancestorId) return true;
            const fbzz::ai::BTNodeDef* node = newTree.FindNode(current);
            if (node == nullptr) return false;
            current = node->parentId;
        }
        return false;
    };
    // 親へ繋げてよいかを 1 か所で判定する。Editor の TryReparent と同じ規則で、
    // 「AI からは繋げるが人間の UI では弾かれる」食い違いを作らない。
    const auto reparent = [&](int childId, int parentId) -> std::string {
        fbzz::ai::BTNodeDef* child = newTree.FindNode(childId);
        if (child == nullptr) return "ノードが見つかりません";
        if (childId == parentId) return "自分自身を親にはできません";
        if (parentId != 0) {
            const fbzz::ai::BTNodeDef* parent = newTree.FindNode(parentId);
            if (parent == nullptr) return "親ノードが見つかりません";
            if (isDescendant(childId, parentId)) return "自分の子孫を親にはできません (循環します)";
            const int maxChildren = fbzz::ai::BTNodeMaxChildren(parent->type);
            if (maxChildren == 0)
                return std::string(fbzz::ai::BTNodeTypeName(parent->type)) + " は葉ノードなので子を持てません";
            if (maxChildren > 0 && child->parentId != parentId && childCount(parentId) >= maxChildren)
                return std::string(fbzz::ai::BTNodeTypeName(parent->type)) + " が持てる子は "
                     + std::to_string(maxChildren) + " 個までです";
        } else {
            for (const auto& node : newTree.nodes)
                if (node.parentId == 0 && node.id != childId) return "ルートは 1 つだけです";
        }
        child->parentId = parentId;
        int nextOrder = 0;
        for (const auto& node : newTree.nodes)
            if (node.parentId == parentId && node.id != childId)
                nextOrder = (std::max)(nextOrder, node.order + 1);
        child->order = nextOrder;
        return {};
    };

    if (type == "bt.node.add") {
        const std::string typeName = StringField(payload, "nodeType");
        int found = -1;
        for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index)
            if (typeName == fbzz::ai::BTNodeTypeName(static_cast<fbzz::ai::BTNodeType>(index))) found = index;
        if (found < 0) { err = Outcome::Err("BAD_ARG", "未知の BT nodeType です: " + typeName); return nullptr; }

        fbzz::ai::BTNodeDef node;
        node.id = newTree.nextNodeId++;
        node.type = static_cast<fbzz::ai::BTNodeType>(found);
        node.name = StringField(payload, "name");
        if (node.name.empty()) node.name = fbzz::ai::BTNodeTypeName(node.type);
        const bool hasRoot = std::any_of(newTree.nodes.begin(), newTree.nodes.end(),
            [](const fbzz::ai::BTNodeDef& item) { return item.parentId == 0; });
        const JsonValue* parentValue = payload.Find("parentId");
        const int requestedParent = parentValue != nullptr ? parentValue->AsInt() : 0;
        node.parentId = 0;
        newTree.nodes.push_back(node);
        if (hasRoot) {
            if (requestedParent == 0) {
                err = Outcome::Err("BT_ROOT_EXISTS",
                                   "ルートは既にあります。parentId を指定してください");
                return nullptr;
            }
            const std::string reason = reparent(node.id, requestedParent);
            if (!reason.empty()) { err = Outcome::Err("BT_REPARENT_REJECTED", reason); return nullptr; }
        }
    } else if (type == "bt.node.remove") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        if (newTree.FindNode(nodeId) == nullptr) {
            err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr;
        }
        // 子孫ごと消す。親だけ消して子が浮くと、残された枝の意味が判らなくなる。
        std::vector<editor::GraphEdge> edges;
        for (const auto& node : newTree.nodes)
            if (node.parentId != 0) edges.push_back({ node.parentId, node.id });
        const std::vector<int> doomed =
            editor::CollectReachable(std::vector<int>{ nodeId }, edges);
        const std::unordered_set<int> doomedSet(doomed.begin(), doomed.end());
        std::erase_if(newTree.nodes, [&doomedSet](const fbzz::ai::BTNodeDef& node) {
            return doomedSet.contains(node.id);
        });
    } else if (type == "bt.node.setParent") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const int parentId = payload.Find("parentId") != nullptr ? payload.Find("parentId")->AsInt() : 0;
        const std::string reason = reparent(nodeId, parentId);
        if (!reason.empty()) { err = Outcome::Err("BT_REPARENT_REJECTED", reason); return nullptr; }
    } else if (type == "bt.node.setOrder") {
        fbzz::ai::BTNodeDef* node = newTree.FindNode(
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0);
        if (node == nullptr) { err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr; }
        if (payload.Find("order") == nullptr) { err = Outcome::Err("BAD_ARG", "order が必要です"); return nullptr; }
        node->order = payload.Find("order")->AsInt();
    } else if (type == "bt.node.setField") {
        fbzz::ai::BTNodeDef* node = newTree.FindNode(
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0);
        if (node == nullptr) { err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr; }
        const std::string field = StringField(payload, "field");
        const JsonValue* value = payload.Find("value");
        if (field.empty() || value == nullptr) {
            err = Outcome::Err("BAD_ARG", "field と value が必要です"); return nullptr;
        }
        const auto asFloat = [value]() { return static_cast<float>(value->AsNumber()); };
        if (field == "name") node->name = value->AsString();
        else if (field == "duration") node->duration = asFloat();
        else if (field == "durationRandom") node->durationRandom = asFloat();
        else if (field == "repeatCount") node->repeatCount = value->AsInt();
        else if (field == "repeatUntilFailure") node->repeatUntilFailure = value->AsBool();
        else if (field == "range") node->range = asFloat();
        else if (field == "threshold01") node->threshold01 = asFloat();
        else if (field == "acceptanceRadius") node->acceptanceRadius = asFloat();
        else if (field == "chaseEntity") node->chaseEntity = value->AsBool();
        else if (field == "repathInterval") node->repathInterval = asFloat();
        else if (field == "turnSpeedDeg") node->turnSpeedDeg = asFloat();
        else if (field == "keyName") node->keyName = value->AsString();
        else if (field == "moveTargetKey") node->moveTargetKey = value->AsString();
        else if (field == "animatorTrigger") node->animatorTrigger = value->AsString();
        else if (field == "waitForAnimation") node->waitForAnimation = value->AsBool();
        else if (field == "scriptMethod") node->scriptMethod = value->AsString();
        else if (field == "soundPath") node->soundPath = value->AsString();
        else if (field == "volume") node->volume = asFloat();
        else if (field == "withinSeconds") node->withinSeconds = asFloat();
        else if (field == "valueBool") node->valueBool = value->AsBool();
        else if (field == "valueInt") node->valueInt = value->AsInt();
        else if (field == "valueFloat") node->valueFloat = asFloat();
        else if (field == "valueString") node->valueString = value->AsString();
        else if (field == "editorX") node->editorX = asFloat();
        else if (field == "editorY") node->editorY = asFloat();
        else if (field == "abortMode") {
            // 純粋条件以外へ付けると Validate が保存を拒否する。理由を先に返す。
            const std::string mode = LowerAscii(value->AsString());
            const bool canAbort = fbzz::ai::BTNodeIsPureCondition(node->type)
                               || node->type == fbzz::ai::BTNodeType::BlackboardCondition;
            if (!canAbort && mode != "none") {
                err = Outcome::Err("BT_ABORT_NOT_ALLOWED",
                    std::string(fbzz::ai::BTNodeTypeName(node->type))
                    + " は副作用を持つため abortMode を設定できません "
                      "(中断チェックのたびに世界が変わり木が非決定的になります)");
                return nullptr;
            }
            if (mode == "none") node->abortMode = fbzz::ai::AbortMode::None;
            else if (mode == "self") node->abortMode = fbzz::ai::AbortMode::Self;
            else if (mode == "lowerpriority") node->abortMode = fbzz::ai::AbortMode::LowerPriority;
            else if (mode == "both") node->abortMode = fbzz::ai::AbortMode::Both;
            else { err = Outcome::Err("BAD_ARG", "abortMode は none/self/lowerPriority/both"); return nullptr; }
        } else if (field == "compareOp") {
            const std::string op = value->AsString();
            const char* names[] = { "==", "!=", "<", "<=", ">", ">=" };
            int found = -1;
            for (int index = 0; index < 6; ++index) if (op == names[index]) found = index;
            if (found < 0) { err = Outcome::Err("BAD_ARG", "compareOp は == != < <= > >="); return nullptr; }
            node->compareOp = static_cast<fbzz::ai::BTCompareOp>(found);
        } else {
            err = Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field);
            return nullptr;
        }
        // Blackboard キーは実在するものだけ。存在しない名前は保存も Compile も通り、
        // 実行時に黙って無視されるという最も気づきにくい壊れ方をする。
        if ((field == "keyName" || field == "moveTargetKey") && !value->AsString().empty()) {
            const std::string key = value->AsString();
            const bool known = std::any_of(newTree.blackboard.begin(), newTree.blackboard.end(),
                [&key](const fbzz::ai::BlackboardDef& def) { return def.name == key; });
            if (!known) {
                err = Outcome::Err("BT_UNKNOWN_KEY",
                    "Blackboard に存在しないキーです: " + key
                    + " (bt.tree の blackboard を確認するか bt.blackboard.add で先に作ってください)");
                return nullptr;
            }
        }
    } else if (type == "bt.blackboard.add") {
        const std::string name = StringField(payload, "name");
        if (name.empty()) { err = Outcome::Err("BAD_ARG", "name が必要です"); return nullptr; }
        if (std::any_of(newTree.blackboard.begin(), newTree.blackboard.end(),
                [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; })) {
            err = Outcome::Err("BT_KEY_EXISTS", "同名のキーが既にあります: " + name);
            return nullptr;
        }
        fbzz::ai::BlackboardDef def;
        def.name = name;
        const std::string typeName = LowerAscii(StringField(payload, "type"));
        if (typeName == "int") def.type = fbzz::ai::BlackboardType::Int;
        else if (typeName == "float") def.type = fbzz::ai::BlackboardType::Float;
        else if (typeName == "vector3") def.type = fbzz::ai::BlackboardType::Vector3;
        else if (typeName == "entity") def.type = fbzz::ai::BlackboardType::Entity;
        else if (typeName == "string") def.type = fbzz::ai::BlackboardType::String;
        else def.type = fbzz::ai::BlackboardType::Bool;
        newTree.blackboard.push_back(std::move(def));
    } else if (type == "bt.blackboard.remove") {
        const std::string name = StringField(payload, "name");
        const auto found = std::find_if(newTree.blackboard.begin(), newTree.blackboard.end(),
            [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; });
        if (found == newTree.blackboard.end()) {
            err = Outcome::Err("BT_KEY_NOT_FOUND", "キーが見つかりません: " + name); return nullptr;
        }
        if (found->reserved) {
            err = Outcome::Err("BT_KEY_RESERVED",
                "予約キーは削除できません (固定添字で PerceptionSystem 等が書き込みます): " + name);
            return nullptr;
        }
        newTree.blackboard.erase(found);
    } else if (type == "bt.autoLayout") {
        std::vector<int> nodeIds;
        std::vector<editor::GraphLayoutEdge> edges;
        for (const auto& node : newTree.nodes) {
            nodeIds.push_back(node.id);
            if (node.parentId != 0) edges.push_back({ node.parentId, node.id });
        }
        const std::vector<int> roots = newTree.FindRootIds();
        editor::GraphLayoutOptions options;
        options.columnStep = 260.0f;
        options.rowStep = 150.0f;
        const auto layout = roots.empty()
            ? editor::ComputeGraphLayout(nodeIds, edges, options)
            : editor::ComputeGraphLayout(nodeIds, edges, roots, options);
        for (auto& node : newTree.nodes) {
            const auto found = layout.find(node.id);
            if (found == layout.end()) continue;
            node.editorX = found->second.x;
            node.editorY = found->second.y;
        }
    } else {
        err = Outcome::Err("UNKNOWN_COMMAND", "未対応の BT コマンドです: " + type);
        return nullptr;
    }

    // 保存前に検証する。壊れた木を書き出すと、次に開いたときに
    // 「AI が壊した」のか「元から壊れていた」のか区別できなくなる。
    std::string validateError;
    if (!fbzz::ai::ValidateBehaviorTreeAsset(newTree, &validateError)) {
        err = Outcome::Err("BT_INVALID", validateError);
        return nullptr;
    }

    editor::EditorContext* context = &ctx;
    const std::string target = absolute.generic_string();
    return std::make_unique<LambdaCommand>("AI: Edit Behavior Tree",
        [context, target, newTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, newTree)) context->requestAssetBrowserRefresh = true;
        },
        [context, target, oldTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, oldTree)) context->requestAssetBrowserRefresh = true;
        });
}

std::unique_ptr<ICommand> BuildVFXAssetCommand(editor::EditorContext& ctx,
                                                const std::string& type,
                                                const JsonValue& payload,
                                                Outcome& err)
{
    const std::string path = StringField(payload, "path");
    asset::VFXGraphAsset oldGraph;
    std::string error;
    if (path.empty() || !asset::LoadVFXGraphAsset(path, oldGraph, &error)) {
        err = Outcome::Err("VFX_NOT_FOUND", error.empty() ? "有効な.vfx pathが必要です" : error);
        return nullptr;
    }
    asset::VFXGraphAsset newGraph = oldGraph;
    if (type == "vfx.graph.set") {
        if (payload.Find("name") == nullptr && payload.Find("maxParticles") == nullptr
            && payload.Find("maxLights") == nullptr && payload.Find("maxAudioVoices") == nullptr) {
            err = Outcome::Err("BAD_ARG", "変更するGraph設定が必要です"); return nullptr;
        }
        if (const std::string name = StringField(payload, "name"); !name.empty()) newGraph.name = name;
        if (const JsonValue* value = payload.Find("maxParticles"); value != nullptr)
            newGraph.maxParticles = value->AsInt();
        if (const JsonValue* value = payload.Find("maxLights"); value != nullptr)
            newGraph.maxLights = value->AsInt();
        if (const JsonValue* value = payload.Find("maxAudioVoices"); value != nullptr)
            newGraph.maxAudioVoices = value->AsInt();
    } else if (type == "vfx.node.add") {
        const auto nodeType = ParseVFXNodeType(StringField(payload, "nodeType"));
        if (!nodeType.has_value()) { err = Outcome::Err("BAD_ARG", "未知のVFX nodeTypeです"); return nullptr; }
        int nextId = 1;
        for (const auto& node : newGraph.nodes) nextId = (std::max)(nextId, node.id + 1);
        asset::VFXGraphNode node;
        node.id = nextId;
        node.type = *nodeType;
        node.name = StringField(payload, "name");
        if (node.name.empty()) node.name = asset::VFXNodeTypeName(*nodeType);
        node.editorX = 260.0f + static_cast<float>((newGraph.nodes.size() % 3) * 220);
        node.editorY = 80.0f + static_cast<float>((newGraph.nodes.size() / 3) * 170);
        node.duration = *nodeType == asset::VFXNodeType::Delay ? 0.25f
            : (*nodeType == asset::VFXNodeType::Particle ? node.particle.duration : 1.0f);
        if (*nodeType == asset::VFXNodeType::SubGraph)
            node.subGraph.graphPath = StringField(payload, "assetPath");
        newGraph.nodes.push_back(std::move(node));
        int from = 0;
        if (const JsonValue* value = payload.Find("from"); value != nullptr && value->IsNumber()) from = value->AsInt();
        if (from == 0) {
            const auto entry = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
                [](const asset::VFXGraphNode& item) { return item.type == asset::VFXNodeType::Entry; });
            if (entry != newGraph.nodes.end()) from = entry->id;
        }
        if (from > 0) newGraph.links.push_back({ from, nextId, asset::VFXLinkTrigger::OnStart, 0.0f });
    } else if (type == "vfx.node.duplicate") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const auto source = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        if (source == newGraph.nodes.end() || source->type == asset::VFXNodeType::Entry) {
            err = Outcome::Err("BAD_ARG", "複製可能なnodeIdが必要です"); return nullptr;
        }
        int nextId = 1;
        for (const auto& node : newGraph.nodes) nextId = (std::max)(nextId, node.id + 1);
        asset::VFXGraphNode duplicate = *source;
        duplicate.id = nextId;
        const std::string requestedName = StringField(payload, "name");
        duplicate.name = requestedName.empty() ? duplicate.name + " Copy" : requestedName;
        duplicate.editorX = payload.Find("editorX") != nullptr
            ? static_cast<float>(payload.Find("editorX")->AsNumber()) : duplicate.editorX + 40.0f;
        duplicate.editorY = payload.Find("editorY") != nullptr
            ? static_cast<float>(payload.Find("editorY")->AsNumber()) : duplicate.editorY + 40.0f;
        newGraph.nodes.push_back(std::move(duplicate));
    } else if (type == "vfx.node.remove") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const auto node = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        if (node == newGraph.nodes.end() || node->type == asset::VFXNodeType::Entry) {
            err = Outcome::Err("BAD_ARG", "削除可能なnodeIdが必要です"); return nullptr;
        }
        std::erase_if(newGraph.nodes, [nodeId](const auto& item) { return item.id == nodeId; });
        std::erase_if(newGraph.links, [nodeId](const auto& item) {
            return item.fromNode == nodeId || item.toNode == nodeId;
        });
        std::erase_if(newGraph.bindings, [nodeId](const auto& item) { return item.nodeId == nodeId; });
        for (auto& remaining : newGraph.nodes)
            if (remaining.parentNodeId == nodeId) remaining.parentNodeId = -1;
    } else if (type == "vfx.node.setEnabled") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const JsonValue* enabled = payload.Find("enabled");
        auto node = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        if (node == newGraph.nodes.end() || node->type == asset::VFXNodeType::Entry
            || enabled == nullptr || !enabled->IsBool()) {
            err = Outcome::Err("BAD_ARG", "Entry以外のnodeIdとenabledが必要です"); return nullptr;
        }
        node->enabled = enabled->AsBool();
    } else if (type == "vfx.node.setMetadata") {
        if (payload.Find("name") == nullptr && payload.Find("editorX") == nullptr
            && payload.Find("editorY") == nullptr) {
            err = Outcome::Err("BAD_ARG", "変更するnode metadataが必要です"); return nullptr;
        }
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        auto node = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        if (node == newGraph.nodes.end()) {
            err = Outcome::Err("BAD_ARG", "nodeId が存在しません"); return nullptr;
        }
        if (const std::string name = StringField(payload, "name"); !name.empty()) node->name = name;
        if (const JsonValue* value = payload.Find("editorX"); value != nullptr)
            node->editorX = static_cast<float>(value->AsNumber());
        if (const JsonValue* value = payload.Find("editorY"); value != nullptr)
            node->editorY = static_cast<float>(value->AsNumber());
    } else if (type == "vfx.node.setParent") {
        // 空間の親子付け。link (実行の因果) とは別軸なので専用コマンドにする。
        // WHY: schemaPath 経由 (vfx.node.setField "parentNodeId") でも書けてしまうが、
        //      それだと「循環」「実体を持たない親」を保存直前の一般検証でしか弾けず、
        //      AI は失敗理由から何を直せばいいのか判断できない。ここで意味のある
        //      エラーコードを返し、1 往復で正しい操作へ導く。
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        // parentNodeId 省略 / -1 で親を外す (owner 直下へ戻す)。
        const int parentId = payload.Find("parentNodeId") != nullptr
            ? payload.Find("parentNodeId")->AsInt() : -1;
        auto node = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        if (node == newGraph.nodes.end()) {
            err = Outcome::Err("BAD_ARG", "nodeId が存在しません"); return nullptr;
        }
        if (node->type == asset::VFXNodeType::Entry || node->type == asset::VFXNodeType::Delay) {
            err = Outcome::Err("NODE_HAS_NO_TRANSFORM",
                               "Entry / Delay は空間上の実体を持たないため親を持てません");
            return nullptr;
        }
        if (parentId != -1) {
            if (parentId == nodeId) {
                err = Outcome::Err("PARENT_SELF", "自分自身を親にはできません"); return nullptr;
            }
            const auto parent = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
                [parentId](const asset::VFXGraphNode& item) { return item.id == parentId; });
            if (parent == newGraph.nodes.end()) {
                err = Outcome::Err("BAD_ARG", "parentNodeId が存在しません"); return nullptr;
            }
            if (parent->type == asset::VFXNodeType::Entry
                || parent->type == asset::VFXNodeType::Delay) {
                err = Outcome::Err("PARENT_HAS_NO_TRANSFORM",
                                   "Entry / Delay は実体を持たないため親にできません。"
                                   "実体を持つノード (Particle / Mesh / Light など) を指定してください");
                return nullptr;
            }
            // 自分の子孫を親にすると循環する。保存時ではなくここで具体的に弾く。
            for (int cursor = parent->parentNodeId, guard = 0;
                 cursor != -1 && guard <= static_cast<int>(newGraph.nodes.size()); ++guard) {
                if (cursor == nodeId) {
                    err = Outcome::Err("PARENT_CYCLE",
                                       "指定した親はこのノードの子孫のため循環します");
                    return nullptr;
                }
                const auto up = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
                    [cursor](const asset::VFXGraphNode& item) { return item.id == cursor; });
                cursor = (up != newGraph.nodes.end()) ? up->parentNodeId : -1;
            }
            // attachBone との併用は拒否しない (socket 追従は正しい設定でありうる)。
            // 実行時は socket が優先されて parentNodeId が無視されるが、それは
            // CollectVFXGraphWarnings が PARENT_OVERRIDDEN_BY_SOCKET として報告し、
            // vfx.lint 経由で AI へ届く。ここで別経路の通知を作ると二重管理になる。
        }
        node->parentNodeId = parentId;
    } else if (type == "vfx.node.setField") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const std::string schemaPath = StringField(payload, "schemaPath");
        const JsonValue* input = payload.Find("value");
        auto node = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
            [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
        reflection::ResolvedProperty resolved;
        if (node == newGraph.nodes.end() || input == nullptr
            || !reflection::ResolveProperty(asset::GetVFXNodeSchema(), &*node, schemaPath, resolved)
            || resolved.property == nullptr) {
            err = Outcome::Err("UNKNOWN_FIELD", "schemaPathを解決できません"); return nullptr;
        }
        std::any value;
        if (!JsonToSchemaValue(*input, resolved.property->type, value)
            || !resolved.property->set(resolved.owner, value)) {
            err = Outcome::Err("TYPE_MISMATCH", "schemaPathの型とvalueが一致しません"); return nullptr;
        }
    } else if (type == "vfx.repair") {
        // vfx.lint が指摘する機械的な不備を自動で直す。
        // WHY: AI は lint → 修正 → lint を回すが、「Entry へ繋ぐ」「パスの綴りを直す」は
        //      毎回同じ手順なので往復させる意味がない。判断の要らないものだけ自動化し、
        //      設計判断が要るもの (何を出すか) は AI に残す。
        const bool connectOrphans = payload.Find("connectOrphans") == nullptr
            || payload.Find("connectOrphans")->AsBool();
        const bool fixAssets = payload.Find("fixAssets") == nullptr
            || payload.Find("fixAssets")->AsBool();
        // 以下は「見た目が壊れる設定」の機械的修復。lint の警告 code と 1:1 で対応する。
        // WHY: これらは直し方が一意に決まる (せん断は等方へ戻す以外に無い、
        //      saturate される値は範囲内へ落とす以外に無い) ので、AI に往復させる意味がない。
        //      逆に「何を出すか」のような設計判断が要るものはここに入れない。
        const bool fixSprites = payload.Find("fixSprites") == nullptr
            || payload.Find("fixSprites")->AsBool();
        const bool fixLighting = payload.Find("fixLighting") == nullptr
            || payload.Find("fixLighting")->AsBool();
        const bool fixSorting = payload.Find("fixSorting") == nullptr
            || payload.Find("fixSorting")->AsBool();
        const bool fixMeshFade = payload.Find("fixMeshFade") == nullptr
            || payload.Find("fixMeshFade")->AsBool();
        const bool fixParents = payload.Find("fixParents") == nullptr
            || payload.Find("fixParents")->AsBool();

        for (auto& node : newGraph.nodes) {
            if (node.type == asset::VFXNodeType::Particle) {
                auto& particle = node.particle;
                // SHEARED_SPRITE — 回転と非等方サイズは併用できない。
                // 回転を残して軸倍率を落とす (回転は動きの質に効き、軸倍率は形にしか効かないため、
                // 意図せず併用してしまった場合に失うものが小さいのは軸倍率の側)。
                const bool spins = particle.angularVelocityMin != 0.0f
                                || particle.angularVelocityMax != 0.0f || particle.useRotationCurve;
                if (fixSprites && spins
                    && (particle.sizeAxisScale.x != particle.sizeAxisScale.y))
                    particle.sizeAxisScale = { 1.0f, 1.0f, 1.0f };
                // LIGHTING_SATURATED — 1.0 超はシェーダー側で丸められ、意味を持たない。
                if (fixLighting && particle.sixWayLighting && particle.lightingStrength > 1.0f)
                    particle.lightingStrength = 0.8f;
                // ALPHA_NO_SORT — 半透明の重なりは描画順で結果が変わる。
                if (fixSorting && particle.blendMode != scene::ParticleBlendMode::Additive
                    && particle.sortMode == scene::ParticleSortMode::None
                    && particle.maxParticles > 1)
                    particle.sortMode = scene::ParticleSortMode::BackToFront;
            }
            // MESH_NO_FADE — 加算ブレンドは RGB が 0 になって初めて消える。
            if (fixMeshFade && node.type == asset::VFXNodeType::Mesh) {
                node.mesh.colorEnd.x = 0.0f;
                node.mesh.colorEnd.y = 0.0f;
                node.mesh.colorEnd.z = 0.0f;
            }
        }
        // PARENT_HAS_NO_TRANSFORM — 実体を持たない親は実行時に無視される。
        // 位置の意図までは復元できないので、親指定を外して「効いていない設定」を消すに留める。
        if (fixParents) {
            for (auto& node : newGraph.nodes) {
                if (node.parentNodeId == -1) continue;
                const auto parent = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
                    [&node](const asset::VFXGraphNode& item) { return item.id == node.parentNodeId; });
                if (parent == newGraph.nodes.end()
                    || parent->type == asset::VFXNodeType::Entry
                    || parent->type == asset::VFXNodeType::Delay)
                    node.parentNodeId = -1;
            }
        }

        if (connectOrphans) {
            const auto entry = std::find_if(newGraph.nodes.begin(), newGraph.nodes.end(),
                [](const asset::VFXGraphNode& node) { return node.type == asset::VFXNodeType::Entry; });
            if (entry != newGraph.nodes.end()) {
                const int entryId = entry->id;
                std::vector<int> reachable{ entryId };
                for (std::size_t head = 0; head < reachable.size(); ++head) {
                    for (const auto& link : newGraph.links) {
                        if (link.fromNode != reachable[head]) continue;
                        if (std::find(reachable.begin(), reachable.end(), link.toNode) == reachable.end())
                            reachable.push_back(link.toNode);
                    }
                }
                for (const auto& node : newGraph.nodes) {
                    if (node.type == asset::VFXNodeType::Entry) continue;
                    if (std::find(reachable.begin(), reachable.end(), node.id) != reachable.end()) continue;
                    // 孤立ノードは Entry から OnStart で繋ぐ。スケジュールが破綻するなら戻す。
                    newGraph.links.push_back({ entryId, node.id, asset::VFXLinkTrigger::OnStart, 0.0f });
                    std::vector<float> starts;
                    float duration = 0.0f;
                    if (!asset::BuildVFXGraphSchedule(newGraph, starts, duration, nullptr))
                        newGraph.links.pop_back();
                }
            }
        }

        if (fixAssets && !ctx.projectRoot.empty()) {
            namespace fs = std::filesystem;
            // 参照先が無いパスを、同じ拡張子のファイルの中からファイル名の近さで置き換える。
            // 綴り違い・フォルダ移動を拾うのが目的なので、確信度が低いものは触らない。
            const auto repairPath = [&](std::string& value) {
                if (value.empty() || value.rfind("primitive:", 0) == 0) return;
                std::error_code ec;
                if (fs::is_regular_file(fs::path(ctx.projectRoot) / value, ec)) return;
                ec.clear();
                if (!ctx.engineRoot.empty()
                    && fs::is_regular_file(fs::path(ctx.engineRoot) / value, ec)) return;
                ec.clear();
                const std::string wanted = LowerAscii(fs::path(value).filename().string());
                const std::string extension = LowerAscii(fs::path(value).extension().string());
                std::string best;
                std::size_t bestScore = 0;
                for (const fs::path& root : { fs::path(ctx.projectRoot), fs::path(ctx.engineRoot) }) {
                    if (root.empty() || !fs::is_directory(root, ec)) { ec.clear(); continue; }
                    for (const auto& file : fs::recursive_directory_iterator(
                             root, fs::directory_options::skip_permission_denied, ec)) {
                        if (!file.is_regular_file(ec)) continue;
                        if (LowerAscii(file.path().extension().string()) != extension) continue;
                        const std::string candidate = LowerAscii(file.path().filename().string());
                        // 先頭からの一致文字数を素点にする。完全一致が最優先。
                        std::size_t score = 0;
                        while (score < candidate.size() && score < wanted.size()
                               && candidate[score] == wanted[score]) ++score;
                        if (candidate == wanted) score = wanted.size() + 100;
                        if (score <= bestScore) continue;
                        bestScore = score;
                        best = fs::relative(file.path(), root, ec).generic_string();
                        ec.clear();
                    }
                    ec.clear();
                }
                // ファイル名の半分以上が一致したものだけ採用する (無関係な置換を避ける)。
                if (!best.empty() && bestScore >= wanted.size() / 2 + 1) value = best;
            };
            for (auto& node : newGraph.nodes) {
                repairPath(node.particle.texturePath);
                repairPath(node.particle.materialPath);
                repairPath(node.particle.meshShapePath);
                repairPath(node.trail.texturePath);
                repairPath(node.trail.materialPath);
                repairPath(node.trail.meshPath);
                repairPath(node.audio.clipPath);
                repairPath(node.decal.albedoPath);
                repairPath(node.mesh.materialPath);
                if (node.mesh.meshPath.rfind("primitive:", 0) != 0) repairPath(node.mesh.meshPath);
                repairPath(node.subGraph.graphPath);
            }
        }
    } else if (type == "vfx.link.add") {
        const int from = payload.Find("from") != nullptr ? payload.Find("from")->AsInt() : 0;
        const int to = payload.Find("to") != nullptr ? payload.Find("to")->AsInt() : 0;
        asset::VFXLinkTrigger trigger = asset::VFXLinkTrigger::OnComplete;
        const std::string triggerName = LowerAscii(StringField(payload, "trigger"));
        if (triggerName == "onstart") trigger = asset::VFXLinkTrigger::OnStart;
        else if (triggerName == "oncollision") trigger = asset::VFXLinkTrigger::OnCollision;
    else if (triggerName == "ondeath") trigger = asset::VFXLinkTrigger::OnDeath;
    else if (triggerName == "onanimationevent")
        trigger = asset::VFXLinkTrigger::OnAnimationEvent;
    else if (triggerName == "ontrigger")
        trigger = asset::VFXLinkTrigger::OnTrigger;
        const float delay = payload.Find("delay") != nullptr
            ? static_cast<float>(payload.Find("delay")->AsNumber()) : 0.0f;
        newGraph.links.push_back({ from, to, trigger, delay });
    } else if (type == "vfx.link.update") {
        const int index = payload.Find("index") != nullptr ? payload.Find("index")->AsInt() : -1;
        if (index < 0 || index >= static_cast<int>(newGraph.links.size())) {
            err = Outcome::Err("BAD_ARG", "link indexが範囲外です"); return nullptr;
        }
        auto& link = newGraph.links[static_cast<std::size_t>(index)];
        if (const JsonValue* value = payload.Find("from"); value != nullptr) link.fromNode = value->AsInt();
        if (const JsonValue* value = payload.Find("to"); value != nullptr) link.toNode = value->AsInt();
        if (const JsonValue* value = payload.Find("delay"); value != nullptr)
            link.delay = static_cast<float>(value->AsNumber());
        if (payload.Find("trigger") != nullptr) {
            const std::string triggerName = LowerAscii(StringField(payload, "trigger"));
            if (triggerName == "onstart") link.trigger = asset::VFXLinkTrigger::OnStart;
            else if (triggerName == "oncollision") link.trigger = asset::VFXLinkTrigger::OnCollision;
    else if (triggerName == "ondeath") link.trigger = asset::VFXLinkTrigger::OnDeath;
    else if (triggerName == "onanimationevent")
        link.trigger = asset::VFXLinkTrigger::OnAnimationEvent;
    else if (triggerName == "ontrigger")
        link.trigger = asset::VFXLinkTrigger::OnTrigger;
            else link.trigger = asset::VFXLinkTrigger::OnComplete;
        }
    } else if (type == "vfx.link.remove") {
        const int index = payload.Find("index") != nullptr ? payload.Find("index")->AsInt() : -1;
        if (index < 0 || index >= static_cast<int>(newGraph.links.size())) {
            err = Outcome::Err("BAD_ARG", "link indexが範囲外です"); return nullptr;
        }
        newGraph.links.erase(newGraph.links.begin() + index);
    } else if (type == "vfx.param.declare") {
        const std::string name = StringField(payload, "name");
        const std::string typeName = LowerAscii(StringField(payload, "paramType"));
        asset::VFXParamType parameterType = asset::VFXParamType::Float;
        if (typeName == "int") parameterType = asset::VFXParamType::Int;
        else if (typeName == "bool") parameterType = asset::VFXParamType::Bool;
        else if (typeName == "color") parameterType = asset::VFXParamType::Color;
        else if (typeName == "vector3") parameterType = asset::VFXParamType::Vector3;
        else if (typeName == "asset") parameterType = asset::VFXParamType::AssetRef;
        asset::VFXParamDefinition definition;
        definition.name = name;
        definition.type = parameterType;
        const JsonValue* defaultValue = payload.Find("defaultValue");
        if (defaultValue == nullptr || !JsonToVFXParamValue(*defaultValue, parameterType, definition.defaultValue)) {
            err = Outcome::Err("TYPE_MISMATCH", "defaultValueがparamTypeと一致しません"); return nullptr;
        }
        if (const JsonValue* minimum = payload.Find("minimum"); minimum != nullptr && minimum->IsNumber()) {
            definition.minimum = static_cast<float>(minimum->AsNumber()); definition.hasRange = true;
        }
        if (const JsonValue* maximum = payload.Find("maximum"); maximum != nullptr && maximum->IsNumber()) {
            definition.maximum = static_cast<float>(maximum->AsNumber()); definition.hasRange = true;
        }
        newGraph.parameters.push_back(std::move(definition));
    } else if (type == "vfx.param.remove") {
        const std::string name = StringField(payload, "name");
        const auto parameter = std::find_if(newGraph.parameters.begin(), newGraph.parameters.end(),
            [&](const auto& item) { return item.name == name; });
        if (parameter == newGraph.parameters.end()) {
            err = Outcome::Err("BAD_ARG", "公開パラメーターが存在しません"); return nullptr;
        }
        std::erase_if(newGraph.parameters, [&](const auto& item) { return item.name == name; });
        std::erase_if(newGraph.bindings, [&](const auto& item) { return item.paramName == name; });
        std::erase_if(newGraph.subGraphForwards, [&](const auto& item) { return item.parentParam == name; });
        for (auto& variant : newGraph.variants)
            std::erase_if(variant.overrides, [&](const auto& item) { return item.paramName == name; });
    } else if (type == "vfx.param.bind") {
        newGraph.bindings.push_back({ StringField(payload, "name"),
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0,
            StringField(payload, "schemaPath") });
    } else if (type == "vfx.param.unbind") {
        const std::string name = StringField(payload, "name");
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : -1;
        const std::string schemaPath = StringField(payload, "schemaPath");
        const std::size_t beforeCount = newGraph.bindings.size();
        std::erase_if(newGraph.bindings, [&](const auto& item) {
            return item.paramName == name && (nodeId < 0 || item.nodeId == nodeId)
                && (schemaPath.empty() || item.schemaPath == schemaPath);
        });
        if (beforeCount == newGraph.bindings.size()) {
            err = Outcome::Err("BAD_ARG", "一致するbindingが存在しません"); return nullptr;
        }
    } else if (type == "vfx.param.setDefault") {
        const std::string name = StringField(payload, "name");
        auto parameter = std::find_if(newGraph.parameters.begin(), newGraph.parameters.end(),
            [&](const auto& item) { return item.name == name; });
        const JsonValue* input = payload.Find("value");
        if (parameter == newGraph.parameters.end() || input == nullptr
            || !JsonToVFXParamValue(*input, parameter->type, parameter->defaultValue)) {
            err = Outcome::Err("TYPE_MISMATCH", "parameterまたはvalueが不正です"); return nullptr;
        }
    } else if (type == "vfx.optimize") {
        // 最適化の戦略。
        //
        // WHY: 粒子数を一律に下げるだけの最適化は、原因が fill rate のときに効きが悪く、
        //      見た目だけが痩せる。パーティクルのコストは粒子数ではなく「塗った画素数」で決まるため、
        //      枚数を減らしても 1 枚あたりが大きいままなら塗る量はほとんど変わらない。
        //      AAA で実際に使う手は「粒を大きくして枚数を減らす」(総塗り面積を下げつつ密度感を保つ) と
        //      「遠距離でレイヤーを間引く」(引きの絵では層の枚数が読めないことを利用する) の 2 つ。
        //      どちらが要るかは vfx.runtime の cost.overdraw と particlePassGpuMs を見て決める。
        const std::string strategy = LowerAscii(StringField(payload, "strategy"));
        const bool cutParticles = strategy.empty() || strategy == "particles" || strategy == "both";
        const bool cutFillRate  = strategy == "fillrate" || strategy == "both";
        if (!strategy.empty() && !cutParticles && !cutFillRate) {
            err = Outcome::Err("BAD_ARG", "strategy は particles / fillRate / both のいずれかです");
            return nullptr;
        }
        const int targetParticles = payload.Find("targetParticles") != nullptr
            ? payload.Find("targetParticles")->AsInt() : 0;
        if (cutParticles && targetParticles < 1) {
            err = Outcome::Err("BAD_ARG", "targetParticlesは1以上です"); return nullptr;
        }
        const auto oldBudget = asset::CalculateVFXGraphBudget(newGraph);
        const float scale = (cutParticles && oldBudget.particles > 0)
            ? (std::min)(1.0f, static_cast<float>(targetParticles) / oldBudget.particles) : 1.0f;

        // 「粒を大きくして枚数を減らす」の倍率。1 粒あたりの面積 s² が k^(2/3) 倍、
        // 枚数が 1/k 倍になるので、総塗り面積は k^(1/3) 分の 1 まで落ちる。
        // size を √k 倍にすると総面積が変わらず fill rate 対策にならないので、意図的に √より弱くする。
        const float sizeCompensation = scale < 1.0f
            ? std::pow(1.0f / (std::max)(scale, 0.01f), 1.0f / 3.0f) : 1.0f;

        std::unordered_set<int> eventSources;
        for (const auto& link : newGraph.links)
            if (link.trigger == asset::VFXLinkTrigger::OnCollision || link.trigger == asset::VFXLinkTrigger::OnDeath)
                eventSources.insert(link.fromNode);

        // 遠距離で間引く候補を決めるため、各ノードの塗り寄与 (枚数 × 面積) を先に見積もる。
        // WHY: 全ノードを一律に間引くと、遠景で「芯だけが残って形が判る」状態も壊れる。
        //      塗りの大半を作っている層だけを落とすのが、見た目の劣化が最も小さい。
        std::vector<std::pair<int, float>> fillContribution;
        for (const auto& node : newGraph.nodes) {
            if (node.type != asset::VFXNodeType::Particle) continue;
            const float size = (std::max)(node.particle.sizeStart, node.particle.sizeEnd);
            fillContribution.emplace_back(node.id, static_cast<float>(node.particle.maxParticles) * size * size);
        }
        std::sort(fillContribution.begin(), fillContribution.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        // 上位 1/3 (最低 1 個) を「遠距離で間引く層」とする。層が 2 枚以下なら間引かない。
        std::unordered_set<int> thinAtDistance;
        if (cutFillRate && fillContribution.size() >= 3) {
            const std::size_t count =
                (std::max)(std::size_t{1}, fillContribution.size() / std::size_t{3});
            for (std::size_t index = 0; index < count; ++index)
                thinAtDistance.insert(fillContribution[index].first);
        }

        for (auto& node : newGraph.nodes) {
            if (node.type != asset::VFXNodeType::Particle) continue;
            if (cutParticles) {
                node.particle.maxParticles =
                    (std::max)(1, static_cast<int>(std::round(node.particle.maxParticles * scale)));
                node.particle.emitRate *= scale;
            }
            if (cutFillRate) {
                if (cutParticles) {
                    // 枚数が減った分の密度感を粒の大きさで補う。総塗り面積は下がったまま。
                    node.particle.sizeStart *= sizeCompensation;
                    node.particle.sizeEnd   *= sizeCompensation;
                }
                // 遠距離でのレイヤー間引き。寄与の大きい層だけを完全に落とす。
                if (thinAtDistance.contains(node.id)) {
                    node.particle.lodFarRateScale = 0.0f;
                    node.particle.lodFarDistance = (std::min)(node.particle.lodFarDistance, 25.0f);
                }
            }
            node.particle.lodEnabled = true;
            node.particle.lodNearRateScale = 1.0f;
            node.particle.lodFarRateScale = (std::min)(node.particle.lodFarRateScale, 0.35f);
            // GPU へ載せられるものは載せる。載らない設定を持つノードは触らない
            // (ここで無理に Gpu を立てると、黙って CPU へ縮退したまま「GPU 化した」と読める)。
            scene::ParticleEmitter candidate = node.particle;
            candidate.simulationMode = scene::ParticleSimulationMode::Gpu;
            if (!eventSources.contains(node.id) && scene::CanUseGpuSimulation(candidate))
                node.particle.simulationMode = scene::ParticleSimulationMode::Gpu;
        }
        newGraph.maxParticles = cutParticles
            ? (std::max)(targetParticles, asset::CalculateVFXGraphBudget(newGraph).particles)
            : newGraph.maxParticles;
    } else if (type == "vfx.variant.upsert") {
        const std::string name = StringField(payload, "name");
        const JsonValue* values = payload.Find("values");
        if (name.empty() || values == nullptr || !values->IsObject()) {
            err = Outcome::Err("BAD_ARG", "name / values objectが必要です"); return nullptr;
        }
        asset::VFXVariantSet variant;
        variant.name = name;
        for (const auto& [paramName, json] : values->AsObject()) {
            const auto definition = std::find_if(newGraph.parameters.begin(), newGraph.parameters.end(),
                [&](const auto& parameter) { return parameter.name == paramName; });
            asset::VFXParamValue value;
            if (definition == newGraph.parameters.end()
                || !JsonToVFXParamValue(json, definition->type, value)) {
                err = Outcome::Err("TYPE_MISMATCH", "variant値が公開パラメーターと一致しません: " + paramName);
                return nullptr;
            }
            variant.overrides.push_back({ paramName, std::move(value) });
        }
        auto existing = std::find_if(newGraph.variants.begin(), newGraph.variants.end(),
            [&](const auto& item) { return item.name == name; });
        if (existing == newGraph.variants.end()) newGraph.variants.push_back(std::move(variant));
        else *existing = std::move(variant);
    } else if (type == "vfx.variant.remove") {
        const std::string name = StringField(payload, "name");
        const std::size_t beforeCount = newGraph.variants.size();
        std::erase_if(newGraph.variants, [&](const auto& item) { return item.name == name; });
        if (beforeCount == newGraph.variants.size()) {
            err = Outcome::Err("BAD_ARG", "Variantが存在しません"); return nullptr;
        }
    } else if (type == "vfx.group.add" || type == "vfx.group.update") {
        asset::VFXGraphGroup* group = nullptr;
        if (type == "vfx.group.add") {
            int nextId = 1;
            for (const auto& item : newGraph.groups) nextId = (std::max)(nextId, item.id + 1);
            newGraph.groups.push_back({});
            newGraph.groups.back().id = nextId;
            group = &newGraph.groups.back();
        } else {
            if (payload.Find("title") == nullptr && payload.Find("note") == nullptr
                && payload.Find("x") == nullptr && payload.Find("y") == nullptr
                && payload.Find("width") == nullptr && payload.Find("height") == nullptr
                && payload.Find("color") == nullptr) {
                err = Outcome::Err("BAD_ARG", "変更するGroup設定が必要です"); return nullptr;
            }
            const int groupId = payload.Find("groupId") != nullptr ? payload.Find("groupId")->AsInt() : 0;
            const auto found = std::find_if(newGraph.groups.begin(), newGraph.groups.end(),
                [groupId](const auto& item) { return item.id == groupId; });
            if (found == newGraph.groups.end()) {
                err = Outcome::Err("BAD_ARG", "groupIdが存在しません"); return nullptr;
            }
            group = &*found;
        }
        if (const std::string title = StringField(payload, "title"); !title.empty()) group->title = title;
        if (payload.Find("note") != nullptr) group->note = StringField(payload, "note");
        if (const JsonValue* value = payload.Find("x"); value != nullptr) group->x = static_cast<float>(value->AsNumber());
        if (const JsonValue* value = payload.Find("y"); value != nullptr) group->y = static_cast<float>(value->AsNumber());
        if (const JsonValue* value = payload.Find("width"); value != nullptr) group->width = static_cast<float>(value->AsNumber());
        if (const JsonValue* value = payload.Find("height"); value != nullptr) group->height = static_cast<float>(value->AsNumber());
        if (const JsonValue* value = payload.Find("color"); value != nullptr && value->IsArray()
            && value->AsArray().size() == 4) {
            group->color = { static_cast<float>(value->AsArray()[0].AsNumber()),
                             static_cast<float>(value->AsArray()[1].AsNumber()),
                             static_cast<float>(value->AsArray()[2].AsNumber()),
                             static_cast<float>(value->AsArray()[3].AsNumber()) };
        }
    } else if (type == "vfx.group.remove") {
        const int groupId = payload.Find("groupId") != nullptr ? payload.Find("groupId")->AsInt() : 0;
        const std::size_t beforeCount = newGraph.groups.size();
        std::erase_if(newGraph.groups, [groupId](const auto& item) { return item.id == groupId; });
        if (beforeCount == newGraph.groups.size()) {
            err = Outcome::Err("BAD_ARG", "groupIdが存在しません"); return nullptr;
        }
    } else return nullptr;

    if (!asset::ValidateVFXGraphAsset(newGraph, &error)) {
        err = Outcome::Err("VFX_VALIDATION", error);
        return nullptr;
    }
    editor::EditorContext* context = &ctx;
    std::string commandLabel = "AI: Edit VFX Graph";
    if (type == "vfx.graph.set") commandLabel = "AI: Set VFX Graph";
    else if (type == "vfx.node.add") commandLabel = "AI: Add VFX Node";
    else if (type == "vfx.node.duplicate") commandLabel = "AI: Duplicate VFX Node";
    else if (type == "vfx.node.remove") commandLabel = "AI: Remove VFX Node";
    else if (type == "vfx.node.setEnabled") commandLabel = "AI: Toggle VFX Node";
    else if (type == "vfx.node.setMetadata") commandLabel = "AI: Set VFX Node Metadata";
    else if (type == "vfx.node.setParent") commandLabel = "AI: Parent VFX Node";
    else if (type == "vfx.node.setField") commandLabel = "AI: Set VFX Node Field";
    else if (type == "vfx.link.add") commandLabel = "AI: Add VFX Link";
    else if (type == "vfx.link.update") commandLabel = "AI: Update VFX Link";
    else if (type == "vfx.link.remove") commandLabel = "AI: Remove VFX Link";
    else if (type == "vfx.param.declare") commandLabel = "AI: Declare VFX Parameter";
    else if (type == "vfx.param.remove") commandLabel = "AI: Remove VFX Parameter";
    else if (type == "vfx.param.bind") commandLabel = "AI: Bind VFX Parameter";
    else if (type == "vfx.param.unbind") commandLabel = "AI: Unbind VFX Parameter";
    else if (type == "vfx.param.setDefault") commandLabel = "AI: Set VFX Parameter Default";
    else if (type == "vfx.variant.upsert") commandLabel = "AI: Upsert VFX Variant";
    else if (type == "vfx.variant.remove") commandLabel = "AI: Remove VFX Variant";
    else if (type == "vfx.group.add") commandLabel = "AI: Add VFX Group";
    else if (type == "vfx.group.update") commandLabel = "AI: Update VFX Group";
    else if (type == "vfx.group.remove") commandLabel = "AI: Remove VFX Group";
    else if (type == "vfx.optimize") commandLabel = "AI: Optimize VFX";
    else if (type == "vfx.repair") commandLabel = "AI: Repair VFX";
    return std::make_unique<LambdaCommand>(std::move(commandLabel),
        [context, path, newGraph]() {
            if (asset::SaveVFXGraphAsset(path, newGraph)) context->requestAssetBrowserRefresh = true;
        },
        [context, path, oldGraph]() {
            if (asset::SaveVFXGraphAsset(path, oldGraph)) context->requestAssetBrowserRefresh = true;
        });
}

// 1つの mutating Command を Undo 可能な ICommand へ変換する (実行はしない)。失敗時 nullptr + err。
// createdSink != nullptr のとき生成系 Command は代表ルートの instanceId をそこへ書き込む。
// detailSink != nullptr のとき、Command が「適用の副作用」を応答へ載せたい場合にそこへ書く。
//   WHY: applied:true だけでは、Template 取り込みでパラメーターが改名されたことも
//        budget が引き上げられたことも AI へ伝わらない。次の手で存在しない名前を
//        指してしまうため、黙って起きる変更は必ず応答へ載せる。
std::unique_ptr<ICommand> BuildCommand(editor::EditorContext& ctx, const std::string& type,
                                       const JsonValue& payload, Outcome& err,
                                       std::shared_ptr<std::string> createdSink,
                                       JsonValue* detailSink = nullptr)
{
    if (type == "vfx.template.apply") {
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) { err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です"); return nullptr; }
        const std::string templateName = StringField(payload, "template");
        const std::string destination = StringField(payload, "path");
        if (templateName.empty() || destination.empty()) {
            err = Outcome::Err("BAD_ARG", "template / path が必要です"); return nullptr;
        }
        // Templates ディレクトリを走査して名前で解決する。
        // WHY: 以前は5種をハードコードした map だったため、プロジェクトへ Template を
        //      追加しても AI からは存在しないままだった。エディタ UI 側と同じ
        //      「ディスクが唯一の信頼元」という規則に揃える。
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
        const std::string lowerTemplate = LowerAscii(templateName);
        fs::path source;
        if (lowerTemplate.find('/') == std::string::npos
            && lowerTemplate.find('\\') == std::string::npos) {
            // 名前指定: Project > Engine の順に Templates を探し、拡張子なしの stem で照合する。
            std::vector<fs::path> roots{ root / "Assets/VFX/Templates" };
            if (!ctx.engineRoot.empty())
                roots.push_back(fs::path(ctx.engineRoot) / "Assets/VFX/Templates");
            for (const fs::path& directory : roots) {
                if (!fs::is_directory(directory, ec)) { ec.clear(); continue; }
                for (const fs::directory_entry& file : fs::directory_iterator(directory, ec)) {
                    if (!file.is_regular_file(ec) || file.path().extension() != ".vfx") continue;
                    if (LowerAscii(file.path().stem().string()) != lowerTemplate) continue;
                    source = file.path();
                    break;
                }
                ec.clear();
                if (!source.empty()) break;
            }
        }
        // パス直指定 (または名前解決に失敗) のときは projectRoot 相対として扱う。
        if (source.empty()) source = root / fs::path(templateName);
        source = fs::weakly_canonical(source, ec);
        const fs::path target = fs::weakly_canonical(root / fs::path(destination), ec);
        const fs::path engineRoot = ctx.engineRoot.empty() ? root
            : fs::weakly_canonical(fs::path(ctx.engineRoot), ec);
        const bool sourceAllowed = source.generic_string().rfind(root.generic_string(), 0) == 0
            || source.generic_string().rfind(engineRoot.generic_string(), 0) == 0;
        if (ec || !sourceAllowed || target.generic_string().rfind(root.generic_string(), 0) != 0
            || LowerAscii(target.extension().string()) != ".vfx") {
            err = Outcome::Err("BAD_PATH", "template と path はprojectRoot配下の.vfxである必要があります");
            return nullptr;
        }
        asset::VFXGraphAsset templateGraph;
        std::string loadError;
        if (!asset::LoadVFXGraphAsset(source.generic_string(), templateGraph, &loadError)) {
            err = Outcome::Err("VFX_TEMPLATE_NOT_FOUND", loadError); return nullptr;
        }
        const bool existed = fs::exists(target, ec);
        asset::VFXGraphAsset previous;
        if (existed && !asset::LoadVFXGraphAsset(target.generic_string(), previous, &loadError)) {
            err = Outcome::Err("VFX_DEST_INVALID", loadError); return nullptr;
        }

        // 取り込み方。人間の UI と同じ 3 択を AI へも開く。
        // WHY: これまでは「複製して新規アセット化」だけで、AI は既存グラフへ層を
        //      足せなかった。人間が Merge で 1 操作にできることを、AI は
        //      ノードを 1 個ずつ add してリンクを張るしかなく、失敗率が違いすぎる。
        const std::string modeText = LowerAscii(StringField(payload, "mode"));
        const bool merging = modeText == "merge";
        const bool subGraphMode = modeText == "subgraph";
        if (!modeText.empty() && !merging && !subGraphMode && modeText != "replace") {
            err = Outcome::Err("BAD_ARG", "mode は replace / merge / subgraph のいずれかです");
            return nullptr;
        }
        if ((merging || subGraphMode) && !existed) {
            err = Outcome::Err("VFX_DEST_NOT_FOUND",
                               "merge / subgraph は既存の .vfx を対象にします");
            return nullptr;
        }

        asset::VFXGraphAsset result;
        JsonValue reportJson = JsonValue::MakeObject();
        if (merging || subGraphMode) {
            result = previous;
            editor::vfx::TemplateMergeOptions options;
            if (const JsonValue* value = payload.Find("anchorNodeId"); value != nullptr)
                options.anchorNodeId = value->AsInt();
            if (const JsonValue* value = payload.Find("delay"); value != nullptr)
                options.anchorDelay = static_cast<float>(value->AsNumber());
            if (const JsonValue* value = payload.Find("parentNodeId"); value != nullptr)
                options.parentNodeId = value->AsInt();
            if (const JsonValue* value = payload.Find("raiseBudget"); value != nullptr)
                options.raiseBudget = value->AsBool();
            options.variantName = StringField(payload, "variant");
            const std::string triggerText = LowerAscii(StringField(payload, "trigger"));
            if (triggerText == "oncomplete") options.anchorTrigger = asset::VFXLinkTrigger::OnComplete;
            else if (triggerText == "onstart") options.anchorTrigger = asset::VFXLinkTrigger::OnStart;
            else if (triggerText == "oncollision") options.anchorTrigger = asset::VFXLinkTrigger::OnCollision;
            else if (triggerText == "ondeath") options.anchorTrigger = asset::VFXLinkTrigger::OnDeath;
            else if (!triggerText.empty()) {
                err = Outcome::Err("BAD_ARG",
                    "trigger は onComplete / onStart / onCollision / onDeath のいずれかです");
                return nullptr;
            }
            if (const JsonValue* groups = payload.Find("groups"); groups != nullptr && groups->IsArray())
                for (const JsonValue& item : groups->AsArray())
                    options.groupFilter.push_back(item.AsInt());

            const std::string label = source.stem().generic_string();
            if (subGraphMode) {
                // 複製せず参照として置く。Template を直すと参照元へ伝播する。
                const fs::path relativeSource = fs::relative(source, root, ec);
                if (ec) { err = Outcome::Err("BAD_PATH", "Template のパスを解決できません"); return nullptr; }
                if (fs::weakly_canonical(target, ec) == source) {
                    err = Outcome::Err("VFX_SELF_REFERENCE",
                                       "自分自身を Sub Graph として参照することはできません");
                    return nullptr;
                }
                int nextNodeId = 0;
                for (const auto& node : result.nodes) nextNodeId = (std::max)(nextNodeId, node.id);
                int anchorId = options.anchorNodeId;
                if (anchorId < 0)
                    for (const auto& node : result.nodes)
                        if (node.type == asset::VFXNodeType::Entry) anchorId = node.id;
                asset::VFXGraphNode node;
                node.id = nextNodeId + 1;
                node.type = asset::VFXNodeType::SubGraph;
                node.name = label;
                std::vector<float> starts;
                float duration = 0.0f;
                node.duration = asset::BuildVFXGraphSchedule(templateGraph, starts, duration, nullptr)
                                && duration > 0.0f ? duration : 1.0f;
                node.subGraph.graphPath = relativeSource.generic_string();
                node.parentNodeId = options.parentNodeId;
                node.editorX = 300.0f;
                node.editorY = 60.0f * static_cast<float>(result.nodes.size());
                result.nodes.push_back(std::move(node));
                if (anchorId > 0)
                    result.links.push_back({ anchorId, nextNodeId + 1, options.anchorTrigger,
                                             options.anchorDelay, {} });
                reportJson.Set("addedNodes", JsonValue(1));
            } else {
                editor::vfx::TemplateMergeReport report;
                std::string mergeError;
                if (!editor::vfx::MergeGraphTemplateInto(result, templateGraph, label, options,
                                                         report, &mergeError)) {
                    err = Outcome::Err("VFX_MERGE_FAILED", mergeError);
                    return nullptr;
                }
                // 改名・budget 引き上げ・不足素材は「黙って起きると困ること」。
                // 応答に載せないと、AI は次の手で存在しないパラメーター名を指す。
                reportJson.Set("addedNodes", JsonValue(static_cast<int>(report.addedNodes.size())));
                JsonValue renamed = JsonValue::MakeArray();
                for (const auto& [oldName, newName] : report.renamedParameters) {
                    JsonValue item = JsonValue::MakeObject();
                    item.Set("from", JsonValue(oldName));
                    item.Set("to", JsonValue(newName));
                    renamed.Push(std::move(item));
                }
                reportJson.Set("renamedParameters", std::move(renamed));
                JsonValue missing = JsonValue::MakeArray();
                for (const auto& path : report.missingAssets) missing.Push(JsonValue(path));
                reportJson.Set("missingAssets", std::move(missing));
                JsonValue budget = JsonValue::MakeObject();
                budget.Set("particles", JsonValue(report.budgetAfter[0]));
                budget.Set("lights", JsonValue(report.budgetAfter[1]));
                budget.Set("audioVoices", JsonValue(report.budgetAfter[2]));
                reportJson.Set("budget", std::move(budget));
            }
        } else {
            result = std::move(templateGraph);
            // Template の説明は Template のもの。複製先へそのまま持ち越すと、
            // 生成した全ての .vfx が同じ説明を持つカタログになる。
            result.description.clear();
            result.tags.clear();
        }
        const std::string requestedName = StringField(payload, "name");
        if (!requestedName.empty()) result.name = requestedName;
        else if (!merging && !subGraphMode) result.name = target.stem().generic_string();
        // 説明とタグは「説明」として持たせる。以前は用途を graph.name へ押し込んでいたため、
        // AI は graphName として読めるのに Editor のカタログはファイル名しか出せなかった。
        if (const JsonValue* value = payload.Find("description"); value != nullptr && value->IsString())
            result.description = value->AsString();
        if (const JsonValue* value = payload.Find("tags"); value != nullptr && value->IsArray()) {
            result.tags.clear();
            for (const JsonValue& item : value->AsArray())
                if (item.IsString() && !item.AsString().empty()) result.tags.push_back(item.AsString());
        }

        // Validate は保存時にも走るが、ここで落とせば「保存されたが壊れている」を避けられる。
        std::string validateError;
        if (!asset::ValidateVFXGraphAsset(result, &validateError)) {
            err = Outcome::Err("VFX_INVALID", validateError); return nullptr;
        }
        reportJson.Set("mode", JsonValue(subGraphMode ? std::string("subgraph")
                                       : merging ? std::string("merge") : std::string("replace")));
        if (detailSink != nullptr) *detailSink = std::move(reportJson);

        editor::EditorContext* context = &ctx;
        const std::string targetString = target.generic_string();
        return std::make_unique<LambdaCommand>("AI: Apply VFX Template",
            [context, targetString, result]() {
                std::error_code createError;
                std::filesystem::create_directories(std::filesystem::path(targetString).parent_path(), createError);
                if (asset::SaveVFXGraphAsset(targetString, result)) context->requestAssetBrowserRefresh = true;
            },
            [context, targetString, existed, previous]() {
                if (existed) (void)asset::SaveVFXGraphAsset(targetString, previous);
                else { std::error_code removeError; std::filesystem::remove(targetString, removeError); }
                context->requestAssetBrowserRefresh = true;
            });
    }
    // モーションベクター生成もテクスチャファイルだけを扱い、Scene を必要としない。
    // Scene 必須チェックより前に置くこと (独立 VFX Editor にはゲーム Scene が無い)。
    if (type == "vfx.generateMotionVectors") {
        namespace fs = std::filesystem;
        fs::path textureFile;
        std::string texturePath;
        if (!ResolveProjectFile(ctx, StringField(payload, "texturePath"), textureFile, texturePath)
            || !fs::is_regular_file(textureFile)) {
            err = Outcome::Err("TEXTURE_NOT_FOUND", "projectRoot 配下のテクスチャを指定してください");
            return nullptr;
        }
        asset::FlipbookMotionVectorSettings settings;
        settings.columns = payload.Find("columns") != nullptr ? payload.Find("columns")->AsInt() : 1;
        settings.rows = payload.Find("rows") != nullptr ? payload.Find("rows")->AsInt() : 1;
        if (const JsonValue* radius = payload.Find("searchRadius"); radius != nullptr)
            settings.searchRadius = radius->AsInt();
        if (const JsonValue* loopValue = payload.Find("loop"); loopValue != nullptr)
            settings.loop = loopValue->AsBool();

        // 生成は既存アセットを書き換えず新規ファイルを足すだけなので、Undo は「何もしない」。
        // WHY: 同名の MV が既にあった場合、Undo で消すとユーザーが手で用意した
        //      アトラスを破壊しうる。生成物の削除は AssetBrowser から明示的に行わせる。
        editor::EditorContext* context = &ctx;
        const std::string resolved = textureFile.generic_string();
        return std::make_unique<LambdaCommand>("AI: Generate Motion Vectors",
            [context, resolved, settings]() {
                const auto result = asset::GenerateFlipbookMotionVectors(resolved, settings);
                if (result.success) context->requestAssetBrowserRefresh = true;
            },
            []() {});
    }
    // .behaviortree の編集も Scene を必要としない (アセット単体で完結する)。
    if (type.starts_with("bt.")) return BuildBehaviorTreeCommand(ctx, type, payload, err);
    // .vfx asset編集はメインSceneを必要としない。独立VFX Editorだけ開いた状態でもAI編集を許可する。
    if (type == "vfx.graph.set" || type.starts_with("vfx.node.") || type.starts_with("vfx.link.")
        || type.starts_with("vfx.param.") || type == "vfx.optimize"
        || type.starts_with("vfx.variant.") || type.starts_with("vfx.group.") || type == "vfx.repair")
        return BuildVFXAssetCommand(ctx, type, payload, err);
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    if (type == "vfx.instance.set" || type == "vfx.instance.clear") {
        const std::string id = StringField(payload, "id");
        const std::string name = StringField(payload, "name");
        GameObject* gameObject = scene->FindByGuid(id);
        auto* component = gameObject != nullptr ? gameObject->GetComponent<scene::VFXGraphComponent>() : nullptr;
        if (component == nullptr) { err = Outcome::Err("NOT_PRESENT", "VFXGraphComponentがありません"); return nullptr; }
        const auto oldOverrides = component->parameterOverrides;
        auto newOverrides = oldOverrides;
        std::erase_if(newOverrides, [&](const asset::VFXParamOverride& item) { return item.paramName == name; });
        if (type == "vfx.instance.set") {
            asset::VFXGraphAsset graph;
            std::string loadError;
            if (!asset::LoadVFXGraphAsset(component->graphPath, graph, &loadError)) {
                err = Outcome::Err("VFX_NOT_FOUND", loadError); return nullptr;
            }
            const auto definition = std::find_if(graph.parameters.begin(), graph.parameters.end(),
                [&](const auto& item) { return item.name == name; });
            const JsonValue* input = payload.Find("value");
            asset::VFXParamValue value;
            if (definition == graph.parameters.end() || input == nullptr
                || !JsonToVFXParamValue(*input, definition->type, value)) {
                err = Outcome::Err("TYPE_MISMATCH", "公開パラメーターとvalueが一致しません"); return nullptr;
            }
            newOverrides.push_back({ name, std::move(value) });
        }
        return std::make_unique<LambdaCommand>("AI: Set VFX Override",
            [scene, id, newOverrides, markDirty]() {
                if (auto* object = scene->FindByGuid(id))
                    if (auto* value = object->GetComponent<scene::VFXGraphComponent>()) {
                        value->parameterOverrides = newOverrides; value->reloadRequested = true;
                    }
                markDirty();
            },
            [scene, id, oldOverrides, markDirty]() {
                if (auto* object = scene->FindByGuid(id))
                    if (auto* value = object->GetComponent<scene::VFXGraphComponent>()) {
                        value->parameterOverrides = oldOverrides; value->reloadRequested = true;
                    }
                markDirty();
            });
    }

    if (type == "node.create") {
        std::string parent = StringField(payload, "parent");
        std::string name = StringField(payload, "name");
        if (name.empty()) name = "GameObject";
        if (!parent.empty() && scene->FindByGuid(parent) == nullptr) {
            err = Outcome::Err("NODE_NOT_FOUND", "parent が見つかりません: " + parent);
            return nullptr;
        }
        auto guid = std::make_shared<std::string>(); // execute で確定する NodeId
        return std::make_unique<LambdaCommand>("AI: Create Node",
            [scene, name, parent, guid, createdSink, markDirty]() {
                GameObject& go = scene->CreateGameObject(name);
                if (!parent.empty()) { if (GameObject* p = scene->FindByGuid(parent)) go.SetParent(*p); }
                *guid = go.instanceId;
                if (createdSink) *createdSink = go.instanceId;
                markDirty();
            },
            [scene, guid, markDirty]() {
                if (!guid->empty()) { if (GameObject* go = scene->FindByGuid(*guid)) scene->DestroyGameObject(go->GetID()); }
                markDirty();
            });
    }

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
            // 元ノード配下へ複製すると、コピー中に生成物を再帰走査へ取り込み得るため禁止する。
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
                context->selectedEntities = { duplicateId };
                markDirty();
            },
            [context, scene, duplicateGuid, previousSelection, markDirty]() {
                if (!duplicateGuid->empty()) {
                    if (GameObject* duplicate = scene->FindByGuid(*duplicateGuid)) {
                        scene->DestroyGameObject(duplicate->GetID());
                    }
                }
                context->selectedEntities = *previousSelection;
                editor::PruneSelection(*context);
                markDirty();
            });
    }

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
        if (LowerAscii(prefabPath.extension().string()) != ".prefab" || !fs::is_regular_file(prefabPath, ec)) {
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

    if (type == "prefab.create") {
        // 選択 (または payload.ids) から .prefab を作り、ソースをインスタンス接続する。
        // WHY: Hierarchy / AssetBrowser D&D と同じ Create Prefab を AI からも実行できるようにする。
        //      SaveSelectionAndConnect に集約済みのため、青色表示・Apply/Revert 接続も自動で付く。
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) {
            err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
            return nullptr;
        }

        // 対象ノード: payload.ids があればそれを、無ければ現在の選択を使う。
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

        // 保存先 .prefab パスを projectRoot 配下へ正規化する (まだ存在しなくてよい)。
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
        if (LowerAscii(prefabPath.extension().string()) != ".prefab") {
            err = Outcome::Err("BAD_PATH", "保存先は .prefab 拡張子にしてください");
            return nullptr;
        }

        const std::string diskPath = prefabPath.generic_string();
        auto connectedGuids = std::make_shared<std::vector<std::string>>();
        return std::make_unique<LambdaCommand>("AI: Create Prefab",
            [scene, selectionGuids, diskPath, connectedGuids, createdSink, markDirty]() {
                // guid → EntityID を毎回解決する (redo でも最新 Scene に追随)。
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

    if (type == "prefab.apply") {
        // インスタンスの現在状態を元 .prefab へ書き戻す (ディスクのみ変更)。
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
        return std::make_unique<LambdaCommand>("AI: Apply Prefab",
            [scene, id, projectRoot, beforeContent, existedBefore, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr || target->prefabAssetPath.empty()) return;
                // Apply 前のアセット内容を退避して undo で書き戻せるようにする。
                const std::string disk = editor::ToProjectAssetDiskPath(projectRoot, target->prefabAssetPath);
                *existedBefore = util::FileSystem::ReadText(disk, *beforeContent);
                editor::PrefabSerializer::Apply(*scene, target->GetID(), projectRoot);
                markDirty();
            },
            [scene, id, projectRoot, beforeContent, existedBefore, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr || target->prefabAssetPath.empty()) return;
                const std::string disk = editor::ToProjectAssetDiskPath(projectRoot, target->prefabAssetPath);
                if (*existedBefore) util::FileSystem::WriteText(disk, *beforeContent);
                else std::filesystem::remove(std::filesystem::path(disk));
                markDirty();
            });
    }

    if (type == "prefab.revert") {
        // インスタンスを元 .prefab の定義へ戻す。シーン変更のため前後スナップショットで undo する。
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
                // 初回は revert を実行して前後スナップショットを取り、redo 以降は after を復元する。
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
        return std::make_unique<LambdaCommand>("AI: Set Material Shader",
            [context, materialFile, materialPath, newAsset]() {
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), newAsset)) {
                    asset::AssetManager::Unload<asset::MaterialAsset>(materialPath);
                    context->requestAssetBrowserRefresh = true;
                }
            },
            [context, materialFile, materialPath, oldAsset]() {
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), oldAsset)) {
                    asset::AssetManager::Unload<asset::MaterialAsset>(materialPath);
                    context->requestAssetBrowserRefresh = true;
                }
            });
    }

    if (type == "node.rename") {
        const std::string id = StringField(payload, "id");
        const std::string name = StringField(payload, "name");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (name.empty()) { err = Outcome::Err("BAD_ARG", "name が空です"); return nullptr; }
        auto oldName = std::make_shared<std::string>(go->name);
        return std::make_unique<LambdaCommand>("AI: Rename Node",
            [scene, id, name, markDirty]()    { if (GameObject* g = scene->FindByGuid(id)) g->name = name; markDirty(); },
            [scene, id, oldName, markDirty]() { if (GameObject* g = scene->FindByGuid(id)) g->name = *oldName; markDirty(); });
    }

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
        // rot は Euler 度で受け取り Quaternion へ変換する (Inspector と同じ表現)。
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

    if (type == "node.delete") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        // 単一ノードのスナップショット (子階層は Undo 対象外 — MVP の制約)。
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
                go.instanceId = id; // NodeId を保って再生成し、参照の安定性を維持する
                go.tag = *tag;
                go.layer = layer;
                go.transform.position = pos;
                go.transform.rotation = rot;
                go.transform.scale = scl;
                if (!parentGuid->empty()) { if (GameObject* p = scene->FindByGuid(*parentGuid)) go.SetParent(*p); }
                go.SetSiblingIndex(sibling);
                RestoreComponents(go, *snapshot);
                // active は SetParent 後に適用し、activeInHierarchy の再計算を正しく行う。
                go.SetActive(active);
                markDirty();
            });
    }

    if (type == "component.add") {
        const std::string id = StringField(payload, "id");
        const std::string comp = StringField(payload, "comp");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const TypeInfo info = InspectComponentType(*go, comp);
        if (!info.known)   { err = Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + comp); return nullptr; }
        if (!info.addable) { err = Outcome::Err("NOT_ADDABLE", "追加できないコンポーネント: " + comp); return nullptr; }
        auto wasPresent = std::make_shared<bool>(false);
        return std::make_unique<LambdaCommand>("AI: Add Component",
            [scene, id, comp, wasPresent, markDirty]() {
                GameObject* g = scene->FindByGuid(id);
                if (!g) return;
                *wasPresent = InspectComponentType(*g, comp).present;
                if (!*wasPresent) AddComponentByName(*g, comp);
                markDirty();
            },
            [scene, id, comp, wasPresent, markDirty]() {
                if (*wasPresent) return; // 元から在ったものは消さない
                if (GameObject* g = scene->FindByGuid(id)) RemoveComponentByName(*g, comp);
                markDirty();
            });
    }

    if (type == "component.remove") {
        const std::string id = StringField(payload, "id");
        const std::string comp = StringField(payload, "comp");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        const TypeInfo info = InspectComponentType(*go, comp);
        if (!info.known)   { err = Outcome::Err("UNKNOWN_COMPONENT", "未知のコンポーネント: " + comp); return nullptr; }
        if (!info.present) { err = Outcome::Err("NOT_PRESENT", "そのコンポーネントは装着されていません: " + comp); return nullptr; }
        // Undo 用に単一エントリのスナップショットを作る (RestoreComponents で再構築)。
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

    // ── Animator ステートマシンの構造編集 (Undo 対応) ────────────────────────
    // すべて MakeAnimatorEditCommand 経由で states/anyState/parameters スナップショット復元する。
    if (type == "animation.addTransition" || type == "animation.setCondition" ||
        type == "animation.removeTransition" ||
        type == "animation.addState" || type == "animation.setState" ||
        type == "animation.removeState" ||
        type == "animation.addMotion" || type == "animation.setMotion" ||
        type == "animation.removeMotion" ||
        type == "animation.addParameter" || type == "animation.removeParameter") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
        if (animator == nullptr) { err = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません"); return nullptr; }

        // レイヤー指定 (省略で Base Layer)。
        // WHY: 上半身レイヤーに独自の遷移グラフを組むには、これらの編集操作が
        //      「どのグラフに対するものか」を選べる必要がある。指定がなければ従来どおり。
        const std::string layerName = StringField(payload, "layer");
        const AnimatorGraphTarget graphTarget = ResolveGraphTarget(*animator, layerName);
        if (graphTarget.states == nullptr) {
            err = Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
            return nullptr;
        }

        auto findStateIndex = [&](const std::string& name) -> int {
            for (int i = 0; i < static_cast<int>((*graphTarget.states).size()); ++i)
                if ((*graphTarget.states)[static_cast<size_t>(i)].name == name) return i;
            return -1;
        };

        // ── パラメーター CRUD ──
        if (type == "animation.addParameter") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "パラメーター name が必要です"); return nullptr; }
            const bool duplicate = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == name; });
            if (duplicate) { err = Outcome::Err("DUPLICATE_PARAM", "同名のパラメーターが既に存在します: " + name); return nullptr; }

            scene::AnimatorParameter prototype;
            prototype.name = name;
            // type 省略時は float。value 省略時は型ごとの既定 (0 / false)。
            const std::string typeName = StringField(payload, "type");
            if (!typeName.empty()) {
                if (typeName == "float")        prototype.type = scene::ParamType::Float;
                else if (typeName == "int")     prototype.type = scene::ParamType::Int;
                else if (typeName == "bool")    prototype.type = scene::ParamType::Bool;
                else if (typeName == "trigger") prototype.type = scene::ParamType::Trigger;
                else { err = Outcome::Err("BAD_ARG", "type は float/int/bool/trigger のいずれかです"); return nullptr; }
            }
            if (const JsonValue* v = payload.Find("value"); v != nullptr) {
                switch (prototype.type) {
                case scene::ParamType::Float:
                    if (!v->IsNumber()) { err = Outcome::Err("BAD_ARG", "float の value は数値です"); return nullptr; }
                    prototype.floatValue = static_cast<float>(v->AsNumber());
                    break;
                case scene::ParamType::Int:
                    if (!v->IsNumber()) { err = Outcome::Err("BAD_ARG", "int の value は数値です"); return nullptr; }
                    prototype.intValue = v->AsInt();
                    break;
                case scene::ParamType::Bool:
                case scene::ParamType::Trigger:
                    if (!v->IsBool()) { err = Outcome::Err("BAD_ARG", "bool/trigger の value は真偽です"); return nullptr; }
                    prototype.boolValue = v->AsBool();
                    break;
                }
            }
            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Parameter", markDirty,
                [prototype](scene::AnimatorComponent& a) { a.parameters.push_back(prototype); });
        }

        if (type == "animation.removeParameter") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "パラメーター name が必要です"); return nullptr; }
            const bool exists = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == name; });
            if (!exists) { err = Outcome::Err("PARAM_NOT_FOUND", "パラメーターが見つかりません: " + name); return nullptr; }
            // WHY: このパラメーターを参照する遷移条件は残す (Unity/本エディター同様)。
            //      無効参照は発火しないだけで壊れず、Undo で丸ごと戻せるため cascade 削除しない。
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Parameter", markDirty,
                [name](scene::AnimatorComponent& a) {
                    a.parameters.erase(
                        std::remove_if(a.parameters.begin(), a.parameters.end(),
                            [&](const scene::AnimatorParameter& p) { return p.name == name; }),
                        a.parameters.end());
                });
        }

        // ── ステート削除 (参照の遷移も掃除) ──
        if (type == "animation.removeState") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            if (findStateIndex(stateName) < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator State", markDirty,
                [stateName, layerName](scene::AnimatorComponent& a) {
                    // 対象ステートを消し、他ステート/AnyState からの遷移参照も除去する (パネルの DeleteState 相当)。
                    GRAPH(a, layerName).StatesRef().erase(
                        std::remove_if(GRAPH(a, layerName).StatesRef().begin(), GRAPH(a, layerName).StatesRef().end(),
                            [&](const scene::AnimationState& s) { return s.name == stateName; }),
                        GRAPH(a, layerName).StatesRef().end());
                    for (auto& s : GRAPH(a, layerName).StatesRef()) {
                        s.transitions.erase(
                            std::remove_if(s.transitions.begin(), s.transitions.end(),
                                [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                            s.transitions.end());
                    }
                    GRAPH(a, layerName).AnyRef().erase(
                        std::remove_if(GRAPH(a, layerName).AnyRef().begin(), GRAPH(a, layerName).AnyRef().end(),
                            [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                        GRAPH(a, layerName).AnyRef().end());
                    if (GRAPH(a, layerName).DefaultRef() == stateName)
                        GRAPH(a, layerName).DefaultRef() = GRAPH(a, layerName).StatesRef().empty() ? std::string{} : GRAPH(a, layerName).StatesRef().front().name;
                });
        }

        // ── ステート追加 ──
        if (type == "animation.addState") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "ステート name が必要です"); return nullptr; }
            if (findStateIndex(name) >= 0) { err = Outcome::Err("DUPLICATE_STATE", "同名のステートが既に存在します: " + name); return nullptr; }

            scene::AnimationState prototype;
            prototype.name = name;
            if (const JsonValue* v = payload.Find("mode"); v != nullptr && v->IsString()) {
                if (!ParseAnimationStateMode(v->AsString(), prototype.mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は clip/blendTree1D/blendTree2D のいずれかです"); return nullptr;
                }
            }
            if (const JsonValue* v = payload.Find("sourcePath"); v != nullptr && v->IsString()) prototype.sourcePath = v->AsString();
            if (const JsonValue* v = payload.Find("clipName"); v != nullptr && v->IsString()) prototype.clipName = v->AsString();
            if (const JsonValue* v = payload.Find("clipIndex"); v != nullptr && v->IsNumber()) prototype.clipIndex = v->AsInt();
            if (const JsonValue* v = payload.Find("speed"); v != nullptr && v->IsNumber()) prototype.speed = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("loop"); v != nullptr && v->IsBool()) prototype.loop = v->AsBool();
            if (const JsonValue* v = payload.Find("ikWeight"); v != nullptr && v->IsNumber()) prototype.ikWeight = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("blendParameter"); v != nullptr && v->IsString()) prototype.blendTree1D.paramName = v->AsString();
            if (const JsonValue* v = payload.Find("blendParameterX"); v != nullptr && v->IsString()) prototype.blendTree2D.paramX = v->AsString();
            if (const JsonValue* v = payload.Find("blendParameterY"); v != nullptr && v->IsString()) prototype.blendTree2D.paramY = v->AsString();
            bool setAsDefault = false;
            if (const JsonValue* v = payload.Find("setAsDefault"); v != nullptr && v->IsBool()) setAsDefault = v->AsBool();

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator State", markDirty,
                [prototype, setAsDefault, layerName](scene::AnimatorComponent& a) {
                    GRAPH(a, layerName).StatesRef().push_back(prototype);
                    // Unity 同様、最初のステートや明示指定時はデフォルトにする。
                    if (setAsDefault || GRAPH(a, layerName).DefaultRef().empty())
                        GRAPH(a, layerName).DefaultRef() = prototype.name;
                });
        }

        // ── ステート更新 (rename は参照追従) ──
        if (type == "animation.setState") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            const int stateIndex = findStateIndex(stateName);
            if (stateIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }

            // rename 検証: 新名は空不可、他ステートと衝突不可。
            std::string newName;
            if (const JsonValue* v = payload.Find("name"); v != nullptr && v->IsString()) {
                newName = v->AsString();
                if (newName.empty()) { err = Outcome::Err("BAD_ARG", "name は空にできません"); return nullptr; }
                const int existing = findStateIndex(newName);
                if (existing >= 0 && existing != stateIndex) { err = Outcome::Err("DUPLICATE_STATE", "同名のステートが既に存在します: " + newName); return nullptr; }
            }
            // mode / 2Dtype は build 時にパース検証する。
            bool hasMode = false;
            scene::AnimationStateMode mode = scene::AnimationStateMode::Clip;
            if (const JsonValue* v = payload.Find("mode"); v != nullptr && v->IsString()) {
                if (!ParseAnimationStateMode(v->AsString(), mode)) { err = Outcome::Err("BAD_ARG", "mode は clip/blendTree1D/blendTree2D のいずれかです"); return nullptr; }
                hasMode = true;
            }
            bool hasBlend2DType = false;
            scene::BlendTree2DType blend2DType = scene::BlendTree2DType::SimpleDirectional;
            if (const JsonValue* v = payload.Find("blend2DType"); v != nullptr && v->IsString()) {
                if (!ParseBlendTree2DType(v->AsString(), blend2DType)) { err = Outcome::Err("BAD_ARG", "blend2DType は simpleDirectional/freeformCartesian のいずれかです"); return nullptr; }
                hasBlend2DType = true;
            }
            // 変更内容を JSON からそのまま持ち回すために payload を複製して束縛する。
            auto captured = std::make_shared<JsonValue>(payload);
            const bool setAsDefault = [&]() {
                const JsonValue* v = payload.Find("setAsDefault");
                return v != nullptr && v->IsBool() && v->AsBool();
            }();

            return MakeAnimatorEditCommand(scene, id, "AI: Set Animator State", markDirty,
                [stateName, newName, hasMode, mode, hasBlend2DType, blend2DType, setAsDefault, captured, layerName]
                (scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(GRAPH(a, layerName).StatesRef().size()); ++i)
                        if (GRAPH(a, layerName).StatesRef()[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = GRAPH(a, layerName).StatesRef()[static_cast<size_t>(index)];
                    const JsonValue& p = *captured;
                    if (hasMode) s.mode = mode;
                    if (const JsonValue* v = p.Find("sourcePath"); v != nullptr && v->IsString()) s.sourcePath = v->AsString();
                    if (const JsonValue* v = p.Find("clipName"); v != nullptr && v->IsString()) s.clipName = v->AsString();
                    if (const JsonValue* v = p.Find("clipIndex"); v != nullptr && v->IsNumber()) s.clipIndex = v->AsInt();
                    if (const JsonValue* v = p.Find("speed"); v != nullptr && v->IsNumber()) s.speed = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("loop"); v != nullptr && v->IsBool()) s.loop = v->AsBool();
                    if (const JsonValue* v = p.Find("ikWeight"); v != nullptr && v->IsNumber()) s.ikWeight = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("blendParameter"); v != nullptr && v->IsString()) s.blendTree1D.paramName = v->AsString();
                    if (const JsonValue* v = p.Find("blendParameterX"); v != nullptr && v->IsString()) s.blendTree2D.paramX = v->AsString();
                    if (const JsonValue* v = p.Find("blendParameterY"); v != nullptr && v->IsString()) s.blendTree2D.paramY = v->AsString();
                    if (hasBlend2DType) s.blendTree2D.type = blend2DType;
                    if (setAsDefault) GRAPH(a, layerName).DefaultRef() = s.name;
                    // rename は参照追従のため最後に行う (s は無効になり得るので name 取得後)。
                    if (!newName.empty() && newName != stateName)
                        RenameAnimatorState(a, stateName, newName);
                });
        }

        // ── BlendTree Motion 追加・更新・削除 ──
        if (type == "animation.addMotion" || type == "animation.setMotion" ||
            type == "animation.removeMotion") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            const int stateIndex = findStateIndex(stateName);
            if (stateIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }
            const scene::AnimationState& state = (*graphTarget.states)[static_cast<size_t>(stateIndex)];
            const bool is2D = state.mode == scene::AnimationStateMode::BlendTree2D;
            if (state.mode != scene::AnimationStateMode::BlendTree1D && !is2D) {
                err = Outcome::Err("NOT_BLEND_TREE", "このステートは BlendTree ではありません: " + stateName); return nullptr;
            }
            const std::vector<scene::BlendTreeMotion>& motions =
                is2D ? state.blendTree2D.motions : state.blendTree1D.motions;

            const bool isAdd = (type == "animation.addMotion");
            const bool isRemove = (type == "animation.removeMotion");
            int motionIndex = -1;
            if (!isAdd) {
                const JsonValue* motionIndexField = payload.Find("motionIndex");
                if (motionIndexField == nullptr || !motionIndexField->IsNumber()) { err = Outcome::Err("BAD_ARG", "motionIndex が必要です"); return nullptr; }
                motionIndex = motionIndexField->AsInt();
                if (motionIndex < 0 || motionIndex >= static_cast<int>(motions.size())) { err = Outcome::Err("MOTION_NOT_FOUND", "motionIndex が範囲外です"); return nullptr; }
            }
            auto captured = std::make_shared<JsonValue>(payload);

            const char* description = isAdd ? "AI: Add BlendTree Motion"
                : isRemove ? "AI: Remove BlendTree Motion" : "AI: Set BlendTree Motion";
            return MakeAnimatorEditCommand(scene, id, description, markDirty,
                [stateName, is2D, isAdd, isRemove, motionIndex, captured, layerName](scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(GRAPH(a, layerName).StatesRef().size()); ++i)
                        if (GRAPH(a, layerName).StatesRef()[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = GRAPH(a, layerName).StatesRef()[static_cast<size_t>(index)];
                    std::vector<scene::BlendTreeMotion>& motionList =
                        is2D ? s.blendTree2D.motions : s.blendTree1D.motions;

                    if (isRemove) {
                        if (motionIndex >= 0 && motionIndex < static_cast<int>(motionList.size()))
                            motionList.erase(motionList.begin() + motionIndex);
                        return;
                    }

                    scene::BlendTreeMotion* motion = nullptr;
                    scene::BlendTreeMotion added;
                    if (isAdd) {
                        motion = &added;
                    } else {
                        if (motionIndex < 0 || motionIndex >= static_cast<int>(motionList.size())) return;
                        motion = &motionList[static_cast<size_t>(motionIndex)];
                    }
                    const JsonValue& p = *captured;
                    if (const JsonValue* v = p.Find("sourcePath"); v != nullptr && v->IsString()) motion->sourcePath = v->AsString();
                    if (const JsonValue* v = p.Find("clipName"); v != nullptr && v->IsString()) motion->clipName = v->AsString();
                    if (const JsonValue* v = p.Find("clipIndex"); v != nullptr && v->IsNumber()) motion->clipIndex = v->AsInt();
                    if (const JsonValue* v = p.Find("threshold"); v != nullptr && v->IsNumber()) motion->threshold = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("posX"); v != nullptr && v->IsNumber()) motion->posX = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("posY"); v != nullptr && v->IsNumber()) motion->posY = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("speed"); v != nullptr && v->IsNumber()) motion->speed = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("ikWeight"); v != nullptr && v->IsNumber()) motion->ikWeight = static_cast<float>(v->AsNumber());
                    if (isAdd) motionList.push_back(added);
                });
        }

        // from 省略 = Any State 遷移。指定時はそのステートの transitions を対象にする。
        const JsonValue* fromField = payload.Find("from");
        const bool isAnyState = (fromField == nullptr) || !fromField->IsString()
            || fromField->AsString().empty() || fromField->AsString() == "AnyState";
        const std::string fromName = isAnyState ? std::string{} : fromField->AsString();
        int fromIndex = -1;
        if (!isAnyState) {
            fromIndex = findStateIndex(fromName);
            if (fromIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "from ステートが見つかりません: " + fromName); return nullptr; }
        }

        // 対象の transitions ベクトルを返す (build 時の検証用。実行時は再解決する)。
        auto sourceTransitions = [&]() -> std::vector<scene::AnimationTransition>& {
            return isAnyState ? (*graphTarget.anyState)
                              : (*graphTarget.states)[static_cast<size_t>(fromIndex)].transitions;
        };

        // ── 遷移削除 ──
        if (type == "animation.removeTransition") {
            const JsonValue* transitionIndexField = payload.Find("transitionIndex");
            if (transitionIndexField == nullptr || !transitionIndexField->IsNumber()) { err = Outcome::Err("BAD_ARG", "transitionIndex が必要です"); return nullptr; }
            const int transitionIndex = transitionIndexField->AsInt();
            if (transitionIndex < 0 || transitionIndex >= static_cast<int>(sourceTransitions().size())) { err = Outcome::Err("TRANSITION_NOT_FOUND", "transitionIndex が範囲外です"); return nullptr; }
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Transition", markDirty,
                [isAnyState, fromName, transitionIndex, layerName](scene::AnimatorComponent& a) {
                    std::vector<scene::AnimationTransition>* transitions = nullptr;
                    if (isAnyState) transitions = &GRAPH(a, layerName).AnyRef();
                    else for (auto& s : GRAPH(a, layerName).StatesRef()) if (s.name == fromName) { transitions = &s.transitions; break; }
                    if (transitions == nullptr || transitionIndex >= static_cast<int>(transitions->size())) return;
                    transitions->erase(transitions->begin() + transitionIndex);
                });
        }

        if (type == "animation.addTransition") {
            const std::string toName = StringField(payload, "to");
            if (toName.empty()) { err = Outcome::Err("BAD_ARG", "to ステート名が必要です"); return nullptr; }
            if (findStateIndex(toName) < 0) { err = Outcome::Err("STATE_NOT_FOUND", "to ステートが見つかりません: " + toName); return nullptr; }
            for (const auto& existing : sourceTransitions())
                if (existing.toStateName == toName) { err = Outcome::Err("DUPLICATE_TRANSITION", "同じ遷移先が既に存在します: " + toName); return nullptr; }

            // 任意の初期設定を build 時に検証しておく。
            scene::AnimationTransition prototype;
            prototype.toStateName = toName;
            if (const JsonValue* v = payload.Find("hasExitTime"); v != nullptr && v->IsBool()) prototype.hasExitTime = v->AsBool();
            if (const JsonValue* v = payload.Find("exitTime"); v != nullptr && v->IsNumber()) prototype.exitTime = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("fixedDuration"); v != nullptr && v->IsBool()) prototype.fixedDuration = v->AsBool();
            if (const JsonValue* v = payload.Find("transitionDuration"); v != nullptr && v->IsNumber()) prototype.transitionDuration = static_cast<float>(v->AsNumber());

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Transition", markDirty,
                [isAnyState, fromName, prototype, layerName](scene::AnimatorComponent& a) {
                    if (isAnyState) { GRAPH(a, layerName).AnyRef().push_back(prototype); }
                    else {
                        for (auto& s : GRAPH(a, layerName).StatesRef())
                            if (s.name == fromName) { s.transitions.push_back(prototype); break; }
                    }
                });
        }

        // animation.setCondition: 指定 transition の conditions を add/update/remove/clear する。
        const JsonValue* transitionIndexField = payload.Find("transitionIndex");
        if (transitionIndexField == nullptr || !transitionIndexField->IsNumber()) {
            err = Outcome::Err("BAD_ARG", "transitionIndex が必要です"); return nullptr;
        }
        const int transitionIndex = transitionIndexField->AsInt();
        if (transitionIndex < 0 || transitionIndex >= static_cast<int>(sourceTransitions().size())) {
            err = Outcome::Err("TRANSITION_NOT_FOUND", "transitionIndex が範囲外です"); return nullptr;
        }
        const std::string action = StringField(payload, "action");
        if (action != "add" && action != "update" && action != "remove" && action != "clear") {
            err = Outcome::Err("BAD_ARG", "action は add/update/remove/clear のいずれかです"); return nullptr;
        }

        scene::AnimationTransition& targetTransition = sourceTransitions()[static_cast<size_t>(transitionIndex)];

        // add / update は条件の中身を build 時に検証する。
        scene::AnimatorCondition prototype;
        int conditionIndex = -1;
        if (action == "add" || action == "update") {
            const std::string paramName = StringField(payload, "parameter");
            if (paramName.empty()) { err = Outcome::Err("BAD_ARG", "parameter 名が必要です"); return nullptr; }
            const bool paramExists = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == paramName; });
            if (!paramExists) { err = Outcome::Err("PARAM_NOT_FOUND", "パラメーターが見つかりません: " + paramName); return nullptr; }
            scene::ConditionOp op = scene::ConditionOp::Greater;
            if (!ParseConditionOp(StringField(payload, "op"), op)) {
                err = Outcome::Err("BAD_ARG", "op は greater/less/equal/notEqual/true/false のいずれかです"); return nullptr;
            }
            prototype.paramName = paramName;
            prototype.op = op;
            if (ConditionOpUsesThreshold(op)) {
                const JsonValue* thresholdField = payload.Find("threshold");
                if (thresholdField != nullptr && thresholdField->IsNumber())
                    prototype.threshold = static_cast<float>(thresholdField->AsNumber());
            }
        }
        if (action == "update" || action == "remove") {
            const JsonValue* conditionIndexField = payload.Find("conditionIndex");
            if (conditionIndexField == nullptr || !conditionIndexField->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "conditionIndex が必要です"); return nullptr;
            }
            conditionIndex = conditionIndexField->AsInt();
            if (conditionIndex < 0 || conditionIndex >= static_cast<int>(targetTransition.conditions.size())) {
                err = Outcome::Err("CONDITION_NOT_FOUND", "conditionIndex が範囲外です"); return nullptr;
            }
        }

        return MakeAnimatorEditCommand(scene, id, "AI: Edit Animator Condition", markDirty,
            [isAnyState, fromName, transitionIndex, action, prototype, conditionIndex, layerName]
            (scene::AnimatorComponent& a) {
                std::vector<scene::AnimationTransition>* transitions = nullptr;
                if (isAnyState) transitions = &GRAPH(a, layerName).AnyRef();
                else for (auto& s : GRAPH(a, layerName).StatesRef()) if (s.name == fromName) { transitions = &s.transitions; break; }
                if (transitions == nullptr || transitionIndex >= static_cast<int>(transitions->size())) return;
                auto& conditions = (*transitions)[static_cast<size_t>(transitionIndex)].conditions;
                if (action == "add") conditions.push_back(prototype);
                else if (action == "clear") conditions.clear();
                else if (action == "remove") {
                    if (conditionIndex < static_cast<int>(conditions.size()))
                        conditions.erase(conditions.begin() + conditionIndex);
                } else if (action == "update") {
                    if (conditionIndex < static_cast<int>(conditions.size()))
                        conditions[static_cast<size_t>(conditionIndex)] = prototype;
                }
            });
    }


    // ── Animator レイヤーと Slot (上半身 / 下半身の出し分け) ────────────────────
    // WHY: レイヤーを作れないと animation.addState 等の layer 指定が使えない。
    //      レイヤー CRUD と Slot 再生をここに揃え、MCP から一連の操作を完結させる。
    if (type == "animation.addLayer" || type == "animation.setLayer" ||
        type == "animation.removeLayer" ||
        type == "animation.playSlot" || type == "animation.stopSlot") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
        if (animator == nullptr) { err = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません"); return nullptr; }

        const auto parseLayerMode = [&](const std::string& name, scene::AnimationLayerMode& out) {
            if (name == "override") { out = scene::AnimationLayerMode::Override; return true; }
            if (name == "additive") { out = scene::AnimationLayerMode::Additive; return true; }
            return false;
        };

        if (type == "animation.addLayer") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "レイヤー name が必要です"); return nullptr; }
            if (animator->FindLayer(name) != nullptr) {
                err = Outcome::Err("DUPLICATE_LAYER", "同名のレイヤーが既に存在します: " + name);
                return nullptr;
            }
            scene::AnimationLayer prototype;
            prototype.name = name;
            if (const JsonValue* v = payload.Find("weight"); v && v->IsNumber())
                prototype.weight = std::clamp(static_cast<float>(v->AsNumber()), 0.0f, 1.0f);
            if (const JsonValue* v = payload.Find("mode"); v && v->IsString()) {
                if (!parseLayerMode(v->AsString(), prototype.mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は override/additive のいずれかです");
                    return nullptr;
                }
            }
            if (const JsonValue* v = payload.Find("enabled"); v && v->IsBool()) prototype.enabled = v->AsBool();
            if (const JsonValue* v = payload.Find("maskPath"); v && v->IsString()) prototype.mask.path = v->AsString();
            if (const JsonValue* v = payload.Find("stateName"); v && v->IsString()) prototype.stateName = v->AsString();
            if (const JsonValue* v = payload.Find("additiveSourcePath"); v && v->IsString())
                prototype.additiveReference.sourcePath = v->AsString();
            if (const JsonValue* v = payload.Find("additiveClipName"); v && v->IsString())
                prototype.additiveReference.clipName = v->AsString();
            if (const JsonValue* v = payload.Find("additiveTime"); v && v->IsNumber())
                prototype.additiveReference.time = static_cast<float>(v->AsNumber());

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Layer", markDirty,
                [prototype](scene::AnimatorComponent& a) { a.layers.push_back(prototype); });
        }

        // 以降は既存レイヤーを対象にする。
        const std::string layerName = StringField(payload, "layer");
        if (layerName.empty()) { err = Outcome::Err("BAD_ARG", "対象 layer 名が必要です"); return nullptr; }
        if (animator->FindLayer(layerName) == nullptr) {
            err = Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
            return nullptr;
        }

        if (type == "animation.removeLayer") {
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Layer", markDirty,
                [layerName](scene::AnimatorComponent& a) {
                    a.layers.erase(
                        std::remove_if(a.layers.begin(), a.layers.end(),
                            [&](const scene::AnimationLayer& l) { return l.name == layerName; }),
                        a.layers.end());
                });
        }

        if (type == "animation.setLayer") {
            // rename 検証: 空不可・他レイヤーと衝突不可。
            std::string newName;
            if (const JsonValue* v = payload.Find("name"); v && v->IsString()) {
                newName = v->AsString();
                if (newName.empty()) { err = Outcome::Err("BAD_ARG", "name は空にできません"); return nullptr; }
                if (newName != layerName && animator->FindLayer(newName) != nullptr) {
                    err = Outcome::Err("DUPLICATE_LAYER", "同名のレイヤーが既に存在します: " + newName);
                    return nullptr;
                }
            }
            scene::AnimationLayerMode mode{};
            bool hasMode = false;
            if (const JsonValue* v = payload.Find("mode"); v && v->IsString()) {
                if (!parseLayerMode(v->AsString(), mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は override/additive のいずれかです");
                    return nullptr;
                }
                hasMode = true;
            }
            // 部分更新: 指定されたキーだけを書き換える。
            const JsonValue* weightValue   = payload.Find("weight");
            const JsonValue* enabledValue  = payload.Find("enabled");
            const JsonValue* maskValue     = payload.Find("maskPath");
            const JsonValue* stateValue    = payload.Find("stateName");
            const JsonValue* defaultValue  = payload.Find("defaultStateName");
            const JsonValue* addSrcValue   = payload.Find("additiveSourcePath");
            const JsonValue* addClipValue  = payload.Find("additiveClipName");
            const JsonValue* addTimeValue  = payload.Find("additiveTime");

            const float weight = (weightValue && weightValue->IsNumber())
                ? std::clamp(static_cast<float>(weightValue->AsNumber()), 0.0f, 1.0f) : 0.0f;
            const bool  enabled = (enabledValue && enabledValue->IsBool()) && enabledValue->AsBool();
            const std::string maskPath = (maskValue && maskValue->IsString()) ? maskValue->AsString() : std::string{};
            const std::string stateName = (stateValue && stateValue->IsString()) ? stateValue->AsString() : std::string{};
            const std::string defaultStateName = (defaultValue && defaultValue->IsString()) ? defaultValue->AsString() : std::string{};
            const std::string addSrc = (addSrcValue && addSrcValue->IsString()) ? addSrcValue->AsString() : std::string{};
            const std::string addClip = (addClipValue && addClipValue->IsString()) ? addClipValue->AsString() : std::string{};
            const float addTime = (addTimeValue && addTimeValue->IsNumber()) ? static_cast<float>(addTimeValue->AsNumber()) : 0.0f;

            const bool hasWeight  = weightValue && weightValue->IsNumber();
            const bool hasEnabled = enabledValue && enabledValue->IsBool();
            const bool hasMask    = maskValue && maskValue->IsString();
            const bool hasState   = stateValue && stateValue->IsString();
            const bool hasDefault = defaultValue && defaultValue->IsString();
            const bool hasAddSrc  = addSrcValue && addSrcValue->IsString();
            const bool hasAddClip = addClipValue && addClipValue->IsString();
            const bool hasAddTime = addTimeValue && addTimeValue->IsNumber();

            return MakeAnimatorEditCommand(scene, id, "AI: Set Animator Layer", markDirty,
                [=](scene::AnimatorComponent& a) {
                    scene::AnimationLayer* l = a.FindLayer(layerName);
                    if (l == nullptr) return;
                    if (hasWeight)  l->weight = weight;
                    if (hasEnabled) l->enabled = enabled;
                    if (hasMode)    l->mode = mode;
                    if (hasMask) {
                        l->mask.path = maskPath;
                        // 次フレームの AnimatorSystem に読み直させる。
                        l->mask.Invalidate();
                    }
                    if (hasState)   l->stateName = stateName;
                    if (hasDefault) l->defaultStateName = defaultStateName;
                    if (hasAddSrc)  l->additiveReference.sourcePath = addSrc;
                    if (hasAddClip) l->additiveReference.clipName = addClip;
                    if (hasAddTime) l->additiveReference.time = addTime;
                    if (!newName.empty()) l->name = newName;
                });
        }

        // ── Slot ────────────────────────────────────────────────────────────
        // WHY: Slot 自体はランタイム状態だが、他の編集と同じ Command 経路に載せる。
        //      dryRun 判定と Undo スタックへの積み込みを呼び出し側に任せられ、
        //      「AI が投げた割り込み再生を Undo で取り消す」も自然に成立する。
        if (type == "animation.playSlot") {
            const std::string sourcePath = StringField(payload, "sourcePath");
            const std::string clipName   = StringField(payload, "clipName");
            if (sourcePath.empty() && clipName.empty()) {
                err = Outcome::Err("BAD_ARG", "sourcePath または clipName が必要です");
                return nullptr;
            }
            float fadeIn = 0.15f, fadeOut = 0.15f, slotSpeed = 1.0f;
            bool  slotLoop = false;
            if (const JsonValue* v = payload.Find("fadeIn"); v && v->IsNumber())  fadeIn = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("fadeOut"); v && v->IsNumber()) fadeOut = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("speed"); v && v->IsNumber())   slotSpeed = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("loop"); v && v->IsBool())      slotLoop = v->AsBool();
            return MakeAnimatorEditCommand(scene, id, "AI: Play Animator Slot", markDirty,
                [=](scene::AnimatorComponent& a) {
                    a.PlaySlot(layerName, sourcePath, clipName, fadeIn, fadeOut, slotSpeed, slotLoop);
                });
        }

        if (type == "animation.stopSlot") {
            float fadeOut = -1.0f;
            if (const JsonValue* v = payload.Find("fadeOut"); v && v->IsNumber())
                fadeOut = static_cast<float>(v->AsNumber());
            return MakeAnimatorEditCommand(scene, id, "AI: Stop Animator Slot", markDirty,
                [layerName, fadeOut](scene::AnimatorComponent& a) {
                    a.StopSlot(layerName, fadeOut);
                });
        }
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

// Command 種別を受けて適用する。undo/redo/selection/asset.import/transaction は個別処理、他は BuildCommand。
Outcome DoCommand(editor::EditorContext& ctx, const std::string& type, const JsonValue& payload, bool dryRun)
{
    if (type == "input.inject") {
        if (ctx.playMode == nullptr) return Outcome::Err("NO_PLAY_MODE", "PlayModeController が未設定です");
        const std::string kind = StringField(payload, "kind");
        if (kind != "clear" && ctx.playMode->IsInEditor()) {
            return Outcome::Err("INVALID_PLAY_STATE", "入力注入は Play/Pause 中だけ実行できます");
        }
        if (dryRun) return DryRunPreview("input.inject:" + kind);
        bool applied = false;
        if (kind == "clear") {
            input::Input::ClearInjected();
            applied = true;
        } else if (kind == "key") {
            const int key = VirtualKeyFromName(StringField(payload, "key"));
            const JsonValue* pressed = payload.Find("pressed");
            if (key < 0 || pressed == nullptr || !pressed->IsBool()) return Outcome::Err("BAD_ARG", "有効な key / pressed が必要です");
            applied = input::Input::InjectKey(static_cast<uint32_t>(key), pressed->AsBool());
        } else if (kind == "axis") {
            const std::string axis = StringField(payload, "axis");
            const JsonValue* value = payload.Find("value");
            if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "axis / value が必要です");
            applied = input::Input::SetVirtualAxis(axis, static_cast<float>(value->AsNumber()));
        } else if (kind == "gamepadAxis") {
            const std::string axis = StringField(payload, "axis");
            const JsonValue* value = payload.Find("value");
            if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "axis / value が必要です");
            applied = input::Input::SetVirtualAxis(axis, static_cast<float>(value->AsNumber()));
        } else if (kind == "gamepadButton") {
            const std::string buttonName = StringField(payload, "buttonName");
            const JsonValue* pressed = payload.Find("pressed");
            if (pressed == nullptr || !pressed->IsBool()) return Outcome::Err("BAD_ARG", "buttonName / pressed が必要です");
            applied = input::Input::SetVirtualButton(buttonName, pressed->AsBool());
        } else if (kind == "mouseButton") {
            const JsonValue* button = payload.Find("button");
            const JsonValue* pressed = payload.Find("pressed");
            if (button == nullptr || !button->IsNumber() || pressed == nullptr || !pressed->IsBool()) {
                return Outcome::Err("BAD_ARG", "button / pressed が必要です");
            }
            applied = input::Input::InjectMouseButton(button->AsInt(), pressed->AsBool());
        } else if (kind == "mousePosition" || kind == "mouseDelta") {
            const JsonValue* value = payload.Find("value");
            if (value == nullptr || !value->IsArray() || value->AsArray().size() < 2
                || !value->AsArray()[0].IsNumber() || !value->AsArray()[1].IsNumber()) {
                return Outcome::Err("BAD_ARG", "value=[x,y] が必要です");
            }
            const math::Vector2 vector{
                static_cast<float>(value->AsArray()[0].AsNumber()),
                static_cast<float>(value->AsArray()[1].AsNumber())
            };
            if (kind == "mousePosition") input::Input::InjectMousePosition(vector);
            else input::Input::InjectMouseDelta(vector);
            applied = true;
        } else if (kind == "mouseScroll") {
            const JsonValue* value = payload.Find("value");
            if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "value が必要です");
            input::Input::InjectMouseScroll(static_cast<float>(value->AsNumber()));
            applied = true;
        } else {
            return Outcome::Err("BAD_ARG", "未知の input kind: " + kind);
        }
        JsonValue result = JsonValue::MakeObject();
        result.Set("applied", JsonValue(applied));
        result.Set("kind", JsonValue(kind));
        return applied ? Outcome::Ok(std::move(result)) : Outcome::Err("INPUT_REJECTED", "入力値が範囲外です");
    }

    // Avatar Mask はアセットファイル操作。シーンの UndoStack には載せない。
    if (type == "avatarMask.write") return DoAvatarMaskWrite(ctx, payload, dryRun);

    if (type == "animation.control") return DoAnimationControl(ctx, payload, dryRun);

    if (type == "animation.setParameter") return DoAnimationSetParameter(ctx, payload, dryRun);

    if (type == "play.control") {
        if (ctx.playMode == nullptr || ctx.activeScene == nullptr) {
            return Outcome::Err("NO_PLAY_MODE", "PlayModeController または Scene が未設定です");
        }
        const std::string action = StringField(payload, "action");
        if (dryRun) return DryRunPreview(type + ":" + action);
        if (action == "start") {
            if (ctx.scriptReloadBusy) return Outcome::Err("SCRIPT_RELOAD_BUSY", "Script のビルドまたは再読み込み中です");
            if (!ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "Play は Editor 状態からのみ開始できます");
            ctx.playMode->Play(*ctx.activeScene);
        } else if (action == "stop") {
            if (ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "Play Mode は開始されていません");
            ctx.playMode->Stop(*ctx.activeScene);
        } else if (action == "pause") {
            if (!ctx.playMode->IsPlaying()) return Outcome::Err("INVALID_PLAY_STATE", "一時停止は Playing 状態でのみ実行できます");
            ctx.playMode->Pause();
        } else if (action == "resume") {
            if (!ctx.playMode->IsPaused()) return Outcome::Err("INVALID_PLAY_STATE", "再開は Paused 状態でのみ実行できます");
            ctx.playMode->Pause();
        } else if (action == "step") {
            if (!ctx.playMode->IsPaused()) return Outcome::Err("INVALID_PLAY_STATE", "ステップは Paused 状態でのみ実行できます");
            ctx.playMode->RequestStep();
        } else {
            return Outcome::Err("BAD_ARG", "未知の Play action: " + action);
        }
        JsonValue result = JsonValue::MakeObject();
        result.Set("action", JsonValue(action));
        result.Set("playState", JsonValue(PlayStateName(*ctx.playMode)));
        result.Set("restorePending", JsonValue(ctx.playMode->HasPendingRestore()));
        return Outcome::Ok(std::move(result));
    }

    if (type == "viewport.camera") {
        if (ctx.editorCamera == nullptr) return Outcome::Err("NO_CAMERA", "Scene View カメラが未設定です");
        math::Vector3 position;
        math::Vector3 lookAt;
        const bool hasPosition = ReadVec3(payload, "position", position);
        bool hasLookAt = ReadVec3(payload, "lookAt", lookAt);
        const std::string targetId = StringField(payload, "targetId");
        if (!targetId.empty()) {
            GameObject* target = ctx.activeScene != nullptr ? ctx.activeScene->FindByGuid(targetId) : nullptr;
            if (target == nullptr) return Outcome::Err("NODE_NOT_FOUND", "targetId が見つかりません: " + targetId);
            lookAt = target->transform.worldPosition;
            hasLookAt = true;
        }
        if (!hasPosition && !hasLookAt) return Outcome::Err("BAD_ARG", "position / lookAt / targetId のいずれかが必要です");
        if (dryRun) return DryRunPreview(type);
        if (hasPosition) ctx.editorCamera->m_position = position;
        if (hasLookAt) ctx.editorCamera->LookAt(lookAt);
        JsonValue result = JsonValue::MakeObject();
        result.Set("position", VectorToJson(ctx.editorCamera->m_position));
        result.Set("forward", VectorToJson(ctx.editorCamera->GetForward()));
        if (hasLookAt) result.Set("lookAt", VectorToJson(lookAt));
        return Outcome::Ok(std::move(result));
    }

    if (type == "editor.undo" || type == "editor.redo") {
        if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
        const bool isUndo = (type == "editor.undo");
        const bool can = isUndo ? ctx.undoStack->CanUndo() : ctx.undoStack->CanRedo();
        const std::string desc = isUndo ? ctx.undoStack->GetUndoDescription() : ctx.undoStack->GetRedoDescription();
        if (!can) {
            JsonValue result = JsonValue::MakeObject();
            result.Set("applied", JsonValue(false));
            return Outcome::Ok(std::move(result));
        }
        if (dryRun) {
            JsonValue result = JsonValue::MakeObject();
            result.Set("dryRun", JsonValue(true));
            result.Set("would", JsonValue(type));
            result.Set("description", JsonValue(desc));
            return Outcome::Ok(std::move(result));
        }
        if (isUndo) ctx.undoStack->Undo(); else ctx.undoStack->Redo();
        JsonValue result = JsonValue::MakeObject();
        result.Set("applied", JsonValue(true));
        result.Set("description", JsonValue(desc));
        return Outcome::Ok(std::move(result));
    }

    if (type == "selection.set") {
        scene::Scene* activeScene = ctx.activeScene;
        if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
        const JsonValue* ids = payload.Find("ids");
        if (ids == nullptr || !ids->IsArray()) return Outcome::Err("BAD_ARG", "ids 配列が必要です");
        std::vector<EntityID> resolved;
        int missing = 0;
        for (const JsonValue& idValue : ids->AsArray()) {
            if (!idValue.IsString()) continue;
            if (GameObject* go = activeScene->FindByGuid(idValue.AsString())) resolved.push_back(go->GetID());
            else ++missing;
        }
        if (dryRun) {
            JsonValue result = JsonValue::MakeObject();
            result.Set("dryRun", JsonValue(true));
            result.Set("resolved", JsonValue(static_cast<int>(resolved.size())));
            result.Set("missing", JsonValue(missing));
            return Outcome::Ok(std::move(result));
        }
        ctx.selectedEntities = std::move(resolved);
        JsonValue result = JsonValue::MakeObject();
        result.Set("selected", JsonValue(static_cast<int>(ctx.selectedEntities.size())));
        result.Set("missing", JsonValue(missing));
        return Outcome::Ok(std::move(result));
    }

    if (type == "asset.import") {
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
        const std::string src = StringField(payload, "src");
        const std::string dst = StringField(payload, "dst");
        if (src.empty() || dst.empty()) return Outcome::Err("BAD_ARG", "src / dst が必要です");
        std::error_code ec;
        const fs::path srcPath = fs::weakly_canonical(fs::path(src), ec);
        const fs::path dstPath = fs::weakly_canonical(fs::path(ctx.projectRoot) / fs::path(dst), ec);
        const fs::path rootPath = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
        if (ec) return Outcome::Err("BAD_PATH", "パス解決に失敗しました");
        // 取り込み先は projectRoot 配下限定 (取り込み元は任意許可)。
        if (dstPath.generic_string().rfind(rootPath.generic_string(), 0) != 0) {
            return Outcome::Err("BAD_PATH", "dst は projectRoot の外です");
        }
        if (!fs::exists(srcPath, ec) || fs::is_directory(srcPath, ec)) {
            return Outcome::Err("SRC_NOT_FOUND", "src ファイルがありません: " + src);
        }
        if (dryRun) {
            JsonValue result = JsonValue::MakeObject();
            result.Set("dryRun", JsonValue(true));
            result.Set("would", JsonValue("asset.import"));
            result.Set("dst", JsonValue(dstPath.generic_string()));
            return Outcome::Ok(std::move(result));
        }
        fs::create_directories(dstPath.parent_path(), ec);
        fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
        if (ec) return Outcome::Err("IMPORT_FAILED", "コピーに失敗しました: " + ec.message());
        ctx.requestAssetBrowserRefresh = true; // 取り込み後に AssetBrowser を更新させる
        JsonValue result = JsonValue::MakeObject();
        result.Set("imported", JsonValue(dst));
        return Outcome::Ok(std::move(result));
    }

    if (type == "editor.transaction") {
        if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
        const std::string label = StringField(payload, "label");
        const JsonValue* cmds = payload.Find("cmds");
        if (cmds == nullptr || !cmds->IsArray() || cmds->AsArray().empty()) {
            return Outcome::Err("BAD_ARG", "cmds 配列が必要です");
        }
        auto composite = std::make_unique<CompositeCommand>(label.empty() ? "AI: Transaction" : label);
        for (const JsonValue& sub : cmds->AsArray()) {
            if (!sub.IsObject()) return Outcome::Err("BAD_ARG", "cmds の要素がオブジェクトではありません");
            const std::string subType = StringField(sub, "t");
            Outcome subErr;
            std::unique_ptr<ICommand> subCommand = BuildCommand(ctx, subType, sub, subErr, nullptr);
            if (subCommand == nullptr) return subErr;
            composite->Add(std::move(subCommand));
        }
        if (dryRun) {
            JsonValue result = JsonValue::MakeObject();
            result.Set("dryRun", JsonValue(true));
            result.Set("would", JsonValue("editor.transaction"));
            result.Set("count", JsonValue(static_cast<int>(cmds->AsArray().size())));
            return Outcome::Ok(std::move(result));
        }
        ctx.undoStack->Execute(std::move(composite));
        JsonValue result = JsonValue::MakeObject();
        result.Set("committed", JsonValue(label));
        result.Set("count", JsonValue(static_cast<int>(cmds->AsArray().size())));
        return Outcome::Ok(std::move(result));
    }

    // 汎用 mutating Command。
    Outcome err;
    auto createdSink = (type == "node.create" || type == "node.duplicate" ||
                        type == "prefab.instantiate" || type == "prefab.create")
        ? std::make_shared<std::string>()
        : nullptr;
    JsonValue detail = JsonValue::MakeObject();
    std::unique_ptr<ICommand> command = BuildCommand(ctx, type, payload, err, createdSink, &detail);
    if (command == nullptr) return err;
    // dryRun でも副作用の予告は返す (適用前に改名や budget 引き上げを知れる方が意味がある)。
    if (dryRun) {
        Outcome preview = DryRunPreview(type);
        if (preview.ok && !detail.AsObject().empty()) preview.result.Set("detail", std::move(detail));
        return preview;
    }
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    ctx.undoStack->Execute(std::move(command));
    JsonValue result = JsonValue::MakeObject();
    result.Set("applied", JsonValue(true));
    result.Set("t", JsonValue(type));
    if (createdSink && !createdSink->empty()) result.Set("id", JsonValue(*createdSink));
    if (!detail.AsObject().empty()) result.Set("detail", std::move(detail));
    return Outcome::Ok(std::move(result));
}

} // namespace

EditorBusDispatcher::EditorBusDispatcher(editor::EditorContext& context) : m_context(context) {}

std::string EditorBusDispatcher::Handle(const std::string& requestLine)
{
    std::string parseError;
    std::optional<JsonValue> root = ParseJson(requestLine, &parseError);
    if (!root.has_value()) return {}; // 相関 id を取れない → 送信側の timeout に委ねる

    std::optional<BusRequest> request = ParseBusRequest(*root, &parseError);
    if (!request.has_value()) {
        // id が取れれば error 応答で明示する。
        const JsonValue* id = root->Find("id");
        if (id != nullptr && id->IsString()) {
            return SerializeJson(MakeErrorResponse(id->AsString(), "BAD_REQUEST", parseError));
        }
        return {};
    }

    const std::string type = request->PayloadType();
    const JsonValue&  payload = request->payload;

    // Scene 依存の操作はアクティブシーンを要求する。
    auto* activeScene = m_context.activeScene;

    Outcome outcome = Outcome::Err("UNKNOWN", "未対応の要求です");

    // ── Query (副作用なし) ────────────────────────────────────────────────
    if (request->IsQuery()) {
        if (type == "editor.catalog") {
            outcome = Outcome::Ok(BuildEditorCatalog());
        } else if (type == "editor.catalog.search") {
            outcome = SearchEditorCatalog(payload);
        } else if (type == "editor.state") {
            outcome = DoEditorState(m_context);
        } else if (type == "editor.undoHistory") {
            outcome = DoUndoHistory(m_context, payload);
        } else if (type == "console.logs") {
            outcome = DoConsoleLogs(m_context, payload);
        } else if (type == "scene.tree") {
            if (activeScene == nullptr) { outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); }
            else {
                // includeGenerated: システムが実行時に作った GO (VFX Graph のノード実体、
                // Foliage bake、Water splash) をツリーへ含めるか。既定は含めない。
                // WHY: 爆発を 5 箇所に置いて再生すれば、それだけで数十ノードが増える。
                //      AI は「編集できるオブジェクト」を探してツリーを読むが、生成物は
                //      編集しても保存されない (SceneSerializer が捨てる) ため、
                //      混ぜると context を食い潰したうえで無駄な編集を誘発する。
                //      実行状態を調べたいときだけ明示的に要求させる。
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
                        // 生成物を含める指定のときだけ印を付ける。既定の応答では常に
                        // false になるフィールドなので、出さないことで冗長さを避ける。
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
                        // 「出ていない = 存在しない」と読ませないための件数。
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
        } else if (type == "scene.find") {
            if (activeScene == nullptr) outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
            else outcome = FindSceneNodes(*activeScene, payload);
        } else if (type == "scene.snapshot") {
            if (activeScene == nullptr) outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
            else outcome = Outcome::Ok(BuildSceneSnapshot(*activeScene));
        } else if (type == "scene.validate") {
            outcome = DoSceneValidate(m_context);
        } else if (type == "scene.selection") {
            JsonValue ids = JsonValue::MakeArray();
            if (activeScene != nullptr) {
                for (EntityID id : m_context.selectedEntities) {
                    if (GameObject* go = activeScene->GetGameObject(id)) ids.Push(JsonValue(go->instanceId));
                }
            }
            JsonValue result = JsonValue::MakeObject();
            result.Set("ids", std::move(ids));
            outcome = Outcome::Ok(std::move(result));
        } else if (type == "node.components") {
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
        } else if (type == "asset.list") {
            outcome = DoAssetList(m_context, payload);
        } else if (type == "asset.inspect") {
            outcome = DoAssetInspect(m_context, payload);
        } else if (type == "asset.findUnused") {
            outcome = DoAssetFindUnused(m_context, payload);
        } else if (type == "asset.thumbnail") {
            outcome = DoAssetThumbnail(m_context, payload);
        } else if (type == "vfx.graph") {
            outcome = DoVFXGraphInspect(payload);
        } else if (type == "vfx.lint") {
            outcome = DoVFXLint(m_context, payload);
        } else if (type == "bt.tree") {
            outcome = DoBehaviorTree(m_context, payload);
        } else if (type == "bt.lint") {
            outcome = DoBehaviorTreeLint(m_context, payload);
        } else if (type == "bt.guide") {
            outcome = DoBehaviorTreeGuide();
        } else if (type == "vfx.guide") {
            outcome = DoVFXGuide();
        } else if (type == "vfx.templateCatalog") {
            outcome = DoVFXTemplateCatalog(m_context, payload);
        } else if (type == "vfx.curvePresets") {
            outcome = DoVFXCurvePresets();
        } else if (type == "vfx.diff") {
            outcome = DoVFXDiff(payload);
        } else if (type == "vfx.params") {
            outcome = DoVFXParams(m_context, payload);
        } else if (type == "vfx.schema") {
            outcome = DoVFXSchema();
        } else if (type == "vfx.nodeField") {
            outcome = DoVFXNodeField(payload);
        } else if (type == "vfx.previewEnsure") {
            outcome = DoVFXPreviewEnsure(m_context);
        } else if (type == "vfx.runtime") {
            outcome = DoVFXRuntime(m_context);
        } else if (type == "vfx.textureAnalyze") {
            outcome = DoVFXTextureAnalyze(m_context, payload);
        } else if (type == "vfx.materialAnalyze") {
            outcome = DoVFXMaterialAnalyze(m_context, payload);
        } else if (type == "vfx.assetSurvey") {
            outcome = DoVFXAssetSurvey(m_context, payload);
    } else if (type == "shader.inspect") {
        outcome = DoShaderInspect(m_context, payload);
    } else if (type == "shader.diagnostics") {
        outcome = DoShaderCompileDiagnostics();
        } else if (type == "vfx.preview") {
            outcome = DoVFXPreview(m_context, payload);
        } else if (type == "vfx.previewMetrics") {
            // 前回との比較対象を取り違えないよう、同じ .vfx / 同じ view のときだけ動き指標を出す。
            // 別のエフェクトへ切り替えた直後の「大きく変わった」は意味を持たない。
            // 解像度違いは ComputePreviewMetrics が画素数の不一致として弾く。
            const std::string key = StringField(payload, "path") + "|"
                + LowerAscii(StringField(payload, "view"));
            const bool comparable = !m_previousPreviewKey.empty() && m_previousPreviewKey == key;
            std::vector<float> luminance;
            outcome = DoVFXPreviewMetrics(m_context, m_vfxPreviewRT,
                                          comparable ? &m_previousPreviewLuminance : nullptr,
                                          luminance);
            if (outcome.ok) {
                m_previousPreviewLuminance = std::move(luminance);
                m_previousPreviewKey = key;
            }
        } else if (type == "material.inspect") {
            outcome = DoMaterialInspect(m_context, payload);
        } else if (type == "animation.state") {
            outcome = DoAnimationState(m_context, payload);
        } else if (type == "avatarMask.get") {
            outcome = DoAvatarMaskGet(m_context, payload);
        } else if (type == "animation.graph") {
            outcome = DoAnimationGraph(m_context, payload);
        } else if (type == "animation.blendTree") {
            outcome = DoAnimationBlendTree(m_context, payload);
        } else if (type == "animation.pose") {
            outcome = DoAnimationPose(m_context, payload);
        } else if (type == "profiler.snapshot") {
            outcome = DoProfilerSnapshot(m_context, payload);
        } else if (type == "physics.raycast") {
            outcome = DoPhysicsRaycast(m_context, payload);
        } else if (type == "physics.overlapSphere") {
            outcome = DoPhysicsOverlapSphere(m_context, payload);
        } else if (type == "physics.events") {
            outcome = DoPhysicsEvents(m_context);
        } else if (type == "viewport.capture") {
            std::string view = StringField(payload, "view");
            if (view.empty()) view = "scene";
            if (view != "scene" && view != "game" && view != "vfx") {
                outcome = Outcome::Err("BAD_ARG", "view は scene、game、vfx のいずれかで指定してください");
            } else {
                const auto target = view == "game" ? m_gameViewportRT
                    : (view == "vfx" ? m_vfxPreviewRT : m_sceneViewportRT);
                outcome = DoViewportCapture(m_context, target);
                if (outcome.ok) outcome.result.Set("view", JsonValue(view));
            }
        } else if (type == "viewport.semantic") {
            std::string view = StringField(payload, "view");
            if (view.empty()) view = "scene";
            if (view != "scene" && view != "game") {
                outcome = Outcome::Err("BAD_ARG", "view は scene または game で指定してください");
            } else if (view == "scene" && m_context.editorCamera == nullptr) {
                outcome = Outcome::Err("NO_CAMERA", "Scene Viewカメラが未設定です");
            } else if (view == "scene") {
                outcome = DoSemanticViewportCapture(m_context,
                    m_sceneViewportRT, *m_context.editorCamera, "scene");
            } else if (m_context.activeScene == nullptr) {
                outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
            } else {
                renderer::Camera gameCamera;
                if (!BuildGameViewportCamera(m_context, m_gameViewportRT, gameCamera)) {
                    outcome = Outcome::Err("NO_CAMERA", "Game ViewカメラまたはRenderTargetが未設定です");
                } else {
                    outcome = DoSemanticViewportCapture(m_context, m_gameViewportRT, gameCamera, "game");
                }
            }
        } else {
            outcome = Outcome::Err("UNKNOWN_QUERY", "未対応の Query: " + type);
        }
    }
    // ── Command (Undo 可能に適用 / dryRun は試算のみ) ─────────────────────
    else {
        outcome = DoCommand(m_context, type, payload, request->dryRun);
    }

    if (outcome.ok) return SerializeJson(MakeOkResponse(request->id, std::move(outcome.result)));
    return SerializeJson(MakeErrorResponse(request->id, outcome.code, outcome.message));
}

} // namespace fbzz::editor::ai
