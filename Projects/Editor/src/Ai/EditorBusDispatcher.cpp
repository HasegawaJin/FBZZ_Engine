// FBZZ Engine
// EditorBusDispatcher.cpp | fbzz::editor::ai
// Editor Command Bus のメインスレッド処理。Query/Command を Scene・UndoStack・Renderer へ写像する。
#include <Editor/Ai/EditorBusDispatcher.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXParameterRuntime.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
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
        for (std::uint32_t index = 0; index < (std::min)(curve->curve.keyCount, 4u); ++index) {
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
        for (std::uint32_t index = 0; index < (std::min)(gradient->gradient.keyCount, 4u); ++index) {
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
        output.Push(std::move(item));
    }
}

Outcome DoVFXGraphInspect(const JsonValue& payload)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");
    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::ParseVFXGraphAsset(path, graph, &error))
        return Outcome::Err("VFX_INVALID", error);
    const bool valid = asset::ValidateVFXGraphAsset(graph, &error);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("name", JsonValue(graph.name));
    result.Set("version", JsonValue(graph.version));
    result.Set("valid", JsonValue(valid));
    result.Set("validationError", JsonValue(valid ? std::string{} : error));
    JsonValue nodes = JsonValue::MakeArray();
    for (const auto& node : graph.nodes) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(node.id));
        item.Set("type", JsonValue(asset::VFXNodeTypeName(node.type)));
        item.Set("name", JsonValue(node.name));
        item.Set("startOffset", JsonValue(node.startOffset));
        item.Set("duration", JsonValue(node.duration));
        if (node.type == asset::VFXNodeType::Particle) {
            item.Set("maxParticles", JsonValue(node.particle.maxParticles));
            item.Set("simulation", JsonValue(node.particle.simulationMode == scene::ParticleSimulationMode::Gpu
                ? "GPU" : "CPU"));
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
    JsonValue signalNodes = JsonValue::MakeArray();
    for (const auto& signal : graph.signalNodes) {
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

Outcome DoVFXSchema()
{
    JsonValue fields = JsonValue::MakeArray();
    AppendSchemaProperties(fields, asset::GetVFXNodeSchema(), {});
    JsonValue nodeTypes = JsonValue::MakeArray();
    for (const auto type : { asset::VFXNodeType::Entry, asset::VFXNodeType::Delay,
            asset::VFXNodeType::Particle, asset::VFXNodeType::Trail, asset::VFXNodeType::MeshTrail,
            asset::VFXNodeType::Light, asset::VFXNodeType::Audio, asset::VFXNodeType::Decal,
            asset::VFXNodeType::SubGraph })
        nodeTypes.Push(JsonValue(asset::VFXNodeTypeName(type)));
    JsonValue result = JsonValue::MakeObject();
    result.Set("schema", JsonValue(std::string(asset::GetVFXNodeSchema().TypeName())));
    result.Set("nodeTypes", std::move(nodeTypes));
    result.Set("fields", std::move(fields));
    return Outcome::Ok(std::move(result));
}

Outcome DoVFXPreview(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.vfxPreviewScene == nullptr) return Outcome::Err("NO_PREVIEW_WORLD", "VFX Preview Worldがありません");
    const std::string path = StringField(payload, "path");
    const JsonValue* timeValue = payload.Find("time");
    if (path.empty() || timeValue == nullptr || !timeValue->IsNumber())
        return Outcome::Err("BAD_ARG", "pathとtimeが必要です");
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
    for (GameObject& go : activeScene.GameObjects()) {
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
    result.Set("count", JsonValue(static_cast<int>(activeScene.GameObjectCount())));
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
Outcome DoAnimationGraph(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    JsonValue parameters = JsonValue::MakeArray();
    for (const auto& param : animator->parameters) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(param.name));
        entry.Set("type", JsonValue(AnimatorParamTypeName(param.type)));
        SetAnimatorParamValueJson(entry, param);
        parameters.Push(std::move(entry));
    }

    // 実効デフォルトステート名 (未指定時は先頭)。Entry リンクの解決規則と一致させる。
    std::string defaultState = animator->defaultStateName;
    if (defaultState.empty() && !animator->states.empty())
        defaultState = animator->states.front().name;

    JsonValue states = JsonValue::MakeArray();
    for (const auto& state : animator->states) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(state.name));
        entry.Set("mode", JsonValue(AnimationStateModeName(state.mode)));
        entry.Set("clipName", JsonValue(state.clipName));
        entry.Set("sourcePath", JsonValue(state.sourcePath));
        entry.Set("speed", JsonValue(state.speed));
        entry.Set("loop", JsonValue(state.loop));
        entry.Set("ikWeight", JsonValue(state.ikWeight));
        entry.Set("isDefault", JsonValue(state.name == defaultState));
        entry.Set("isCurrent", JsonValue(state.name == animator->currentStateName));
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
    for (const auto& transition : animator->anyStateTransitions)
        anyStateTransitions.Push(TransitionToJson(transition));

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("controllerPath", JsonValue(animator->controllerPath));
    result.Set("defaultState", JsonValue(defaultState));
    result.Set("currentState", JsonValue(animator->currentStateName));
    result.Set("blendToState", JsonValue(animator->blendToState));
    result.Set("blendWeight", JsonValue(animator->blendWeight));
    result.Set("parameters", std::move(parameters));
    result.Set("states", std::move(states));
    result.Set("anyStateTransitions", std::move(anyStateTransitions));
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
    return std::make_unique<LambdaCommand>(description,
        [scene, id, edit, beforeStates, beforeAny, beforeParams, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            *beforeStates = a->states;
            *beforeAny = a->anyStateTransitions;
            *beforeParams = a->parameters;
            edit(*a);
            markDirty();
        },
        [scene, id, beforeStates, beforeAny, beforeParams, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            a->states = *beforeStates;
            a->anyStateTransitions = *beforeAny;
            a->parameters = *beforeParams;
            a->currentStateName.clear();
            a->blendToState.clear();
            a->stateTime = 0.0f;
            a->blendWeight = 0.0f;
            markDirty();
        });
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
    return std::nullopt;
}

bool JsonToSchemaValue(const JsonValue& json, reflection::PropertyType type, std::any& output)
{
    if (type == reflection::PropertyType::Float && json.IsNumber()) output = static_cast<float>(json.AsNumber());
    else if (type == reflection::PropertyType::Int && json.IsNumber()) output = json.AsInt();
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
            curve.curve.keyCount = static_cast<std::uint32_t>((std::min)(keys->AsArray().size(), std::size_t{4}));
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
            gradient.gradient.keyCount = static_cast<std::uint32_t>((std::min)(keys->AsArray().size(), std::size_t{4}));
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
    if (type == "vfx.node.add") {
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
    } else if (type == "vfx.link.add") {
        const int from = payload.Find("from") != nullptr ? payload.Find("from")->AsInt() : 0;
        const int to = payload.Find("to") != nullptr ? payload.Find("to")->AsInt() : 0;
        asset::VFXLinkTrigger trigger = asset::VFXLinkTrigger::OnComplete;
        const std::string triggerName = LowerAscii(StringField(payload, "trigger"));
        if (triggerName == "onstart") trigger = asset::VFXLinkTrigger::OnStart;
        else if (triggerName == "oncollision") trigger = asset::VFXLinkTrigger::OnCollision;
        else if (triggerName == "ondeath") trigger = asset::VFXLinkTrigger::OnDeath;
        const float delay = payload.Find("delay") != nullptr
            ? static_cast<float>(payload.Find("delay")->AsNumber()) : 0.0f;
        newGraph.links.push_back({ from, to, trigger, delay });
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
    } else if (type == "vfx.param.bind") {
        newGraph.bindings.push_back({ StringField(payload, "name"),
            payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0,
            StringField(payload, "schemaPath") });
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
        const int targetParticles = payload.Find("targetParticles") != nullptr
            ? payload.Find("targetParticles")->AsInt() : 0;
        if (targetParticles < 1) { err = Outcome::Err("BAD_ARG", "targetParticlesは1以上です"); return nullptr; }
        const auto oldBudget = asset::CalculateVFXGraphBudget(newGraph);
        const float scale = oldBudget.particles > 0
            ? (std::min)(1.0f, static_cast<float>(targetParticles) / oldBudget.particles) : 1.0f;
        std::unordered_set<int> eventSources;
        for (const auto& link : newGraph.links)
            if (link.trigger == asset::VFXLinkTrigger::OnCollision || link.trigger == asset::VFXLinkTrigger::OnDeath)
                eventSources.insert(link.fromNode);
        for (auto& node : newGraph.nodes) {
            if (node.type != asset::VFXNodeType::Particle) continue;
            node.particle.maxParticles = (std::max)(1, static_cast<int>(std::round(node.particle.maxParticles * scale)));
            node.particle.emitRate *= scale;
            node.particle.lodEnabled = true;
            node.particle.lodNearRateScale = 1.0f;
            node.particle.lodFarRateScale = (std::min)(node.particle.lodFarRateScale, 0.35f);
            if (!eventSources.contains(node.id)
                && node.particle.collisionMode == scene::ParticleCollisionMode::None
                && node.particle.meshParticlePath.empty())
                node.particle.simulationMode = scene::ParticleSimulationMode::Gpu;
        }
        newGraph.maxParticles = (std::max)(targetParticles, asset::CalculateVFXGraphBudget(newGraph).particles);
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
    } else return nullptr;

    if (!asset::ValidateVFXGraphAsset(newGraph, &error)) {
        err = Outcome::Err("VFX_VALIDATION", error);
        return nullptr;
    }
    editor::EditorContext* context = &ctx;
    return std::make_unique<LambdaCommand>("AI: Edit VFX Graph",
        [context, path, newGraph]() {
            if (asset::SaveVFXGraphAsset(path, newGraph)) context->requestAssetBrowserRefresh = true;
        },
        [context, path, oldGraph]() {
            if (asset::SaveVFXGraphAsset(path, oldGraph)) context->requestAssetBrowserRefresh = true;
        });
}

// 1つの mutating Command を Undo 可能な ICommand へ変換する (実行はしない)。失敗時 nullptr + err。
// createdSink != nullptr のとき生成系 Command は代表ルートの instanceId をそこへ書き込む。
std::unique_ptr<ICommand> BuildCommand(editor::EditorContext& ctx, const std::string& type,
                                       const JsonValue& payload, Outcome& err,
                                       std::shared_ptr<std::string> createdSink)
{
    if (type == "vfx.template.apply") {
        namespace fs = std::filesystem;
        if (ctx.projectRoot.empty()) { err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です"); return nullptr; }
        const std::string templateName = StringField(payload, "template");
        const std::string destination = StringField(payload, "path");
        if (templateName.empty() || destination.empty()) {
            err = Outcome::Err("BAD_ARG", "template / path が必要です"); return nullptr;
        }
        const std::string lowerTemplate = LowerAscii(templateName);
        static const std::unordered_map<std::string, std::string> BUILTIN_TEMPLATES = {
            { "explosion", "Explosion.vfx" }, { "fire", "Fire.vfx" }, { "smoke", "Smoke.vfx" },
            { "impact", "Impact.vfx" }, { "magic", "Magic.vfx" },
        };
        const auto builtin = BUILTIN_TEMPLATES.find(lowerTemplate);
        const fs::path sourceRelative = builtin != BUILTIN_TEMPLATES.end()
            ? fs::path("Assets/VFX/Templates") / builtin->second : fs::path(templateName);
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
        fs::path source = fs::weakly_canonical(root / sourceRelative, ec);
        if (builtin != BUILTIN_TEMPLATES.end() && !fs::exists(source, ec) && !ctx.engineRoot.empty())
            source = fs::weakly_canonical(fs::path(ctx.engineRoot) / "Assets/VFX/Templates" / builtin->second, ec);
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
        const std::string requestedName = StringField(payload, "name");
        if (!requestedName.empty()) templateGraph.name = requestedName;
        const bool existed = fs::exists(target, ec);
        asset::VFXGraphAsset previous;
        if (existed && !asset::LoadVFXGraphAsset(target.generic_string(), previous, &loadError)) {
            err = Outcome::Err("VFX_DEST_INVALID", loadError); return nullptr;
        }
        editor::EditorContext* context = &ctx;
        const std::string targetString = target.generic_string();
        return std::make_unique<LambdaCommand>("AI: Apply VFX Template",
            [context, targetString, templateGraph]() {
                std::error_code createError;
                std::filesystem::create_directories(std::filesystem::path(targetString).parent_path(), createError);
                if (asset::SaveVFXGraphAsset(targetString, templateGraph)) context->requestAssetBrowserRefresh = true;
            },
            [context, targetString, existed, previous]() {
                if (existed) asset::SaveVFXGraphAsset(targetString, previous);
                else { std::error_code removeError; std::filesystem::remove(targetString, removeError); }
                context->requestAssetBrowserRefresh = true;
            });
    }
    // .vfx asset編集はメインSceneを必要としない。独立VFX Editorだけ開いた状態でもAI編集を許可する。
    if (type.starts_with("vfx.node.") || type.starts_with("vfx.link.")
        || type.starts_with("vfx.param.") || type == "vfx.optimize"
        || type == "vfx.variant.upsert")
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

        auto findStateIndex = [&](const std::string& name) -> int {
            for (int i = 0; i < static_cast<int>(animator->states.size()); ++i)
                if (animator->states[static_cast<size_t>(i)].name == name) return i;
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
                [stateName](scene::AnimatorComponent& a) {
                    // 対象ステートを消し、他ステート/AnyState からの遷移参照も除去する (パネルの DeleteState 相当)。
                    a.states.erase(
                        std::remove_if(a.states.begin(), a.states.end(),
                            [&](const scene::AnimationState& s) { return s.name == stateName; }),
                        a.states.end());
                    for (auto& s : a.states) {
                        s.transitions.erase(
                            std::remove_if(s.transitions.begin(), s.transitions.end(),
                                [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                            s.transitions.end());
                    }
                    a.anyStateTransitions.erase(
                        std::remove_if(a.anyStateTransitions.begin(), a.anyStateTransitions.end(),
                            [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                        a.anyStateTransitions.end());
                    if (a.defaultStateName == stateName)
                        a.defaultStateName = a.states.empty() ? std::string{} : a.states.front().name;
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
                [prototype, setAsDefault](scene::AnimatorComponent& a) {
                    a.states.push_back(prototype);
                    // Unity 同様、最初のステートや明示指定時はデフォルトにする。
                    if (setAsDefault || a.defaultStateName.empty())
                        a.defaultStateName = prototype.name;
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
                [stateName, newName, hasMode, mode, hasBlend2DType, blend2DType, setAsDefault, captured]
                (scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(a.states.size()); ++i)
                        if (a.states[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = a.states[static_cast<size_t>(index)];
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
                    if (setAsDefault) a.defaultStateName = s.name;
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
            const scene::AnimationState& state = animator->states[static_cast<size_t>(stateIndex)];
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
                [stateName, is2D, isAdd, isRemove, motionIndex, captured](scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(a.states.size()); ++i)
                        if (a.states[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = a.states[static_cast<size_t>(index)];
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
            return isAnyState ? animator->anyStateTransitions
                              : animator->states[static_cast<size_t>(fromIndex)].transitions;
        };

        // ── 遷移削除 ──
        if (type == "animation.removeTransition") {
            const JsonValue* transitionIndexField = payload.Find("transitionIndex");
            if (transitionIndexField == nullptr || !transitionIndexField->IsNumber()) { err = Outcome::Err("BAD_ARG", "transitionIndex が必要です"); return nullptr; }
            const int transitionIndex = transitionIndexField->AsInt();
            if (transitionIndex < 0 || transitionIndex >= static_cast<int>(sourceTransitions().size())) { err = Outcome::Err("TRANSITION_NOT_FOUND", "transitionIndex が範囲外です"); return nullptr; }
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Transition", markDirty,
                [isAnyState, fromName, transitionIndex](scene::AnimatorComponent& a) {
                    std::vector<scene::AnimationTransition>* transitions = nullptr;
                    if (isAnyState) transitions = &a.anyStateTransitions;
                    else for (auto& s : a.states) if (s.name == fromName) { transitions = &s.transitions; break; }
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
                [isAnyState, fromName, prototype](scene::AnimatorComponent& a) {
                    if (isAnyState) { a.anyStateTransitions.push_back(prototype); }
                    else {
                        for (auto& s : a.states)
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
            [isAnyState, fromName, transitionIndex, action, prototype, conditionIndex]
            (scene::AnimatorComponent& a) {
                std::vector<scene::AnimationTransition>* transitions = nullptr;
                if (isAnyState) transitions = &a.anyStateTransitions;
                else for (auto& s : a.states) if (s.name == fromName) { transitions = &s.transitions; break; }
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
    std::unique_ptr<ICommand> command = BuildCommand(ctx, type, payload, err, createdSink);
    if (command == nullptr) return err;
    if (dryRun) return DryRunPreview(type);
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    ctx.undoStack->Execute(std::move(command));
    JsonValue result = JsonValue::MakeObject();
    result.Set("applied", JsonValue(true));
    result.Set("t", JsonValue(type));
    if (createdSink && !createdSink->empty()) result.Set("id", JsonValue(*createdSink));
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
                // ルート GO から再帰的に {id,name,tag,active,layer,children} を構築する。
                struct Builder {
                    static JsonValue Node(GameObject* go) {
                        JsonValue node = JsonValue::MakeObject();
                        node.Set("id", JsonValue(go->instanceId));
                        node.Set("name", JsonValue(go->name));
                        node.Set("tag", JsonValue(go->tag));
                        node.Set("active", JsonValue(go->activeSelf()));
                        node.Set("layer", JsonValue(go->layer));
                        JsonValue children = JsonValue::MakeArray();
                        for (int i = 0; i < go->GetChildCount(); ++i) {
                            if (GameObject* child = go->GetChild(i)) children.Push(Node(child));
                        }
                        node.Set("children", std::move(children));
                        return node;
                    }
                };
                JsonValue roots = JsonValue::MakeArray();
                for (GameObject* go : activeScene->GetRootGameObjects()) {
                    if (go != nullptr) roots.Push(Builder::Node(go));
                }
                JsonValue result = JsonValue::MakeObject();
                result.Set("roots", std::move(roots));
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
        } else if (type == "vfx.params") {
            outcome = DoVFXParams(m_context, payload);
        } else if (type == "vfx.schema") {
            outcome = DoVFXSchema();
        } else if (type == "vfx.preview") {
            outcome = DoVFXPreview(m_context, payload);
        } else if (type == "material.inspect") {
            outcome = DoMaterialInspect(m_context, payload);
        } else if (type == "animation.state") {
            outcome = DoAnimationState(m_context, payload);
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
