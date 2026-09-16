/// @file    EditorBusDispatcher.cpp
/// @brief   Editor Command Bus のメインスレッド処理。Query/Command を Scene・UndoStack・Renderer へ写像する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/EditorBusDispatcher.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/Ai/PreviewMetrics.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/FluidDocument.hpp>
// Add Object プリセットは Hierarchy メニューと同じ登録表を共有する。
#include <Editor/Util/ObjectPresets.hpp>
// パーティクルの見た目は .mat 側にあるため、診断は素材を解決してから答える。
#include <Editor/Util/ParticleMaterialFactory.hpp>
// Terrain ブラシは対話ツール (TerrainTool) と同じカーネルを叩く。
#include <Tools/TerrainBrush.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
// Sprite の切り直しは Sprite Editor と同じ実装を共有する (ID の引き継ぎ規則を割らない)。
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphEditor/GraphSubgraphOps.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/AI/BehaviorTreeTypes.hpp>
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
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/TextureAnalysis.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ParticleCurvePresets.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/BehaviorTreeComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Systems/NavMeshBakeSystem.hpp>
#include <Engine/Scene/Systems/NavMeshQuery.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

// Sprite のコマ切り抜き (sprite.thumbnail)。stb は StbImage.cpp が実装を持ち、
// miniz は CMakeLists.txt が C ソースとして別コンパイルしている。
#include <stb_image.h>
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
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

// OperatorBridge の結果を Outcome へ詰め替える。
// WHY: Outcome はこの翻訳単位に閉じた型で、Operator 層 (Editor/Op) と AI 層の
//      両方から見える場所へ置くと依存が広がる。境界で 1 度だけ変換する。
Outcome FromBridge(OperatorBridgeResult bridge)
{
    if (bridge.ok) return Outcome::Ok(std::move(bridge.result));
    return Outcome::Err(std::move(bridge.code), std::move(bridge.message));
}

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

// projectRoot 配下のファイルだけを許す解決。定義はこのファイルの後方にあるため前方宣言する。
bool ResolveProjectFile(const editor::EditorContext& ctx, const std::string& requested,
                        std::filesystem::path& outPath, std::string& outRelative);

// ── Behavior Tree ───────────────────────────────────────────────────────────
// bt.tree / bt.guide / bt.lint / bt.node.* を vfx.* と同じ語彙で揃える。
// 「読む → 構造を知る → 規約を読む → 編集 → 検証」の流れは VFX グラフと同一のため。

// ── BT ノードのフィールド目録 ───────────────────────────────────────────────
// 受理集合を 1 つの表にして bt.schema で公開し、書き込み時も同じ表で弾く。
// 種別を見ずに代入すると Wait へ range を書いても保存まで通り、
// 「実行しても行動が変わらない」としか見えない誤りになる。
// appliesTo は **ランタイムが実際に読むか** で決める。Inspector の見た目ではない
// (turnSpeedDeg は LookAt しか読まず、range は IsTargetInRange しか読まない)。
struct BTFieldSpec {
    const char* name;
    const char* type;         // "float" | "int" | "bool" | "string" | "enum"
    const char* description;
    const char* enumValues;   // "" 以外なら | 区切りの受理値
    float       minValue;     // minValue == maxValue なら範囲指定なし
    float       maxValue;
};

const BTFieldSpec kBTFieldSpecs[] = {
    { "name", "string", "表示名。空ならノード種別名が使われる", "", 0.0f, 0.0f },
    { "editorX", "float", "エディタ Canvas 上の X 座標 (実行には影響しない)", "", 0.0f, 0.0f },
    { "editorY", "float", "エディタ Canvas 上の Y 座標 (実行には影響しない)", "", 0.0f, 0.0f },
    { "abortMode", "enum",
      "Running 中の枝を中断する条件。lowerPriority が BT の中断機構の本体",
      "none|self|lowerPriority|both", 0.0f, 0.0f },
    { "duration", "float", "待機 / クールダウン / 制限時間 [s]", "", 0.0f, 600.0f },
    { "durationRandom", "float",
      "duration へ加える ±ランダム幅 [s]。0 だと同時スポーンした個体の待機が完全に同期する",
      "", 0.0f, 60.0f },
    { "repeatCount", "int", "繰り返し回数。0 = 無限", "", 0.0f, 0.0f },
    { "repeatUntilFailure", "bool", "Failure が返るまで繰り返す", "", 0.0f, 0.0f },
    { "successPolicy", "enum", "Parallel の成功条件", "requireOne|requireAll", 0.0f, 0.0f },
    { "keyName", "string", "参照する Blackboard キー名 (bt.tree の blackboard に実在するもの)",
      "", 0.0f, 0.0f },
    { "compareOp", "enum", "比較演算子", "==|!=|<|<=|>|>=", 0.0f, 0.0f },
    { "withinSeconds", "float", "「N 秒以内に書かれた値か」も条件に加える。0 = 時間条件なし",
      "", 0.0f, 60.0f },
    { "valueBool", "bool", "比較 / 代入する値 (Bool キー)", "", 0.0f, 0.0f },
    { "valueInt", "int", "比較 / 代入する値 (Int キー)", "", 0.0f, 0.0f },
    { "valueFloat", "float", "比較 / 代入する値 (Float キー)", "", 0.0f, 0.0f },
    { "valueString", "string", "比較 / 代入する値 (String キー)", "", 0.0f, 0.0f },
    { "moveTargetKey", "string", "移動目標を持つ Blackboard キー (Vector3 か Entity)",
      "", 0.0f, 0.0f },
    { "acceptanceRadius", "float", "到達とみなす距離 [m]", "", 0.0f, 20.0f },
    { "chaseEntity", "bool",
      "true なら Entity を追跡し続ける。false なら一度だけ目的地へ向かう", "", 0.0f, 0.0f },
    { "repathInterval", "float", "経路再計算の間隔 [s]", "", 0.0f, 5.0f },
    { "range", "float", "範囲内とみなす距離 [m]", "", 0.0f, 200.0f },
    { "turnSpeedDeg", "float", "旋回速度 [deg/s]", "", 0.0f, 3600.0f },
    { "animatorTrigger", "string", "Animator へ送るトリガー名", "", 0.0f, 0.0f },
    { "waitForAnimation", "bool", "再生完了まで Running を維持する (段階 3 では未対応)",
      "", 0.0f, 0.0f },
    { "soundPath", "string", "再生する音声アセットのパス", "", 0.0f, 0.0f },
    { "volume", "float", "音量", "", 0.0f, 2.0f },
    { "scriptMethod", "string", "呼び出すスクリプトのメソッド名", "", 0.0f, 0.0f },
    { "threshold01", "float", "HP 閾値 [0,1]", "", 0.0f, 1.0f },
};

const BTFieldSpec* FindBTFieldSpec(std::string_view field)
{
    for (const BTFieldSpec& spec : kBTFieldSpecs)
        if (field == spec.name) return &spec;
    return nullptr;
}

// そのフィールドをその種別のランタイムが読むか。
bool BTFieldAppliesTo(std::string_view field, fbzz::ai::BTNodeType type)
{
    using T = fbzz::ai::BTNodeType;
    if (field == "name" || field == "editorX" || field == "editorY") return true;
    if (field == "abortMode")
        return fbzz::ai::BTNodeIsPureCondition(type) || type == T::BlackboardCondition;
    if (field == "duration" || field == "durationRandom")
        return type == T::Wait || type == T::Cooldown || type == T::TimeLimit;
    if (field == "repeatCount" || field == "repeatUntilFailure") return type == T::Repeat;
    if (field == "successPolicy") return type == T::Parallel;
    if (field == "keyName" || field == "compareOp" || field == "withinSeconds"
        || field == "valueBool" || field == "valueInt" || field == "valueFloat"
        || field == "valueString")
        return type == T::BlackboardCondition || type == T::BlackboardCompare
            || type == T::SetBlackboard;
    if (field == "moveTargetKey" || field == "acceptanceRadius" || field == "chaseEntity"
        || field == "repathInterval") return type == T::MoveTo;
    if (field == "range") return type == T::IsTargetInRange;
    if (field == "turnSpeedDeg") return type == T::LookAt;
    if (field == "animatorTrigger" || field == "waitForAnimation") return type == T::PlayAnimation;
    if (field == "soundPath" || field == "volume") return type == T::PlayAudio;
    if (field == "scriptMethod") return type == T::RunScript;
    if (field == "threshold01") return type == T::IsHealthBelow;
    return false;
}

// ノードの現在値を JSON へ。bt.node.setField の value と同じ表現で返すので、
// 読んで一部だけ変えて書き戻せる。
JsonValue BTFieldValueJson(const fbzz::ai::BTNodeDef& node, std::string_view field)
{
    if (field == "name") return JsonValue(node.name);
    if (field == "editorX") return JsonValue(node.editorX);
    if (field == "editorY") return JsonValue(node.editorY);
    if (field == "abortMode") {
        const char* names[] = { "none", "self", "lowerPriority", "both" };
        return JsonValue(std::string(names[static_cast<int>(node.abortMode)]));
    }
    if (field == "duration") return JsonValue(node.duration);
    if (field == "durationRandom") return JsonValue(node.durationRandom);
    if (field == "repeatCount") return JsonValue(node.repeatCount);
    if (field == "repeatUntilFailure") return JsonValue(node.repeatUntilFailure);
    if (field == "successPolicy")
        return JsonValue(std::string(node.successPolicy == fbzz::ai::BTParallelPolicy::RequireOne
                                     ? "requireOne" : "requireAll"));
    if (field == "keyName") return JsonValue(node.keyName);
    if (field == "compareOp")
        return JsonValue(std::string(fbzz::ai::BTCompareOpName(node.compareOp)));
    if (field == "withinSeconds") return JsonValue(node.withinSeconds);
    if (field == "valueBool") return JsonValue(node.valueBool);
    if (field == "valueInt") return JsonValue(node.valueInt);
    if (field == "valueFloat") return JsonValue(node.valueFloat);
    if (field == "valueString") return JsonValue(node.valueString);
    if (field == "moveTargetKey") return JsonValue(node.moveTargetKey);
    if (field == "acceptanceRadius") return JsonValue(node.acceptanceRadius);
    if (field == "chaseEntity") return JsonValue(node.chaseEntity);
    if (field == "repathInterval") return JsonValue(node.repathInterval);
    if (field == "range") return JsonValue(node.range);
    if (field == "turnSpeedDeg") return JsonValue(node.turnSpeedDeg);
    if (field == "animatorTrigger") return JsonValue(node.animatorTrigger);
    if (field == "waitForAnimation") return JsonValue(node.waitForAnimation);
    if (field == "soundPath") return JsonValue(node.soundPath);
    if (field == "volume") return JsonValue(node.volume);
    if (field == "scriptMethod") return JsonValue(node.scriptMethod);
    if (field == "threshold01") return JsonValue(node.threshold01);
    return JsonValue();
}

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

// lint の code ごとに「どう直すか」を機械可読で持つ表。vfx.lint の VFXFixHint と同じ役割。
// BT は「木としては正しいが意図どおり動かない」壊れ方が多く、直し方は code ごとに
// ほぼ一意に決まるので、推論ではなく参照にする。
// code の正本は Engine の CollectBehaviorTreeWarnings (Editor の警告 banner と同じ集合)。
// autoFixable: bt.repair が判断なしで直せるもの。false は設計判断が要るため AI に残す。
struct BTFixHint {
    const char* code;
    const char* severity;   // "error" 相当の実害があるものは "error"
    bool        autoFixable;
    const char* action;
    const char* caution;
};

const BTFixHint* FindBTFixHint(std::string_view code)
{
    static constexpr BTFixHint kHints[] = {
        { "no-lower-priority-abort", "error", true,
          "bt_repair(fixAborts=true) で、その条件の abortMode を lowerPriority にする。"
          "意図的に割り込ませたくない枝なら、その枝を Selector の最後へ回す (bt_node_set_order)。",
          "lowerPriority を付けると、下位の枝が Running 中でも条件が真に立った瞬間に中断される。"
          "中断されたくない不可分な行動 (再生中の攻撃モーション等) を含む枝には付けない。" },
        { "unreachable-sibling", "error", false,
          "後続の枝を活かすなら、塞いでいる子を bt_node_set_order で最後へ回すか、"
          "無限 Repeat なら repeatCount を有限にする / repeatUntilFailure=true にする。"
          "塞ぐのが意図なら、後続の枝は bt_node_remove で消す。",
          "どちらが意図かは機械的に決められない。木に残っているだけで実行されない枝は、"
          "読んだ人に「動いているはず」と誤解させ続ける。" },
        { "empty-composite", "error", false,
          "bt_node_add(parentId=<この id>) で子を足すか、まだ作らないなら "
          "AlwaysSucceed / AlwaysFail で栓をする。", "" },
        { "empty-decorator", "error", false,
          "bt_node_add(parentId=<この id>) で子を 1 つ足す。Decorator は子が無いと何も修飾しない。", "" },
        { "unresolved-key", "error", true,
          "bt_repair(fixKeys=true) で、綴りの近い既存キーへ張り替える。"
          "新しいキーが要るなら bt_blackboard_add で先に作る。",
          "自動置換は名前の近さだけで選ぶため、置換後に bt_tree の該当ノードでキーを確認すること。" },
        { "missing-key", "error", false,
          "bt_node_set_field(field=\"keyName\") で参照先を設定する。"
          "候補は bt_tree の blackboard にあるものだけ。", "" },
        { "zero-cooldown", "warning", true,
          "bt_repair(fixDurations=true) で duration を 1.0 秒にする。", "" },
        { "zero-duration-wait", "warning", true,
          "bt_repair(fixDurations=true) で duration を 1.0 秒にする。"
          "「1 tick だけ譲る」意図なら Wait ではなく AlwaysSucceed を使う。", "" },
        { "zero-weights", "warning", true,
          "bt_repair(fixWeights=true) で全ての重みを 1 (等確率) へ戻す。",
          "偏らせたい意図があった場合、その意図は復元できない。" },
        { "empty-script-method", "warning", false,
          "bt_node_set_field(field=\"scriptMethod\") でスクリプトのメソッド名を設定する。", "" },
        { "empty-animator-trigger", "warning", false,
          "bt_node_set_field(field=\"animatorTrigger\") でトリガー名を設定する。"
          "実在するトリガー名は animation_get_graph の parameters で確認する。", "" },
        { "empty-sound-path", "warning", false,
          "bt_node_set_field(field=\"soundPath\") で音声アセットのパスを設定する。"
          "実在パスは asset_list で調べる。", "" },
    };
    for (const auto& hint : kHints)
        if (code == hint.code) return &hint;
    return nullptr;
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
    int autoFixableCount = 0;
    for (const auto& warning : fbzz::ai::CollectBehaviorTreeWarnings(asset)) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(warning.nodeId));
        item.Set("code", JsonValue(warning.code));
        item.Set("message", JsonValue(warning.message));
        if (const BTFixHint* hint = FindBTFixHint(warning.code); hint != nullptr) {
            item.Set("severity", JsonValue(std::string(hint->severity)));
            item.Set("autoFixable", JsonValue(hint->autoFixable));
            item.Set("fix", JsonValue(std::string(hint->action)));
            if (hint->caution[0] != '\0') item.Set("caution", JsonValue(std::string(hint->caution)));
            if (hint->autoFixable) ++autoFixableCount;
        } else {
            // 手順を用意していない code は「自動修復できない」と明示する。
            // 黙って欠落させると、fix が無いことを「直さなくてよい」と読みかねない。
            item.Set("severity", JsonValue(std::string("warning")));
            item.Set("autoFixable", JsonValue(false));
        }
        issues.Push(std::move(item));
    }

    // コンパイル時にしか判らない不整合も併せて返す。Validate は構造しか見ないので、
    // 「保存もできて Validate も通るが実行時に効かない」層はここにしか現れない。
    JsonValue compileWarnings = JsonValue::MakeArray();
    fbzz::ai::BehaviorTreeRuntime compiled;
    std::string compileError;
    const bool compiles = fbzz::ai::CompileBehaviorTree(asset, compiled, &compileError);
    if (compiles)
        for (const std::string& warning : compiled.compileWarnings)
            compileWarnings.Push(JsonValue(warning));

    std::string validateError;
    const bool valid = fbzz::ai::ValidateBehaviorTreeAsset(asset, &validateError);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("issues", std::move(issues));
    result.Set("autoFixableCount", JsonValue(autoFixableCount));
    result.Set("compileWarnings", std::move(compileWarnings));
    result.Set("compiles", JsonValue(compiles));
    if (!compiles) result.Set("compileError", JsonValue(compileError));
    result.Set("valid", JsonValue(valid));
    if (!valid) result.Set("error", JsonValue(validateError));
    result.Set("note", JsonValue(std::string(
        "valid=false は保存が拒否される致命的な不整合 (ルートが 0/2 個・循環・子数超過)。"
        "issues は保存できるが意図どおり動かない構成で、autoFixable=true のものは "
        "bt.repair がまとめて直せる。compileWarnings は保存も Validate も通るが"
        "実行時に効かないもの (解決できなかった Blackboard キー等)。")));
    return Outcome::Ok(std::move(result));
}

// bt.guide — 木を組む前に読む規約。VFX の vfx.guide と同じ位置づけ。
Outcome DoBehaviorTreeGuide()
{
    // lintCode は、その規約を機械的に検査している bt.lint の issue code。
    // 空文字は「検査できないが守るべき設計原則」で、AI 側の判断に委ねる部分を明示する。
    struct Rule { const char* topic; const char* rule; const char* why; const char* lintCode; };
    static constexpr Rule kRules[] = {
        { "structure",
          "Selector の子は「やりたいことの優先順位」で並べる。order が小さいほど先に試される。"
          "戦闘 → 追跡 → 巡回 → 待機 のように、緊急度の高い枝を必ず左 (小さい order) へ置く。",
          "BT の挙動は木の形ではなく order で決まる。並べ替えを怠ると、"
          "「巡回が先に Success して戦闘へ入らない」という形で静かに壊れる。", "" },
        { "structure",
          "Sequence は AND、Selector は OR。「条件を確かめてから行動する」は "
          "Sequence(条件, 行動) で書く。",
          "Selector で書くと条件が Failure でも行動が実行され、条件の意味が消える。", "" },
        { "abort",
          "割り込みたい条件には abortMode=lowerPriority を付ける。"
          "付けられるのは純粋条件ノード (HasTarget / IsTargetInRange / BlackboardCondition 等) だけ。",
          "これが BT が FSM に対して優位を持つ最大の理由。無いと「巡回中にプレイヤーを"
          "発見しても、現在のウェイポイントに着くまで反応しない」鈍い AI になる。"
          "副作用のあるノードへ付けると、中断チェックのたびに世界が変わり木が非決定的になるため"
          "Validate が拒否する。", "no-lower-priority-abort" },
        { "structure",
          "後続の兄弟へ制御が渡らない子を途中に置かない。無限 Repeat と AlwaysRunning は "
          "Sequence を、AlwaysSucceed と Succeeder は Selector を、そこで打ち止めにする。",
          "木には見えているのに絶対に実行されない枝ができる。読んだ人には"
          "「動いているはず」に見え続けるので、木を読んでも気づけない。", "unreachable-sibling" },
        { "blackboard",
          "キーは bt.tree の blackboard に載っているものだけを使う。存在しない名前を書いても"
          "保存は通り、Compile 時に解決できず実行時は黙って無視される。",
          "「値を変えても行動が変わらない」としか見えず、綴り違いに最後まで気付けない。",
          "unresolved-key" },
        { "blackboard",
          "reserved=true のキーは PerceptionSystem 等が固定添字で書き込む。"
          "改名も削除もしてはならない。",
          "固定添字が前提なので、順序が変わると別のキーへ書かれる。", "" },
        { "timing",
          "Wait / Cooldown には durationRandom を入れる。",
          "同時にスポーンした敵の待機が完全に同期すると、群れが機械的に見える。", "" },
        { "structure",
          "未実装の枝は AlwaysSucceed / AlwaysFail で栓をしてから木を組む。",
          "空の Composite は「子が 0 個」として即座に結果が確定し、"
          "組み立て途中の木が意図しない結果を返す。", "empty-composite" },
        { "fields",
          "フィールドを書く前に bt.schema でその種別が読むものを確かめる。"
          "名前が実在しても種別が読まなければ効かない (Wait の range、HasTarget の duration)。",
          "保存も Validate も通るため、「設定したのに行動が変わらない」としか見えない。", "" },
        { "debug",
          "動かないときは木ではなく bt.runtime を見る。status=notEvaluated は到達していない、"
          "Blackboard の written=false は知覚側が書いていない、を意味する。",
          "「条件が偽」「割り込めていない」「到達していない」は木からも画面からも区別できず、"
          "推測で直すと別の箇所を壊す。", "" },
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
        // 検査できる規約は code を添える。bt.lint の同じ code がその規約の実装。
        if (item.lintCode[0] != '\0')
            entry.Set("lintCode", JsonValue(std::string(item.lintCode)));
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
    // 面が増えたので、どの順で呼ぶかを規約と一緒に返す。
    // WHY: BT は「木としては正しいが意図どおり動かない」壊れ方をするため、
    //      作って画面を見る、では収束しない。静的検査と実行状態の両方で挟む。
    result.Set("workflow", JsonValue(std::string(
        "1. bt.templateCatalog / bt.template.apply で動く骨格を取り込む "
        "(ゼロから積むより確実で abortMode やキーまで持ち込める)。"
        "2. bt.schema でその種別が読むフィールドを確かめてから bt.node.setField。"
        "3. bt.lint → autoFixable=true は bt.repair でまとめて直す。"
        "4. play_control(start) → bt.runtime で「到達しているか」「条件が真か」を確かめる。"
        "5. bt.diff で編集前後の木の意味の変化を確認する。")));
    return Outcome::Ok(std::move(result));
}

// bt.schema — ノード種別ごとに「何を書けるか」を返す。bt.node.setField の対。
// 実在しない名前は BT_UNKNOWN_FIELD で弾かれるが、実在するがその種別では読まれない
// 名前 (Wait へ range) は保存まで通ってしまう。受理集合そのものを公開する。
Outcome DoBehaviorTreeSchema(const JsonValue& payload)
{
    // nodeType 指定があればその種別だけに絞る。木を組む最中は 1 種別しか要らないのに、
    // 全 26 種別ぶんの目録を毎回返すと応答の大半が読まれないまま context を食う。
    const std::string filter = StringField(payload, "nodeType");

    JsonValue fields = JsonValue::MakeArray();
    for (const BTFieldSpec& spec : kBTFieldSpecs) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("field", JsonValue(std::string(spec.name)));
        entry.Set("type", JsonValue(std::string(spec.type)));
        entry.Set("description", JsonValue(std::string(spec.description)));
        if (spec.enumValues[0] != '\0') {
            JsonValue values = JsonValue::MakeArray();
            std::string current;
            for (const char* cursor = spec.enumValues; ; ++cursor) {
                if (*cursor == '|' || *cursor == '\0') {
                    values.Push(JsonValue(current));
                    current.clear();
                    if (*cursor == '\0') break;
                } else current.push_back(*cursor);
            }
            entry.Set("enumValues", std::move(values));
        }
        if (spec.minValue != spec.maxValue) {
            entry.Set("min", JsonValue(spec.minValue));
            entry.Set("max", JsonValue(spec.maxValue));
        }
        // そのフィールドを読む種別。ここに無い種別へ書くと BT_FIELD_NOT_APPLICABLE。
        JsonValue appliesTo = JsonValue::MakeArray();
        for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
            const auto type = static_cast<fbzz::ai::BTNodeType>(index);
            if (BTFieldAppliesTo(spec.name, type))
                appliesTo.Push(JsonValue(std::string(fbzz::ai::BTNodeTypeName(type))));
        }
        entry.Set("appliesTo", std::move(appliesTo));
        fields.Push(std::move(entry));
    }

    JsonValue nodeTypes = JsonValue::MakeArray();
    for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
        const auto type = static_cast<fbzz::ai::BTNodeType>(index);
        const std::string typeName = fbzz::ai::BTNodeTypeName(type);
        if (!filter.empty() && filter != typeName) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("nodeType", JsonValue(typeName));
        entry.Set("category", JsonValue(std::string(
            fbzz::ai::BTNodeIsComposite(type) ? "composite"
            : fbzz::ai::BTNodeIsDecorator(type) ? "decorator"
            : fbzz::ai::BTNodeIsPureCondition(type) ? "condition" : "action")));
        entry.Set("maxChildren", JsonValue(fbzz::ai::BTNodeMaxChildren(type)));
        entry.Set("canAbort", JsonValue(fbzz::ai::BTNodeIsPureCondition(type)
                                        || type == fbzz::ai::BTNodeType::BlackboardCondition));
        JsonValue own = JsonValue::MakeArray();
        for (const BTFieldSpec& spec : kBTFieldSpecs)
            if (BTFieldAppliesTo(spec.name, type)) own.Push(JsonValue(std::string(spec.name)));
        entry.Set("fields", std::move(own));
        nodeTypes.Push(std::move(entry));
    }
    if (!filter.empty() && nodeTypes.AsArray().empty())
        return Outcome::Err("BAD_ARG", "未知の BT nodeType です: " + filter);

    JsonValue result = JsonValue::MakeObject();
    result.Set("fields", std::move(fields));
    result.Set("nodeTypes", std::move(nodeTypes));
    result.Set("note", JsonValue(std::string(
        "appliesTo は「ランタイムが実際に読むか」で決めてある (Inspector の見た目ではない)。"
        "ここに載っていない組み合わせを bt.node.setField へ渡すと "
        "BT_FIELD_NOT_APPLICABLE で拒否される。")));
    return Outcome::Ok(std::move(result));
}

// bt.nodeField — ノードの現在値を読む。bt.node.setField の対になる読み出し。
// bt.tree は要約なので全フィールドを返さない。現在値を知らないまま書くと、
// 変更が効いたのかどうかも判断できない。
Outcome DoBehaviorTreeNodeField(editor::EditorContext& ctx, const JsonValue& payload)
{
    fbzz::ai::BehaviorTreeAsset asset;
    std::string relative;
    std::filesystem::path absolute;
    Outcome error;
    if (!LoadBehaviorTreeForAi(ctx, payload, asset, relative, absolute, error)) return error;

    const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
    const fbzz::ai::BTNodeDef* node = asset.FindNode(nodeId);
    if (node == nullptr) return Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません");

    const std::string field = StringField(payload, "field");
    JsonValue values = JsonValue::MakeArray();
    if (!field.empty()) {
        if (FindBTFieldSpec(field) == nullptr)
            return Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field
                                + " (bt.schema の fields を参照してください)");
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("field", JsonValue(field));
        entry.Set("value", BTFieldValueJson(*node, field));
        entry.Set("appliesToType", JsonValue(BTFieldAppliesTo(field, node->type)));
        values.Push(std::move(entry));
    } else {
        // field 省略時は「その種別が実際に読むフィールド」だけを返す。
        // 全 29 フィールドを返すと、大半が既定値のまま意味を持たない行になる。
        for (const BTFieldSpec& spec : kBTFieldSpecs) {
            if (!BTFieldAppliesTo(spec.name, node->type)) continue;
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("field", JsonValue(std::string(spec.name)));
            entry.Set("value", BTFieldValueJson(*node, spec.name));
            entry.Set("appliesToType", JsonValue(true));
            values.Push(std::move(entry));
        }
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("nodeId", JsonValue(nodeId));
    result.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node->type))));
    result.Set("values", std::move(values));
    result.Set("note", JsonValue(std::string(
        "value は bt.node.setField の value と同じ表現なので、読んで一部だけ変えて書き戻せる。"
        "appliesToType=false はその種別のランタイムが読まないフィールド (保存されても効かない)。")));
    return Outcome::Ok(std::move(result));
}

// bt.runtime — Play 中に「今どの枝が走っているか」と Blackboard の実値を返す。
// BT が動かない原因は「条件が偽」「割り込めていない」「到達していない」の 3 通りで、
// 木を読んでも lint を掛けても区別できない。実値と最終 status を突き合わせて初めて決まる。
Outcome DoBehaviorTreeRuntime(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.activeScene == nullptr)
        return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    // path 省略時は「今走っている BT を 1 体」。指定時はその木を使うエージェントに絞る。
    const std::string requested = NormalizeAssetPath(StringField(payload, "path"));
    const std::string requestedGuid = StringField(payload, "id");

    const scene::GameObject* owner = nullptr;
    const scene::BehaviorTreeComponent* component = nullptr;
    JsonValue agents = JsonValue::MakeArray();
    for (scene::GameObject* gameObject :
         ctx.activeScene->FindObjectsOfType<scene::BehaviorTreeComponent>()) {
        if (gameObject == nullptr) continue;
        const auto* candidate = gameObject->GetComponent<scene::BehaviorTreeComponent>();
        if (candidate == nullptr) continue;
        // 候補の一覧は常に返す。1 体しか返さないと「他にも居るのか」が判らず、
        // 別のエージェントを見たいときに Hierarchy を手で探す羽目になる。
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(gameObject->instanceId));
        item.Set("name", JsonValue(gameObject->name));
        item.Set("treePath", JsonValue(candidate->treePath));
        item.Set("running", JsonValue(candidate->runtime != nullptr));
        agents.Push(std::move(item));

        if (component != nullptr) continue;
        if (!requestedGuid.empty() && gameObject->instanceId != requestedGuid) continue;
        if (!requested.empty() && NormalizeAssetPath(candidate->treePath) != requested) continue;
        if (candidate->runtime == nullptr) continue;
        owner = gameObject;
        component = candidate;
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("agents", std::move(agents));
    if (component == nullptr) {
        // 「木が悪い」のか「そもそも走っていない」のかを取り違えさせない。
        result.Set("active", JsonValue(false));
        result.Set("reason", JsonValue(std::string(
            "条件に一致する、実行中の BehaviorTreeComponent がありません。"
            "play_control(start) で Play へ入るか、agents から id を選び直してください。")));
        return Outcome::Ok(std::move(result));
    }

    const fbzz::ai::BehaviorTreeRuntime& runtime = *component->runtime;
    JsonValue nodes = JsonValue::MakeArray();
    // runtime は DFS pre-order の配列。index が小さいほど高優先度なので、
    // この順のまま返せば「上から順に読めば優先順位」という読み方がそのまま通る。
    for (std::size_t index = 0; index < runtime.nodes.size(); ++index) {
        const std::uint8_t status = index < component->lastNodeStatus.size()
            ? component->lastNodeStatus[index] : 0;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(index < runtime.authoringIdOf.size()
                                     ? runtime.authoringIdOf[index] : 0));
        item.Set("nodeType", JsonValue(std::string(
            fbzz::ai::BTNodeTypeName(runtime.nodes[index].type))));
        static const char* kStatusNames[] = { "notEvaluated", "success", "failure", "running" };
        item.Set("status", JsonValue(std::string(kStatusNames[status < 4 ? status : 0])));
        nodes.Push(std::move(item));
    }

    // Blackboard の実値。「条件が偽のまま」なのかを判断する唯一の材料。
    JsonValue blackboard = JsonValue::MakeArray();
    for (std::size_t index = 0; index < runtime.blackboard.size(); ++index) {
        const auto key = static_cast<fbzz::ai::BlackboardKey>(index);
        const fbzz::ai::BlackboardDef& def = runtime.blackboard[index];
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(def.name));
        item.Set("type", JsonValue(std::string(fbzz::ai::BlackboardTypeName(def.type))));
        item.Set("reserved", JsonValue(def.reserved));
        // 一度も書かれていないキーは既定値のまま。既定値と「書かれた結果たまたま
        // 既定値と同じ」を区別しないと、知覚システムが動いているのかが判らない。
        item.Set("written", JsonValue(component->blackboard.IsSet(key)));
        item.Set("lastWriteTime", JsonValue(component->blackboard.GetLastWriteTime(key)));
        switch (def.type) {
        case fbzz::ai::BlackboardType::Bool: {
            bool value = false;
            if (component->blackboard.GetBool(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Int: {
            int value = 0;
            if (component->blackboard.GetInt(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Float: {
            float value = 0.0f;
            if (component->blackboard.GetFloat(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        case fbzz::ai::BlackboardType::Vector3: {
            math::Vector3 value = math::Vector3::ZERO;
            if (component->blackboard.GetVector3(key, value)) {
                JsonValue vector = JsonValue::MakeArray();
                vector.Push(JsonValue(value.x));
                vector.Push(JsonValue(value.y));
                vector.Push(JsonValue(value.z));
                item.Set("value", std::move(vector));
            }
            break;
        }
        case fbzz::ai::BlackboardType::Entity: {
            scene::EntityID value = scene::EntityID::INVALID;
            if (component->blackboard.GetEntity(key, value))
                item.Set("value", JsonValue(static_cast<int>(value.index)));
            break;
        }
        default: {
            std::string value;
            if (component->blackboard.GetString(key, value)) item.Set("value", JsonValue(value));
            break;
        }
        }
        blackboard.Push(std::move(item));
    }

    JsonValue compileWarnings = JsonValue::MakeArray();
    for (const std::string& warning : runtime.compileWarnings)
        compileWarnings.Push(JsonValue(warning));

    result.Set("active", JsonValue(true));
    result.Set("id", JsonValue(owner->instanceId));
    result.Set("name", JsonValue(owner->name));
    result.Set("treePath", JsonValue(component->loadedTreePath.empty()
                                     ? component->treePath : component->loadedTreePath));
    result.Set("rootStatus", JsonValue(std::string(
        fbzz::ai::BTStatusName(component->lastRootStatus))));
    result.Set("tickCount", JsonValue(static_cast<int>(component->tickCount)));
    result.Set("elapsedTime", JsonValue(component->elapsedTime));
    result.Set("nodes", std::move(nodes));
    result.Set("blackboard", std::move(blackboard));
    result.Set("compileWarnings", std::move(compileWarnings));
    result.Set("note", JsonValue(std::string(
        "nodes は DFS pre-order (index が小さいほど高優先度)。status=notEvaluated は"
        "「今回の tick で到達しなかった」= 上位の枝で決着した、を意味する。"
        "written=false のキーは一度も書かれていないので、条件が偽なのは"
        "木ではなく知覚側 (PerceptionSystem / スクリプト) の問題。")));
    return Outcome::Ok(std::move(result));
}

// bt.diff — 2 つの .behaviortree の構造差分を返す。
// bt.tree の目視比較はノードが 20 を超えると追えず、別案を作って比べる使い方も成立しない。
Outcome DoBehaviorTreeDiff(editor::EditorContext& ctx, const JsonValue& payload)
{
    const auto load = [&ctx](const std::string& key, const JsonValue& source,
                             fbzz::ai::BehaviorTreeAsset& out, Outcome& error) {
        JsonValue wrapper = JsonValue::MakeObject();
        wrapper.Set("path", JsonValue(StringField(source, key.c_str())));
        std::string relative;
        std::filesystem::path absolute;
        return LoadBehaviorTreeForAi(ctx, wrapper, out, relative, absolute, error);
    };
    fbzz::ai::BehaviorTreeAsset base;
    fbzz::ai::BehaviorTreeAsset target;
    Outcome error;
    if (StringField(payload, "base").empty() || StringField(payload, "target").empty())
        return Outcome::Err("BAD_ARG", "base と target が必要です");
    if (!load("base", payload, base, error)) return error;
    if (!load("target", payload, target, error)) return error;

    JsonValue added = JsonValue::MakeArray();
    JsonValue removed = JsonValue::MakeArray();
    JsonValue changed = JsonValue::MakeArray();

    for (const auto& node : target.nodes) {
        const fbzz::ai::BTNodeDef* previous = base.FindNode(node.id);
        if (previous == nullptr) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("nodeId", JsonValue(node.id));
            item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
            item.Set("name", JsonValue(node.name));
            item.Set("parentId", JsonValue(node.parentId));
            added.Push(std::move(item));
            continue;
        }
        JsonValue fields = JsonValue::MakeArray();
        const auto pushChange = [&fields](const std::string& name, JsonValue before, JsonValue after) {
            JsonValue field = JsonValue::MakeObject();
            field.Set("field", JsonValue(name));
            field.Set("before", std::move(before));
            field.Set("after", std::move(after));
            fields.Push(std::move(field));
        };
        // 種別変更は「別のノードになった」に等しいので、フィールド差分より先に出す。
        if (previous->type != node.type)
            pushChange("nodeType",
                       JsonValue(std::string(fbzz::ai::BTNodeTypeName(previous->type))),
                       JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        // 木の形 (親・優先度) は BT の挙動そのものなので必ず差分に出す。
        if (previous->parentId != node.parentId)
            pushChange("parentId", JsonValue(previous->parentId), JsonValue(node.parentId));
        if (previous->order != node.order)
            pushChange("order", JsonValue(previous->order), JsonValue(node.order));
        // 値の差分は「変更後の種別が読むフィールド」だけ見る。読まれないフィールドの
        // 差分を並べても、挙動は 1 ミリも変わらないため差分の意味が薄まる。
        for (const BTFieldSpec& spec : kBTFieldSpecs) {
            if (spec.name == std::string_view("editorX")
                || spec.name == std::string_view("editorY")) continue;  // 座標は挙動に無関係
            if (!BTFieldAppliesTo(spec.name, node.type)) continue;
            const JsonValue before = BTFieldValueJson(*previous, spec.name);
            const JsonValue after = BTFieldValueJson(node, spec.name);
            if (SerializeJson(before) == SerializeJson(after)) continue;
            pushChange(spec.name, before, after);
        }
        if (fields.AsArray().empty()) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        item.Set("name", JsonValue(node.name));
        item.Set("fields", std::move(fields));
        changed.Push(std::move(item));
    }
    for (const auto& node : base.nodes) {
        if (target.FindNode(node.id) != nullptr) continue;
        JsonValue item = JsonValue::MakeObject();
        item.Set("nodeId", JsonValue(node.id));
        item.Set("nodeType", JsonValue(std::string(fbzz::ai::BTNodeTypeName(node.type))));
        item.Set("name", JsonValue(node.name));
        removed.Push(std::move(item));
    }

    // Blackboard の増減も出す。キーが消えると、それを参照するノードが実行時に無言で死ぬ。
    JsonValue keysAdded = JsonValue::MakeArray();
    JsonValue keysRemoved = JsonValue::MakeArray();
    const auto hasKey = [](const fbzz::ai::BehaviorTreeAsset& asset, const std::string& name) {
        return std::any_of(asset.blackboard.begin(), asset.blackboard.end(),
                           [&name](const fbzz::ai::BlackboardDef& def) { return def.name == name; });
    };
    for (const auto& def : target.blackboard)
        if (!hasKey(base, def.name)) keysAdded.Push(JsonValue(def.name));
    for (const auto& def : base.blackboard)
        if (!hasKey(target, def.name)) keysRemoved.Push(JsonValue(def.name));

    JsonValue result = JsonValue::MakeObject();
    result.Set("base", JsonValue(StringField(payload, "base")));
    result.Set("target", JsonValue(StringField(payload, "target")));
    result.Set("addedNodes", std::move(added));
    result.Set("removedNodes", std::move(removed));
    result.Set("changedNodes", std::move(changed));
    result.Set("addedKeys", std::move(keysAdded));
    result.Set("removedKeys", std::move(keysRemoved));
    result.Set("note", JsonValue(std::string(
        "editorX / editorY は挙動に無関係なので差分に含めない。"
        "parentId と order の変化は木の意味そのものが変わったことを示す。")));
    return Outcome::Ok(std::move(result));
}

// ── Behavior Tree のテンプレート ────────────────────────────────────────────
// 探索順は「Project → 開発 Engine → 実行ファイル同梱」。Editor と揃えないと
// 「AI では使えるのに Editor では出てこない」テンプレートが生まれる。
std::vector<std::filesystem::path> BehaviorTreeTemplateRoots(const editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    constexpr const char* kRelative = "Assets/AI/Templates";
    std::vector<fs::path> roots;
    if (!ctx.projectRoot.empty()) roots.push_back(fs::path(ctx.projectRoot) / kRelative);
    if (!ctx.engineRoot.empty()) roots.push_back(fs::path(ctx.engineRoot) / kRelative);
    const fs::path executableDirectory = util::FileSystem::GetExecutableDirectory();
    roots.push_back(executableDirectory / "assets/AI/Templates");
    roots.push_back(executableDirectory / kRelative);
    return roots;
}

// テンプレート名 (拡張子なし) かパスから実ファイルを解決する。
bool ResolveBehaviorTreeTemplate(const editor::EditorContext& ctx, const std::string& requested,
                                 std::filesystem::path& outPath)
{
    namespace fs = std::filesystem;
    if (requested.empty()) return false;
    std::error_code errorCode;
    // パス指定ならそのまま (projectRoot 配下に限る)。
    if (requested.find('/') != std::string::npos || requested.find('\\') != std::string::npos) {
        std::string relative;
        if (ResolveProjectFile(ctx, requested, outPath, relative)
            && fs::is_regular_file(outPath, errorCode)) return true;
    }
    for (const fs::path& root : BehaviorTreeTemplateRoots(ctx)) {
        if (!fs::is_directory(root, errorCode)) { errorCode.clear(); continue; }
        for (fs::recursive_directory_iterator iterator(root, errorCode), end;
             iterator != end; iterator.increment(errorCode)) {
            if (errorCode) { errorCode.clear(); break; }
            const fs::directory_entry& file = *iterator;
            if (!file.is_regular_file(errorCode)) continue;
            if (file.path().extension() != ".behaviortree") continue;
            if (file.path().stem().generic_string() != requested) continue;
            outPath = file.path();
            return true;
        }
    }
    return false;
}

// bt.templateCatalog — 取り込める骨格の目録。
// WHY: bt.guide の recipes は「こう組め」という文章で、そのまま実体にはならない。
//      動く木が既にあるなら、ゼロから積むより取り込んで直すほうが確実に速い。
Outcome DoBehaviorTreeTemplateCatalog(editor::EditorContext& ctx)
{
    namespace fs = std::filesystem;
    JsonValue templates = JsonValue::MakeArray();
    std::vector<std::string> seen;
    std::error_code errorCode;
    for (const fs::path& root : BehaviorTreeTemplateRoots(ctx)) {
        if (!fs::is_directory(root, errorCode)) { errorCode.clear(); continue; }
        for (fs::recursive_directory_iterator iterator(root, errorCode), end;
             iterator != end; iterator.increment(errorCode)) {
            if (errorCode) { errorCode.clear(); break; }
            const fs::directory_entry& file = *iterator;
            if (!file.is_regular_file(errorCode)) continue;
            if (file.path().extension() != ".behaviortree") continue;
            const std::string name = file.path().stem().generic_string();
            // 先に見つかった root (優先度が高い) の同名を勝たせる。
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            seen.push_back(name);

            fbzz::ai::BehaviorTreeAsset asset;
            if (!fbzz::ai::ParseBehaviorTreeAsset(file.path().generic_string(), asset)) continue;
            JsonValue item = JsonValue::MakeObject();
            item.Set("name", JsonValue(name));
            item.Set("path", JsonValue(file.path().generic_string()));
            item.Set("treeName", JsonValue(asset.name));
            item.Set("description", JsonValue(asset.description));
            item.Set("nodeCount", JsonValue(static_cast<int>(asset.nodes.size())));
            templates.Push(std::move(item));
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("templates", std::move(templates));
    result.Set("usage", JsonValue(std::string(
        "bt.template.apply(template=<name>, path=<書き出し先.behaviortree>) で取り込む。"
        "取り込み後は bt.lint → bt.node.setField で用途に合わせて調整する。")));
    return Outcome::Ok(std::move(result));
}

// シェーダーの変数目録を JSON にする。descriptor が無効なら空配列を返す。
// ShaderDescriptor は PS バイトコードのリフレクション結果で、「何を書けるか」の唯一の正本。
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
// 実際に効くのはコンパイル済みバイトコードのリフレクション結果で、ソース上の宣言ではない
// (未使用変数は最適化で消える)。存在しない名前を書いても保存は通り実行時に無視される。
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

// Editor の表示と同じ正本から、シェーダーコンパイル診断を AI へ返す。
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
        ".scene", ".prefab", ".mat", ".tex", ".terrain", ".animcontroller", ".vfx", ".fluid",
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

// ---------------------------------------------------------------------------
// Sprite (Texture のサブアセット)
//
// WHY 専用の op を持つか:
//   Sprite は独立したファイルではないので asset.list に出ない。asset.thumbnail も
//   ファイル丸ごとしか返せず、シートの «何番目がどの絵か» は AI から一切見えない。
//   一覧 (sprite.list) と切り抜き画像 (sprite.thumbnail) が無いと、AI は
//   UUID を書き写す以外に割り当てる方法が無い。規約は Docs/design/sprite-reference.md。
// ---------------------------------------------------------------------------

// sprites を 1 つも持たない Single Texture は «全面 1 枚» を画像名で参照できる
// (ResolveSpriteReference の暗黙 Single)。この 1 枚だけは ID を持たないので、
// ID が引けないことを «壊れている» と判定してはいけない。
bool IsImplicitSingleSprite(const std::string& texturePath, const std::string& token)
{
    asset::TextureImportSettings settings;
    if (!asset::GetCachedTextureImportSettings(texturePath, settings)) return false;
    if (settings.type != asset::TextureType::Sprite
        || settings.spriteMode != asset::SpriteMode::Single
        || !settings.sprites.empty()) return false;
    return token == util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(texturePath).stem());
}

// 参照文字列は「そのまま貼れる完成形」を返す。AI に組み立てさせると、
// 区切り (::sprite::) の写し間違いが静かな «アトラス全面» になって返ってくる。
std::string MakeSpriteReferenceFor(const std::string& relativeTexturePath,
                                   const asset::SpriteRect& sprite,
                                   bool nameIsUnique)
{
    const bool useName = nameIsUnique && !sprite.name.empty();
    return asset::MakeSpriteReference(relativeTexturePath,
                                      useName ? sprite.name : sprite.id);
}

bool LoadSpriteSettings(editor::EditorContext& ctx, const JsonValue& payload,
                        std::filesystem::path& outFile, std::string& outRelative,
                        asset::TextureImportSettings& outSettings, Outcome& err)
{
    std::error_code ec;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), outFile, outRelative)
        || !std::filesystem::is_regular_file(outFile, ec)) {
        err = Outcome::Err("ASSET_NOT_FOUND", "projectRoot 配下のテクスチャを指定してください");
        return false;
    }
    if (!asset::GetCachedTextureImportSettings(outFile.generic_string(), outSettings)) {
        err = Outcome::Err("NO_SPRITE_META",
            "この画像に .meta がありません。Inspector で Texture Type を Sprite にしてください");
        return false;
    }
    if (outSettings.type != asset::TextureType::Sprite) {
        err = Outcome::Err("NOT_A_SPRITE",
            "Texture Type が Sprite ではありません: " + outRelative);
        return false;
    }
    return true;
}

// 名前がテクスチャ内で一意かを引けるようにする。重複した名前で参照を作ると
// どちらを指すか決まらないので、その Sprite だけ ID で返す。
std::unordered_map<std::string, int> CountSpriteNames(
    const asset::TextureImportSettings& settings)
{
    std::unordered_map<std::string, int> counts;
    for (const asset::SpriteRect& sprite : settings.sprites) ++counts[sprite.name];
    return counts;
}

Outcome DoSpriteList(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    asset::TextureImportSettings settings;
    Outcome err = Outcome::Ok(JsonValue::MakeObject());
    if (!LoadSpriteSettings(ctx, payload, file, relative, settings, err)) return err;

    const auto nameCounts = CountSpriteNames(settings);
    JsonValue sprites = JsonValue::MakeArray();
    for (const asset::SpriteRect& sprite : settings.sprites) {
        const auto found = nameCounts.find(sprite.name);
        const bool unique = found != nameCounts.end() && found->second == 1;
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(sprite.id));
        item.Set("name", JsonValue(sprite.name));
        item.Set("x", JsonValue(static_cast<int>(sprite.x)));
        item.Set("y", JsonValue(static_cast<int>(sprite.y)));
        item.Set("width", JsonValue(static_cast<int>(sprite.width)));
        item.Set("height", JsonValue(static_cast<int>(sprite.height)));
        item.Set("pivotX", JsonValue(static_cast<double>(sprite.pivotX)));
        item.Set("pivotY", JsonValue(static_cast<double>(sprite.pivotY)));
        item.Set("reference", JsonValue(MakeSpriteReferenceFor(relative, sprite, unique)));
        if (!unique) item.Set("nameIsAmbiguous", JsonValue(true));
        sprites.Push(std::move(item));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("spriteMode", JsonValue(settings.spriteMode == asset::SpriteMode::Multiple
                                       ? "Multiple" : "Single"));
    result.Set("pixelsPerUnit", JsonValue(static_cast<double>(settings.pixelsPerUnit)));
    result.Set("count", JsonValue(static_cast<int>(settings.sprites.size())));
    result.Set("sprites", std::move(sprites));
    result.Set("hint", JsonValue(std::string(
        "reference をそのまま UIImage.texturePath / SpriteRenderer.spritePath / "
        ".mat の albedo (メッシュ描画の .mat のみ — Particle / UI / Decal は切り抜きが効きません) / "
        ".fluid の texture 発生源へ入れてください (component_set)。"
        "どのコマがどの絵かは sprite_thumbnail で確認できます。"
        "名前は sprite_rename で付け直せます (ID は変わらないので既存の参照は切れません)。")));
    return Outcome::Ok(std::move(result));
}

// sprite.thumbnail — 1 コマだけを切り抜いた PNG を返す。
// WHY 切り抜いて返すか: シート全体を返しても «何番目» は見えない。AI が絵を見て
//     選べる形にすることが、名前付けと割り当てを任せられる最低条件になる。
Outcome DoSpriteThumbnail(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    asset::TextureImportSettings settings;
    Outcome err = Outcome::Ok(JsonValue::MakeObject());
    if (!LoadSpriteSettings(ctx, payload, file, relative, settings, err)) return err;

    const std::string token = StringField(payload, "sprite");
    if (token.empty()) return Outcome::Err("BAD_ARG", "sprite (ID または名前) が必要です");
    const asset::SpriteRect* sprite = asset::FindSprite(settings, token);
    if (sprite == nullptr)
        return Outcome::Err("SPRITE_NOT_FOUND",
            "この ID / 名前の Sprite がありません: " + token + " (sprite_list で確認してください)");

    int sourceWidth = 0;
    int sourceHeight = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(util::FileSystem::PathToUtf8(file).c_str(),
                                &sourceWidth, &sourceHeight, &channels, 4);
    if (pixels == nullptr || sourceWidth <= 0 || sourceHeight <= 0) {
        if (pixels) stbi_image_free(pixels);
        return Outcome::Err("DECODE_FAILED", "画像をデコードできません: " + relative);
    }

    // 幅 / 高さ 0 は「画像全体」を意味する (Single Sprite の表現)。
    const int rectX = std::clamp(static_cast<int>(sprite->x), 0, sourceWidth - 1);
    const int rectY = std::clamp(static_cast<int>(sprite->y), 0, sourceHeight - 1);
    const int rectW = sprite->width > 0
        ? std::min(static_cast<int>(sprite->width), sourceWidth - rectX) : sourceWidth - rectX;
    const int rectH = sprite->height > 0
        ? std::min(static_cast<int>(sprite->height), sourceHeight - rectY) : sourceHeight - rectY;
    if (rectW <= 0 || rectH <= 0) {
        stbi_image_free(pixels);
        return Outcome::Err("EMPTY_RECT", "Sprite の矩形が空です");
    }

    // 転送量を抑えるため長辺 256 までへ間引く。コマの識別に等倍は要らない。
    constexpr int kMaxSide = 256;
    const int step = std::max(1, (std::max(rectW, rectH) + kMaxSide - 1) / kMaxSide);
    const int outWidth = std::max(1, rectW / step);
    const int outHeight = std::max(1, rectH / step);
    std::vector<uint8_t> cropped(static_cast<size_t>(outWidth) * outHeight * 4);
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            const size_t src = (static_cast<size_t>(rectY + y * step) * sourceWidth
                                + static_cast<size_t>(rectX + x * step)) * 4;
            const size_t dst = (static_cast<size_t>(y) * outWidth + x) * 4;
            std::memcpy(cropped.data() + dst, pixels + src, 4);
        }
    }
    stbi_image_free(pixels);

    size_t pngSize = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(
        cropped.data(), outWidth, outHeight, 4, &pngSize, 6, MZ_FALSE);
    if (png == nullptr || pngSize == 0) {
        if (png) mz_free(png);
        return Outcome::Err("ENCODE_FAILED", "PNG へ変換できません");
    }
    const std::vector<uint8_t> bytes(static_cast<uint8_t*>(png),
                                     static_cast<uint8_t*>(png) + pngSize);
    mz_free(png);

    const auto nameCounts = CountSpriteNames(settings);
    const auto found = nameCounts.find(sprite->name);
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue("image/png"));
    result.Set("base64", JsonValue(Base64Encode(bytes)));
    result.Set("width", JsonValue(outWidth));
    result.Set("height", JsonValue(outHeight));
    result.Set("sourceWidth", JsonValue(rectW));
    result.Set("sourceHeight", JsonValue(rectH));
    result.Set("name", JsonValue(sprite->name));
    result.Set("reference", JsonValue(MakeSpriteReferenceFor(
        relative, *sprite, found != nameCounts.end() && found->second == 1)));
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
    const std::string stateName = StringField(payload, "state");
    const JsonValue* timeValue = payload.Find("time");
    const JsonValue* frameValue = payload.Find("frame");
    if (action != "play" && action != "pause" && action != "stop" && action != "seek") {
        return Outcome::Err("BAD_ARG", "未知の animation action: " + action);
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
    if (dryRun) return DryRunPreview("animation.control:" + action);

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
        animator->stateTime = 0.0f;
    } else if (action == "seek") {
        float seconds = timeValue != nullptr && timeValue->IsNumber() ? static_cast<float>(timeValue->AsNumber()) : -1.0f;
        if (frameValue != nullptr && frameValue->IsNumber()) {
            const asset::AnimationClip* clip = nullptr;
            const std::string activeStateName = stateName.empty() ? animator->currentStateName : stateName;
            const auto stateIterator = std::find_if(animator->states.begin(), animator->states.end(), [&](const scene::AnimationState& state) {
                return state.name == activeStateName;
            });
            if (stateIterator != animator->states.end()) {
                const scene::AnimationState& state = *stateIterator;
                if (!state.sourcePath.empty()) {
                    for (size_t index = 0; index < animator->clips.size(); ++index) {
                        if (index < animator->clipSourcePaths.size()
                            && animator->clipSourcePaths[index] == state.sourcePath
                            && (state.clipName.empty() || animator->clips[index].name == state.clipName)) {
                            clip = &animator->clips[index];
                            break;
                        }
                    }
                }
                if (clip == nullptr && !state.clipName.empty()) {
                    for (const auto& candidate : animator->clips) {
                        if (candidate.name == state.clipName) { clip = &candidate; break; }
                    }
                }
                if (clip == nullptr && state.clipIndex >= 0
                    && state.clipIndex < static_cast<int>(animator->clips.size())) {
                    clip = &animator->clips[static_cast<size_t>(state.clipIndex)];
                }
            }
            if (clip == nullptr || clip->frameRate <= 0.0f) return Outcome::Err("CLIP_NOT_READY", "frame seek にはロード済み clip が必要です");
            seconds = static_cast<float>(frameValue->AsNumber()) / clip->frameRate;
        }
        if (seconds < 0.0f) return Outcome::Err("BAD_ARG", "seek には time または frame が必要です");
        animator->stateTime = seconds;
    }
    return DoAnimationState(ctx, payload);
}

// ── Animator ステートマシン照会・パラメーター駆動 ─────────────────────────────
// animation.state は再生状態、animation.pose は骨行列だけで、「どのパラメーターが
// どの遷移を発火させるか」という構造が読めなかった。

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
// 書けるが読めない状態だと、作った構成を確認できず重複作成を繰り返すことになる。
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
    entry.Set("currentState", JsonValue(
        layer.states.empty() ? std::string{} : layer.runtime.currentStateName));
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
        result.Set("dampTime", JsonValue(tree.dampTime));
        result.Set("syncNormalizedTime", JsonValue(tree.syncNormalizedTime));
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
    result.Set("skinningVertices", JsonValue(
        static_cast<std::int64_t>(rendering.renderStats.skinningVertexCount)));
    // uint32_t は JsonValue の int / int64_t / double と暗黙変換が競合するため、
    // JSON の整数表現へ明示的に昇格させる。
    result.Set("skinningDispatches", JsonValue(
        static_cast<std::int64_t>(rendering.renderStats.skinningDispatchCount)));
    result.Set("totalObjects", JsonValue(rendering.renderStats.totalObjects));
    result.Set("frustumCulled", JsonValue(rendering.renderStats.frustumCulled));
    result.Set("occlusionCulled", JsonValue(rendering.renderStats.occlusionCulled));
    // シャドウマップ描画はカメラ視点の統計と別枠。合計だけ見て「描画が軽い」と誤判断しないよう分けて返す。
    result.Set("shadowDrawCalls", JsonValue(rendering.renderStats.shadowDrawCalls));
    result.Set("shadowTriangles", JsonValue(rendering.renderStats.shadowTriangleCount));
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
// どの編集も「states + anyStateTransitions + parameters を丸ごとスナップショットして復元」で
// 戻せる (clip 実体を含まない軽量ベクトル)。
// 復元後はランタイム遷移状態を消し、消えたステートを指したまま再生が続くのを防ぐ。
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
// ラムダは実行時に GameObject を引き直すので、対象レイヤーもその場で解決する。
// 消えていたら Base Layer へ落とす (落ちるより Undo スタックを壊さない方が安全)。
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

// sprite.rename / sprite.slice — .meta の [texture].sprites を書き換える。
//
// WHY ID を触らないか: ID はこの Sprite への参照そのもの。名前を変えたいだけで
//     ID まで振り直すと、.scene に保存済みの参照が «アトラス全面» に化ける。
//     slice も重なりで ID を引き継ぐ (Sprite Editor の Smart と同じ規則)。
std::unique_ptr<ICommand> BuildSpriteCommand(editor::EditorContext& ctx,
                                             const std::string& type,
                                             const JsonValue& payload,
                                             Outcome& err,
                                             JsonValue* detailSink)
{
    namespace fs = std::filesystem;
    fs::path absolute;
    std::string relative;
    asset::TextureImportSettings settings;
    if (!LoadSpriteSettings(ctx, payload, absolute, relative, settings, err)) return nullptr;

    const std::string metaPath = absolute.generic_string() + ".meta";
    const asset::TextureImportSettings before = settings;

    if (type == "sprite.rename") {
        const std::string token = StringField(payload, "sprite");
        const std::string newName = StringField(payload, "name");
        if (token.empty() || newName.empty()) {
            err = Outcome::Err("BAD_ARG", "sprite (ID または名前) と name が必要です");
            return nullptr;
        }
        const asset::SpriteRect* target = asset::FindSprite(settings, token);
        if (target == nullptr) {
            err = Outcome::Err("SPRITE_NOT_FOUND", "この ID / 名前の Sprite がありません: " + token);
            return nullptr;
        }
        // 添字で指す。ID が空の (移行前の) .meta でも 1 件だけを確実に書き換えるため。
        const std::size_t targetIndex =
            static_cast<std::size_t>(target - settings.sprites.data());
        const std::string targetId = target->id;
        for (std::size_t i = 0; i < settings.sprites.size(); ++i) {
            if (i != targetIndex && settings.sprites[i].name == newName) {
                err = Outcome::Err("DUPLICATE_NAME",
                    "同じ名前の Sprite が既にあります: " + newName
                    + " (名前は参照キーを兼ねるのでテクスチャ内で一意である必要があります)");
                return nullptr;
            }
        }
        settings.sprites[targetIndex].name = newName;

        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("id", JsonValue(targetId));
            detailSink->Set("name", JsonValue(newName));
            detailSink->Set("reference",
                JsonValue(asset::MakeSpriteReference(relative, newName)));
        }
    } else if (type == "sprite.slice") {
        // 生成も畳み込みも Sprite Editor と同じ実装を通す (Editor/Util/SpriteSlicer.hpp)。
        const std::string imagePath = util::FileSystem::PathToUtf8(absolute);
        const auto intField = [&payload](const char* key, int fallback) {
            const JsonValue* value = payload.Find(key);
            return value != nullptr ? value->AsInt(fallback) : fallback;
        };
        const auto floatField = [&payload](const char* key, float fallback) {
            const JsonValue* value = payload.Find(key);
            return value != nullptr
                ? static_cast<float>(value->AsNumber(static_cast<double>(fallback)))
                : fallback;
        };
        const std::string modeName = StringField(payload, "mode");
        const spriteslice::ExistingMode mode =
            modeName == "replace" ? spriteslice::ExistingMode::DeleteExisting
          : modeName == "safe"    ? spriteslice::ExistingMode::Safe
                                  : spriteslice::ExistingMode::Smart;
        const std::string prefix = StringField(payload, "prefix").empty()
            ? util::FileSystem::PathToUtf8(absolute.stem()) + "_"
            : StringField(payload, "prefix");

        spriteslice::Result generated;
        if (StringField(payload, "type") == "automatic") {
            spriteslice::AutoTrimParams params;
            params.pivotX   = floatField("pivotX", 0.5f);
            params.pivotY   = floatField("pivotY", 0.5f);
            params.baseName = prefix;
            generated = spriteslice::GenerateAutoTrim(imagePath, params);
        } else {
            int imageWidth = 0;
            int imageHeight = 0;
            if (!stbi_info(imagePath.c_str(), &imageWidth, &imageHeight, nullptr)
                || imageWidth <= 0 || imageHeight <= 0) {
                err = Outcome::Err("DECODE_FAILED", "画像の寸法を取得できません: " + relative);
                return nullptr;
            }
            spriteslice::GridParams params;
            params.columns        = intField("columns", 0);
            params.rows           = intField("rows", 0);
            params.cellWidth      = intField("cellWidth", 0);
            params.cellHeight     = intField("cellHeight", 0);
            params.byCellCount    = params.columns > 0 || params.rows > 0;
            if (!params.byCellCount && (params.cellWidth <= 0 || params.cellHeight <= 0)) {
                err = Outcome::Err("BAD_ARG",
                    "columns/rows か cellWidth/cellHeight のどちらかを指定してください "
                    "(type=automatic なら不要です)");
                return nullptr;
            }
            if (params.byCellCount) {
                params.columns = std::max(1, params.columns);
                params.rows    = std::max(1, params.rows);
            }
            params.offsetX        = intField("offsetX", 0);
            params.offsetY        = intField("offsetY", 0);
            params.paddingX       = intField("paddingX", 0);
            params.paddingY       = intField("paddingY", 0);
            params.pivotX         = floatField("pivotX", 0.5f);
            params.pivotY         = floatField("pivotY", 0.5f);
            params.keepEmptyRects = payload.Find("keepEmptyRects") != nullptr
                ? payload.Find("keepEmptyRects")->AsBool() : true;
            params.baseName       = prefix;
            generated = spriteslice::GenerateGrid(
                imagePath, static_cast<uint32_t>(imageWidth),
                static_cast<uint32_t>(imageHeight), params);
        }
        if (!generated.error.empty()) {
            err = Outcome::Err("EMPTY_SLICE", generated.error);
            return nullptr;
        }

        int reused = 0;
        settings.sprites = spriteslice::MergeIntoExisting(
            before.sprites, std::move(generated.sprites), mode, &reused);
        settings.spriteMode = asset::SpriteMode::Multiple;

        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("count", JsonValue(static_cast<int>(settings.sprites.size())));
            detailSink->Set("reusedIds", JsonValue(reused));
            // replace 以外は既存の矩形を消さないので、参照が切れるのは replace のときだけ。
            detailSink->Set("brokenReferences",
                JsonValue(mode == spriteslice::ExistingMode::DeleteExisting
                          ? static_cast<int>(before.sprites.size()) : 0));
        }
    } else {
        err = Outcome::Err("UNKNOWN_TYPE", "未対応の sprite 操作です: " + type);
        return nullptr;
    }

    const auto write = [metaPath, absolute](const asset::TextureImportSettings& value) {
        asset::TextureAsset asset;
        asset.sourcePath = absolute.generic_string();
        asset.settings = value;
        const asset::TexDescSerializer serializer;
        (void)serializer.Save(asset, metaPath);
    };
    editor::EditorContext* context = &ctx;
    const asset::TextureImportSettings after = settings;
    return std::make_unique<LambdaCommand>("AI: " + type,
        [write, after, context]() {
            write(after);
            context->requestAssetBrowserRefresh = true;
        },
        [write, before, context]() {
            write(before);
            context->requestAssetBrowserRefresh = true;
        });
}

// WHY VFX と同じ「アセット丸ごとスナップショット」方式にするか:
//     木は最大でも数十ノードで、丸ごと持っても軽い。差分 Undo は
//     「親を付け替えたら order も変わる」ような連動を取りこぼしやすい。
std::unique_ptr<ICommand> BuildBehaviorTreeCommand(editor::EditorContext& ctx,
                                                    const std::string& type,
                                                    const JsonValue& payload,
                                                    Outcome& err,
                                                    JsonValue* detailSink)
{
    namespace fs = std::filesystem;
    fs::path absolute;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), absolute, relative)) {
        err = Outcome::Err("BT_NOT_FOUND", "projectRoot 配下の .behaviortree を指定してください");
        return nullptr;
    }

    // ── Template 取り込みだけは書き出し先が存在しなくてよい ──────────────────
    // WHY 先に分けるか: 以降の処理は「既存の木を読んで一部を書き換える」前提で、
    //     取り込みは「木そのものを差し替える」なので読み込みの成否条件が違う。
    if (type == "bt.template.apply") {
        fs::path templatePath;
        const std::string requested = StringField(payload, "template");
        if (!ResolveBehaviorTreeTemplate(ctx, requested, templatePath)) {
            err = Outcome::Err("BT_TEMPLATE_NOT_FOUND",
                "テンプレートが見つかりません: " + requested
                + " (bt.templateCatalog で名前を確認してください)");
            return nullptr;
        }
        fbzz::ai::BehaviorTreeAsset templateTree;
        std::string templateError;
        if (!fbzz::ai::ParseBehaviorTreeAsset(templatePath.generic_string(), templateTree,
                                              &templateError)) {
            err = Outcome::Err("BT_PARSE_FAILED", templateError);
            return nullptr;
        }
        fbzz::ai::EnsureReservedBlackboardKeys(templateTree);
        if (const JsonValue* value = payload.Find("name"); value != nullptr && value->IsString())
            templateTree.name = value->AsString();
        else templateTree.name = absolute.stem().generic_string();
        // 説明はテンプレートのもの。持ち越すと生成した全ての木が同じ説明を持つ。
        if (const JsonValue* value = payload.Find("description");
            value != nullptr && value->IsString()) templateTree.description = value->AsString();
        else templateTree.description.clear();

        std::string validateError;
        if (!fbzz::ai::ValidateBehaviorTreeAsset(templateTree, &validateError)) {
            err = Outcome::Err("BT_INVALID", validateError);
            return nullptr;
        }

        // 上書き先が既にあれば Undo で戻せるよう中身を控える。
        const bool existed = fs::is_regular_file(absolute);
        fbzz::ai::BehaviorTreeAsset previous;
        if (existed) (void)fbzz::ai::ParseBehaviorTreeAsset(absolute.generic_string(), previous);

        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("template", JsonValue(templatePath.generic_string()));
            report.Set("overwrote", JsonValue(existed));
            report.Set("nodeCount", JsonValue(static_cast<int>(templateTree.nodes.size())));
            // 取り込んだ直後に触る id が判らないと、必ず bt.tree を読み直すことになる。
            JsonValue roots = JsonValue::MakeArray();
            for (const int rootId : templateTree.FindRootIds()) roots.Push(JsonValue(rootId));
            report.Set("roots", std::move(roots));
            *detailSink = std::move(report);
        }

        editor::EditorContext* context = &ctx;
        const std::string target = absolute.generic_string();
        return std::make_unique<LambdaCommand>("AI: Apply Behavior Tree Template",
            [context, target, templateTree]() {
                std::error_code createError;
                fs::create_directories(fs::path(target).parent_path(), createError);
                if (fbzz::ai::SaveBehaviorTreeAsset(target, templateTree))
                    context->requestAssetBrowserRefresh = true;
            },
            [context, target, existed, previous]() {
                std::error_code removeError;
                if (existed) (void)fbzz::ai::SaveBehaviorTreeAsset(target, previous);
                else fs::remove(target, removeError);
                context->requestAssetBrowserRefresh = true;
            });
    }

    if (!fs::is_regular_file(absolute)) {
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
    // 追加・削除・親付け・複製の実体は Editor/GraphEditor/BehaviorTreeOps.hpp。
    // 検査規則を手で写すと Editor 側と黙ってずれる (Docs/design/editor-operator-model.md)。
    if (type == "bt.node.add") {
        fbzz::ai::BTNodeType nodeType{};
        const std::string typeName = StringField(payload, "nodeType");
        if (!editor::btops::FindNodeType(typeName, nodeType)) {
            err = Outcome::Err("BAD_ARG", "未知の BT nodeType です: " + typeName); return nullptr;
        }
        const JsonValue* parentValue = payload.Find("parentId");
        const int requestedParent = parentValue != nullptr ? parentValue->AsInt() : 0;

        // orphanOnReject=false: API 経路なので、繋げないなら追加ごと取り消して
        // 呼び出しを成否で完結させる (孤立ノードが黙って増えない)。
        const editor::btops::AddNodeResult added = editor::btops::AddNode(
            newTree, nodeType, StringField(payload, "name"), requestedParent,
            0.0f, 0.0f, /*orphanOnReject=*/false);
        if (!added.rejectReason.empty()) {
            const bool rootExists = added.rejectReason.rfind("ルートは既にあります", 0) == 0;
            err = Outcome::Err(rootExists ? "BT_ROOT_EXISTS" : "BT_REPARENT_REJECTED",
                               added.rejectReason);
            return nullptr;
        }
    } else if (type == "bt.node.remove") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        if (editor::btops::RemoveSubtree(newTree, nodeId) == 0) {
            err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr;
        }
    } else if (type == "bt.node.setParent") {
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const int parentId = payload.Find("parentId") != nullptr ? payload.Find("parentId")->AsInt() : 0;
        const std::string reason = editor::btops::TryReparentNode(newTree, nodeId, parentId);
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
        // 目録に無い名前と、名前は実在するがその種別が読まないフィールドを分けて弾く。
        // WHY 後者も拒否するか: 保存も Validate も通ってしまい、「設定したのに
        //     行動が変わらない」という最も気づきにくい形でしか現れないため。
        if (FindBTFieldSpec(field) == nullptr) {
            err = Outcome::Err("BT_UNKNOWN_FIELD", "未知のフィールドです: " + field
                               + " (bt.schema の fields を参照してください)");
            return nullptr;
        }
        if (!BTFieldAppliesTo(field, node->type)) {
            err = Outcome::Err("BT_FIELD_NOT_APPLICABLE",
                std::string(fbzz::ai::BTNodeTypeName(node->type)) + " は " + field
                + " を読みません (保存はできますが実行時に無視されます)。"
                  "bt.schema(nodeType=\"" + fbzz::ai::BTNodeTypeName(node->type)
                + "\") でそのノードが読むフィールドを確認してください");
            return nullptr;
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
        } else if (field == "successPolicy") {
            // Parallel の成否。bt.schema が受理値として公開しているので、ここでも受ける。
            const std::string policy = LowerAscii(value->AsString());
            if (policy == "requireone") node->successPolicy = fbzz::ai::BTParallelPolicy::RequireOne;
            else if (policy == "requireall") node->successPolicy = fbzz::ai::BTParallelPolicy::RequireAll;
            else { err = Outcome::Err("BAD_ARG", "successPolicy は requireOne / requireAll"); return nullptr; }
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
    } else if (type == "bt.node.duplicate") {
        // 部分木ごと複製する。Editor の Duplicate Subtree と同一実装なので、
        // 「AI が作った木を人間が触ると形が変わる」食い違いが起きない。
        const int nodeId = payload.Find("nodeId") != nullptr ? payload.Find("nodeId")->AsInt() : 0;
        const fbzz::ai::BTNodeDef* source = newTree.FindNode(nodeId);
        if (source == nullptr) {
            err = Outcome::Err("BT_NODE_NOT_FOUND", "ノードが見つかりません"); return nullptr;
        }
        if (source->parentId == 0) {
            err = Outcome::Err("BT_ROOT_EXISTS", "ルートは複製できません (木にルートは 1 つだけです)");
            return nullptr;
        }
        // 複製先の親。省略すると元と同じ親の末尾へ兄弟として並ぶ。
        const int requestedParent = payload.Find("parentId") != nullptr
            ? payload.Find("parentId")->AsInt() : 0;

        const editor::btops::DuplicateResult duplicated =
            editor::btops::DuplicateSubtree(newTree, nodeId, 40.0f, 40.0f, requestedParent);
        if (duplicated.newRootId == 0) {
            err = Outcome::Err("BT_NODE_NOT_FOUND",
                               duplicated.rejectReason.empty() ? "複製に失敗しました"
                                                               : duplicated.rejectReason);
            return nullptr;
        }
        if (!duplicated.rejectReason.empty()) {
            err = Outcome::Err("BT_REPARENT_REJECTED", duplicated.rejectReason); return nullptr;
        }
        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("rootNodeId", JsonValue(duplicated.newRootId));
            report.Set("copiedNodes", JsonValue(static_cast<int>(duplicated.idMap.size())));
            // 新しい id を返さないと、複製直後に中身を編集するために
            // もう一度 bt.tree を読み直すことになる。
            JsonValue mapping = JsonValue::MakeArray();
            for (const auto& entry : duplicated.idMap) {
                JsonValue item = JsonValue::MakeObject();
                item.Set("from", JsonValue(entry.first));
                item.Set("to", JsonValue(entry.second));
                mapping.Push(std::move(item));
            }
            report.Set("idMap", std::move(mapping));
            *detailSink = std::move(report);
        }
    } else if (type == "bt.repair") {
        // bt.lint が autoFixable=true と言った code だけを機械的に直す。
        // 直し方が一意に決まるものだけを扱い、設計判断 (何をする木か) には触れない。
        const auto flag = [&payload](const char* name) {
            const JsonValue* value = payload.Find(name);
            return value == nullptr || value->AsBool();
        };
        const bool fixAborts    = flag("fixAborts");
        const bool fixDurations = flag("fixDurations");
        const bool fixWeights   = flag("fixWeights");
        const bool fixKeys      = flag("fixKeys");

        JsonValue repaired = JsonValue::MakeArray();
        const auto record = [&repaired](const char* code, int nodeId, std::string detail) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("code", JsonValue(std::string(code)));
            item.Set("nodeId", JsonValue(nodeId));
            item.Set("detail", JsonValue(std::move(detail)));
            repaired.Push(std::move(item));
        };

        // 修復対象は lint が指した nodeId をそのまま使う。同じ判定を書き直すと、
        // lint が指摘した箇所と repair が直す箇所がずれていく。
        const std::vector<fbzz::ai::BTWarning> warnings =
            fbzz::ai::CollectBehaviorTreeWarnings(newTree);
        for (const auto& warning : warnings) {
            fbzz::ai::BTNodeDef* node = newTree.FindNode(warning.nodeId);
            if (node == nullptr) continue;
            if (fixAborts && warning.code == "no-lower-priority-abort") {
                node->abortMode = fbzz::ai::AbortMode::LowerPriority;
                record("no-lower-priority-abort", node->id, "abortMode を lowerPriority にしました");
            } else if (fixDurations && warning.code == "zero-cooldown") {
                node->duration = 1.0f;
                record("zero-cooldown", node->id, "duration を 1.0 秒にしました");
            } else if (fixDurations && warning.code == "zero-duration-wait") {
                node->duration = 1.0f;
                record("zero-duration-wait", node->id, "duration を 1.0 秒にしました");
            } else if (fixWeights && warning.code == "zero-weights") {
                for (float& weight : node->childWeights) weight = 1.0f;
                record("zero-weights", node->id, "全ての重みを 1 (等確率) に戻しました");
            } else if (fixKeys && warning.code == "unresolved-key") {
                // 綴り違いを既存キーへ寄せる。完全一致が無いので、
                // 最も近い名前 (大文字小文字を無視した前方一致 → 部分一致) を選ぶ。
                const auto nearest = [&newTree](const std::string& wanted) -> std::string {
                    if (wanted.empty()) return {};
                    const std::string lowered = LowerAscii(wanted);
                    std::string best;
                    for (const auto& def : newTree.blackboard) {
                        const std::string candidate = LowerAscii(def.name);
                        if (candidate == lowered) return def.name;
                        const bool related = candidate.starts_with(lowered)
                                          || lowered.starts_with(candidate)
                                          || candidate.find(lowered) != std::string::npos
                                          || lowered.find(candidate) != std::string::npos;
                        // 同じくらい近いなら短い方 (余計な修飾が付いていない方) を採る。
                        if (related && (best.empty() || def.name.size() < best.size()))
                            best = def.name;
                    }
                    return best;
                };
                if (const std::string replacement = nearest(node->keyName);
                    !replacement.empty() && replacement != node->keyName) {
                    record("unresolved-key", node->id,
                           "keyName \"" + node->keyName + "\" を \"" + replacement + "\" へ変更しました");
                    node->keyName = replacement;
                }
                if (const std::string replacement = nearest(node->moveTargetKey);
                    !replacement.empty() && replacement != node->moveTargetKey) {
                    record("unresolved-key", node->id,
                           "moveTargetKey \"" + node->moveTargetKey + "\" を \""
                           + replacement + "\" へ変更しました");
                    node->moveTargetKey = replacement;
                }
            }
        }
        if (detailSink != nullptr) {
            JsonValue report = JsonValue::MakeObject();
            report.Set("repaired", std::move(repaired));
            // 直せなかったものを残す。空配列を返さないと「全部直った」と読まれる。
            JsonValue remaining = JsonValue::MakeArray();
            for (const auto& warning : fbzz::ai::CollectBehaviorTreeWarnings(newTree)) {
                JsonValue item = JsonValue::MakeObject();
                item.Set("nodeId", JsonValue(warning.nodeId));
                item.Set("code", JsonValue(warning.code));
                item.Set("message", JsonValue(warning.message));
                const BTFixHint* hint = FindBTFixHint(warning.code);
                item.Set("autoFixable", JsonValue(hint != nullptr && hint->autoFixable));
                remaining.Push(std::move(item));
            }
            report.Set("remaining", std::move(remaining));
            *detailSink = std::move(report);
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
        // 間隔の定数ごと共有実装が持つ。数値を写すとノード幅を変えた瞬間に黙ってずれる。
        editor::btops::AutoLayout(newTree);
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
    // Undo 履歴に何をした操作か残す。全部が "Edit Behavior Tree" だと、
    // editor_get_undo_history で自分の編集を identify できない。
    std::string commandLabel = "AI: Edit Behavior Tree";
    if (type == "bt.node.add") commandLabel = "AI: Add Behavior Tree Node";
    else if (type == "bt.node.remove") commandLabel = "AI: Remove Behavior Tree Node";
    else if (type == "bt.node.duplicate") commandLabel = "AI: Duplicate Behavior Tree Subtree";
    else if (type == "bt.node.setParent") commandLabel = "AI: Reparent Behavior Tree Node";
    else if (type == "bt.node.setOrder") commandLabel = "AI: Set Behavior Tree Priority";
    else if (type == "bt.node.setField") commandLabel = "AI: Set Behavior Tree Field";
    else if (type == "bt.blackboard.add") commandLabel = "AI: Add Blackboard Key";
    else if (type == "bt.blackboard.remove") commandLabel = "AI: Remove Blackboard Key";
    else if (type == "bt.autoLayout") commandLabel = "AI: Auto Layout Behavior Tree";
    else if (type == "bt.repair") commandLabel = "AI: Repair Behavior Tree";
    return std::make_unique<LambdaCommand>(std::move(commandLabel),
        [context, target, newTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, newTree)) context->requestAssetBrowserRefresh = true;
        },
        [context, target, oldTree]() {
            if (fbzz::ai::SaveBehaviorTreeAsset(target, oldTree)) context->requestAssetBrowserRefresh = true;
        });
}

// 1つの mutating Command を Undo 可能な ICommand へ変換する (実行はしない)。失敗時 nullptr + err。
// createdSink != nullptr のとき生成系 Command は代表ルートの instanceId をそこへ書き込む。
// detailSink != nullptr のとき、Command が「適用の副作用」を応答へ載せたい場合にそこへ書く。
//   applied:true だけでは Template 取り込みでの改名も budget の引き上げも伝わらず、
//   次の手で存在しない名前を指してしまう。黙って起きる変更は必ず応答へ載せる。
// ═════════════════════════════════════════════════════════════════════════════
// ワールドオーサリング (Scene 入出力 / Terrain / NavMesh / Environment / Audio / UI / Build)
//
// ここまでの Query/Command は「シーンに置いたオブジェクトとそのコンポーネント」を扱うが、
// 屋外シーンの実体は地形の高さ・スプラット・植生・NavMesh・空と光の設定でできている。
// どれもブラシとベイクでしか変えられず、component.set だけでは読むことすらできない。
// シーンの移動手段 (scene.list / scene.open / scene.save) もここが持つ。
// ═════════════════════════════════════════════════════════════════════════════

// projectRoot 配下を走査して .scene を列挙する。
// WHY: AI が扱えるのは「今開いているシーン」だけで、他に何があるのかを知る手段が無かった。
//      Library/Baked は生成物で編集対象ではないため除外する (asset.list と同じ方針)。
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
            // 生成物・VCS ディレクトリへは降りない (走査時間と応答量の両方を無駄にする)。
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
    // dirty のときは scene.open が拒否される。先に知れないと必ず 1 往復無駄になる。
    result.Set("dirty", JsonValue(ctx.sceneDirty));
    result.Set("prefabEditMode", JsonValue(ctx.InPrefabEditMode()));
    result.Set("scenes", std::move(scenes));
    return Outcome::Ok(std::move(result));
}

// アクティブシーンを切り替える。未保存変更は discardUnsaved を明示しない限り拒否する。
// 確認モーダルを開くと人がクリックするまでバスの drain が止まり、以降の要求も返らない。
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
    // 開き直すと Undo スタックは破棄される (別シーンの EntityID を持つコマンドは復元できない)。
    result.Set("undoCleared", JsonValue(true));
    return Outcome::Ok(std::move(result));
}

// 現在のシーンを保存する。path 省略で上書き、指定で別名保存 (以降のカレントもそのパスになる)。
Outcome DoSceneSave(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    if (!ctx.saveScenePathImmediate) return Outcome::Err("NO_HOST", "シーン保存機能が未接続です");
    if (ctx.InPrefabEditMode()) return Outcome::Err("PREFAB_EDIT_MODE", "Prefab 編集モード中はシーンを保存できません");
    // Play 中に保存すると、走っているシーン (遷移後なら別ファイルの中身) を
    // currentScenePath へ書き込むことになる。
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
        // 名前の決定は設計判断なので機械的に埋めない。AI に明示させる。
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

// ── Add Object プリセット ───────────────────────────────────────────────────

// Hierarchy の Add Object メニューと同じ登録表を返す。
// 自力で組み立てさせると毎回中身が変わり、人が置いた Cube と AI が置いた Cube が別物になる。
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

// ── Terrain ─────────────────────────────────────────────────────────────────

// heightData / splatData の統計を返す。値そのもの (65x65 で 4225 個) は返さない。
// 判断に使うのは「どれくらい起伏があるか」の要約で、生データは context を食い潰す。
// 特定地点の実値が要るときは terrain.sample で点を指定して読む。
JsonValue TerrainStatsJson(const scene::TerrainComponent& terrain)
{
    JsonValue stats = JsonValue::MakeObject();
    const size_t expected = static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows);
    const bool hasHeight = terrain.heightData.size() == expected && expected > 0;
    stats.Set("hasHeightData", JsonValue(hasHeight));
    if (hasHeight) {
        float minValue = terrain.heightData[0];
        float maxValue = terrain.heightData[0];
        double sum = 0.0;
        for (float value : terrain.heightData) {
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
            sum += static_cast<double>(value);
        }
        // 正規化値ではなくワールド高さで返す。AI が指定する targetHeight と単位を揃えるため。
        stats.Set("minHeight", JsonValue(minValue * terrain.maxHeight));
        stats.Set("maxHeight", JsonValue(maxValue * terrain.maxHeight));
        stats.Set("meanHeight", JsonValue(sum / static_cast<double>(expected) * terrain.maxHeight));
        stats.Set("flat", JsonValue((maxValue - minValue) * terrain.maxHeight < 0.001f));
    }
    const bool hasSplat = terrain.splatData.size() == expected * 4u && expected > 0;
    stats.Set("hasSplatData", JsonValue(hasSplat));
    if (hasSplat) {
        double channelSum[4] = { 0.0, 0.0, 0.0, 0.0 };
        for (size_t i = 0; i < expected; ++i) {
            for (int c = 0; c < 4; ++c)
                channelSum[c] += static_cast<double>(terrain.splatData[i * 4u + static_cast<size_t>(c)]);
        }
        JsonValue coverage = JsonValue::MakeArray();
        for (int c = 0; c < 4; ++c)
            coverage.Push(JsonValue(channelSum[c] / (static_cast<double>(expected) * 255.0)));
        stats.Set("layerCoverage", std::move(coverage));
    }
    return stats;
}

Outcome DoTerrainInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue terrains = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::TerrainComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* terrain = activeScene->GetComponent<scene::TerrainComponent>(eid);
        if (go == nullptr || terrain == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;

        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("enabled", JsonValue(terrain->enabled));
        entry.Set("columns", JsonValue(terrain->columns));
        entry.Set("rows", JsonValue(terrain->rows));
        entry.Set("cellSize", JsonValue(terrain->cellSize));
        entry.Set("maxHeight", JsonValue(terrain->maxHeight));
        entry.Set("chunkSize", JsonValue(terrain->chunkSize));
        entry.Set("terrainAssetPath", JsonValue(terrain->terrainAssetPath));
        // ローカル寸法とワールド原点。ブラシ位置をワールドで指定するために両方要る。
        entry.Set("localSize", VectorToJson({
            static_cast<float>(terrain->columns - 1) * terrain->cellSize,
            terrain->maxHeight,
            static_cast<float>(terrain->rows - 1) * terrain->cellSize }));
        entry.Set("worldOrigin", VectorToJson(go->transform.worldPosition));
        JsonValue layers = JsonValue::MakeArray();
        for (int i = 0; i < 4; ++i) {
            JsonValue layer = JsonValue::MakeObject();
            layer.Set("index", JsonValue(i));
            layer.Set("material", JsonValue(terrain->layerMaterials[static_cast<size_t>(i)]));
            layers.Push(std::move(layer));
        }
        entry.Set("layers", std::move(layers));
        entry.Set("stats", TerrainStatsJson(*terrain));
        terrains.Push(std::move(entry));
    }

    if (!filterId.empty() && terrains.AsArray().empty())
        return Outcome::Err("NOT_PRESENT", "TerrainComponent を持つノードが見つかりません: " + filterId);

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(terrains.AsArray().size())));
    result.Set("terrains", std::move(terrains));
    return Outcome::Ok(std::move(result));
}

// ワールド座標のブラシ中心に対して、重なる Terrain を列挙する。
struct TerrainHit {
    GameObject*              go = nullptr;
    scene::TerrainComponent* terrain = nullptr;
    math::Vector3            local;   // Terrain ローカル座標へ変換したブラシ中心
};

std::vector<TerrainHit> CollectTerrainsUnderBrush(scene::Scene& activeScene,
                                                  const math::Vector3& worldCenter,
                                                  float radius,
                                                  const std::string& restrictToNodeId)
{
    std::vector<TerrainHit> hits;
    for (scene::EntityID eid : activeScene.GetEntities<scene::TerrainComponent>()) {
        GameObject* go = activeScene.GetGameObject(eid);
        auto* terrain = activeScene.GetComponent<scene::TerrainComponent>(eid);
        if (go == nullptr || terrain == nullptr || !terrain->enabled) continue;
        if (!restrictToNodeId.empty() && go->instanceId != restrictToNodeId) continue;
        const math::Vector3 local = ToTerrainLocal(go->transform, worldCenter);
        // 半径 0 の問い合わせ (sample) でも矩形内なら拾えるよう、下限を 0 として扱う。
        if (!BrushOverlapsTerrainXZ(*terrain, local, std::max(radius, 0.0f))) continue;
        hits.push_back({ go, terrain, local });
    }
    return hits;
}

// 指定ワールド点の高さ・法線・レイヤー重みを返す。オブジェクトを置く前の下見や、
// sculpt 後に「本当に平らになったか」を数値で確かめるのに使う。
Outcome DoTerrainSample(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* points = payload.Find("points");
    if (points == nullptr || !points->IsArray() || points->AsArray().empty())
        return Outcome::Err("BAD_ARG", "points ([[x,y,z], ...]) が必要です");
    if (points->AsArray().size() > 256)
        return Outcome::Err("BAD_ARG", "points は 256 点までです");
    const std::string restrictTo = StringField(payload, "id");

    JsonValue samples = JsonValue::MakeArray();
    for (const JsonValue& pointValue : points->AsArray()) {
        if (!pointValue.IsArray() || pointValue.AsArray().size() < 2) continue;
        const auto& array = pointValue.AsArray();
        // [x, z] の 2 要素も許す (高さを問い合わせるのに y は不要なため)。
        const bool hasY = array.size() >= 3;
        const math::Vector3 world{
            static_cast<float>(array[0].AsNumber()),
            hasY ? static_cast<float>(array[1].AsNumber()) : 0.0f,
            static_cast<float>(array[hasY ? 2 : 1].AsNumber())
        };
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("query", VectorToJson(world));
        const std::vector<TerrainHit> hits = CollectTerrainsUnderBrush(*activeScene, world, 0.0f, restrictTo);
        if (hits.empty()) {
            sample.Set("onTerrain", JsonValue(false));
            samples.Push(std::move(sample));
            continue;
        }
        const TerrainHit& hit = hits.front();
        const scene::TerrainComponent& terrain = *hit.terrain;
        const float localHeight = terrain.GetHeightAt(hit.local.x, hit.local.z);
        const math::Vector3 localNormal = terrain.GetNormalAt(hit.local.x, hit.local.z);
        sample.Set("onTerrain", JsonValue(true));
        sample.Set("terrainId", JsonValue(hit.go->instanceId));
        sample.Set("local", VectorToJson({ hit.local.x, localHeight, hit.local.z }));
        // 高さはワールド Y で返す (ブラシの targetHeight もワールド系で受けるため)。
        sample.Set("worldHeight", JsonValue(ToTerrainWorld(hit.go->transform,
            { hit.local.x, localHeight, hit.local.z }).y));
        sample.Set("normal", VectorToJson(localNormal));
        // 斜度は NavMesh の歩行可否と直結するので、法線から算出して添える。
        sample.Set("slopeDegrees", JsonValue(math::ToDeg(std::acos(
            std::clamp(localNormal.y, -1.0f, 1.0f)))));
        const size_t expected = static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows);
        if (terrain.splatData.size() == expected * 4u && expected > 0) {
            const int gx = std::clamp(static_cast<int>(hit.local.x / terrain.cellSize + 0.5f), 0, terrain.columns - 1);
            const int gz = std::clamp(static_cast<int>(hit.local.z / terrain.cellSize + 0.5f), 0, terrain.rows - 1);
            const size_t base = (static_cast<size_t>(gz) * static_cast<size_t>(terrain.columns)
                               + static_cast<size_t>(gx)) * 4u;
            JsonValue weights = JsonValue::MakeArray();
            for (int c = 0; c < 4; ++c)
                weights.Push(JsonValue(static_cast<float>(terrain.splatData[base + static_cast<size_t>(c)]) / 255.0f));
            sample.Set("layerWeights", std::move(weights));
        }
        samples.Push(std::move(sample));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(samples.AsArray().size())));
    result.Set("samples", std::move(samples));
    return Outcome::Ok(std::move(result));
}

// payload からブラシ形状を読む。共通なので sculpt / paint の両方で使う。
bool ReadTerrainBrush(const JsonValue& payload, TerrainBrush& outBrush, std::string& outError)
{
    if (const JsonValue* v = payload.Find("radius"); v != nullptr && v->IsNumber())
        outBrush.radius = static_cast<float>(v->AsNumber());
    if (const JsonValue* v = payload.Find("strength"); v != nullptr && v->IsNumber())
        outBrush.strength = static_cast<float>(v->AsNumber());
    const std::string falloff = LowerAscii(StringField(payload, "falloff"));
    if (!falloff.empty()) {
        if (falloff == "linear")        outBrush.falloff = TerrainFalloff::Linear;
        else if (falloff == "smooth")   outBrush.falloff = TerrainFalloff::Smooth;
        else if (falloff == "gaussian") outBrush.falloff = TerrainFalloff::Gaussian;
        else { outError = "falloff は linear / smooth / gaussian です"; return false; }
    }
    if (!(outBrush.radius > 0.0f) || outBrush.radius > 500.0f) {
        outError = "radius は 0 より大きく 500 以下で指定してください"; return false;
    }
    if (!(outBrush.strength > 0.0f) || outBrush.strength > 1.0f) {
        outError = "strength は 0 より大きく 1 以下で指定してください"; return false;
    }
    return true;
}

// Terrain の Undo は「触れた Terrain の丸ごとスナップショット」で戻す (TerrainTool と同じ方式)。
// WHY: heightData / splatData は差分の記述が複雑で、部分復元を書くと Resize や
//      レイヤー入れ替えと組み合わせたときに壊れる。ストローク単位のコピーで揃える。
struct TerrainSnapshot {
    std::string             instanceId;
    scene::TerrainComponent component;
};

std::unique_ptr<ICommand> MakeTerrainEditCommand(scene::Scene* activeScene,
                                                 const char* label,
                                                 std::vector<TerrainSnapshot> before,
                                                 std::vector<TerrainSnapshot> after,
                                                 std::function<void()> markDirty)
{
    auto apply = [activeScene, markDirty](const std::vector<TerrainSnapshot>& values) {
        for (const TerrainSnapshot& snapshot : values) {
            GameObject* target = activeScene->FindByGuid(snapshot.instanceId);
            if (target == nullptr) continue;
            auto* component = target->GetComponent<scene::TerrainComponent>();
            if (component == nullptr) continue;
            *component = snapshot.component;
            component->heightDirty = true;
            component->splatDirty = true;
            component->colliderDirty = true;
        }
        if (markDirty) markDirty();
    };
    auto beforeShared = std::make_shared<std::vector<TerrainSnapshot>>(std::move(before));
    auto afterShared  = std::make_shared<std::vector<TerrainSnapshot>>(std::move(after));
    return std::make_unique<LambdaCommand>(label,
        [apply, afterShared]()  { apply(*afterShared); },
        [apply, beforeShared]() { apply(*beforeShared); });
}

// ── NavMesh ─────────────────────────────────────────────────────────────────

// Surface の設定・ベイク結果・XZ バウンドを返す。
JsonValue NavMeshSurfaceJson(GameObject& go, const scene::NavMeshSurfaceComponent& surface)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue(go.instanceId));
    entry.Set("name", JsonValue(go.name));
    entry.Set("enabled", JsonValue(surface.enabled));
    entry.Set("agentTypeId", JsonValue(surface.agentTypeId));
    entry.Set("collectObjects", JsonValue(
        surface.collectObjects == scene::NavMeshCollectObjects::Volume ? "volume" : "thisObject"));
    entry.Set("size", VectorToJson(surface.size));
    entry.Set("cellSize", JsonValue(surface.cellSize));
    entry.Set("maxSlopeAngleDeg", JsonValue(surface.maxSlopeAngleDeg));
    entry.Set("agentRadius", JsonValue(surface.agentRadius));
    entry.Set("agentHeight", JsonValue(surface.agentHeight));
    entry.Set("maxClimb", JsonValue(surface.maxClimb));
    const char* stateName = "idle";
    if (surface.bakeState == scene::NavMeshBakeState::Baking)    stateName = "baking";
    else if (surface.bakeState == scene::NavMeshBakeState::Done) stateName = "done";
    entry.Set("bakeState", JsonValue(stateName));
    entry.Set("bakeProgress", JsonValue(surface.bakeProgress));
    entry.Set("needsBake", JsonValue(surface.needsBake));
    entry.Set("polygonCount", JsonValue(static_cast<int>(surface.navMesh.polygons.size())));
    entry.Set("offMeshLinkCount", JsonValue(static_cast<int>(surface.navMesh.offMeshLinks.size())));
    // ベイクが「終わったが空だった」ときの理由と、セル判定の内訳。
    // WHY: polygonCount=0 だけでは、傾斜・障害物・エージェント半径のどれで落ちたのかが
    //      画を撮っても数値を読んでも分からず、AI は設定を総当たりするしかなかった。
    if (!surface.bakeStats.failReason.empty())
        entry.Set("failReason", JsonValue(surface.bakeStats.failReason));
    if (surface.bakeStats.cellsX > 0) {
        JsonValue bakeInfo = JsonValue::MakeObject();
        bakeInfo.Set("seconds",     JsonValue(surface.bakeStats.bakeSeconds));
        bakeInfo.Set("cellsX",      JsonValue(surface.bakeStats.cellsX));
        bakeInfo.Set("cellsZ",      JsonValue(surface.bakeStats.cellsZ));
        bakeInfo.Set("walkable",    JsonValue(surface.bakeStats.walkableCells));
        bakeInfo.Set("tooSteep",    JsonValue(surface.bakeStats.steepCells));
        bakeInfo.Set("tooHighStep", JsonValue(surface.bakeStats.stepCells));
        bakeInfo.Set("obstructed",  JsonValue(surface.bakeStats.obstructedCells));
        bakeInfo.Set("eroded",      JsonValue(surface.bakeStats.erodedCells));
        bakeInfo.Set("areaSquareMeters", JsonValue(surface.bakeStats.areaSquareMeters));
        entry.Set("bake", std::move(bakeInfo));
    }
    // 歩ける範囲そのもの。AI が「どこを目的地に選べるか」を知る唯一の手掛かりになる。
    if (!surface.navMesh.polygons.empty()) {
        math::Vector3 boundsMin{ 1e30f, 1e30f, 1e30f };
        math::Vector3 boundsMax{ -1e30f, -1e30f, -1e30f };
        std::unordered_map<int, int> areaHistogram;
        for (const scene::NavMeshPolygon& polygon : surface.navMesh.polygons) {
            ++areaHistogram[polygon.areaType];
            for (const math::Vector3& vertex : polygon.vertices) {
                boundsMin = { std::min(boundsMin.x, vertex.x), std::min(boundsMin.y, vertex.y), std::min(boundsMin.z, vertex.z) };
                boundsMax = { std::max(boundsMax.x, vertex.x), std::max(boundsMax.y, vertex.y), std::max(boundsMax.z, vertex.z) };
            }
        }
        JsonValue bounds = JsonValue::MakeObject();
        bounds.Set("min", VectorToJson(boundsMin));
        bounds.Set("max", VectorToJson(boundsMax));
        entry.Set("bounds", std::move(bounds));
        JsonValue areas = JsonValue::MakeArray();
        for (const auto& [areaType, count] : areaHistogram) {
            JsonValue area = JsonValue::MakeObject();
            area.Set("areaType", JsonValue(areaType));
            area.Set("polygons", JsonValue(count));
            area.Set("cost", JsonValue(surface.areaCosts[static_cast<size_t>(std::clamp(areaType, 0, 31))]));
            areas.Push(std::move(area));
        }
        entry.Set("areas", std::move(areas));
    }
    return entry;
}

Outcome DoNavMeshState(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue surfaces = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* surface = activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (go == nullptr || surface == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        JsonValue entry = NavMeshSurfaceJson(*go, *surface);
        // ベイク後に地形や Modifier が動いていれば、この NavMesh はもう現状と合っていない。
        // WHY ここで出すか: 古い NavMesh でも navmesh_find_path は成功を返すので、
        //      「壁を通り抜ける経路」を得たあとでしか気付けなかった。
        if (surface->navMesh.IsValid()) {
            const uint64_t current = scene::HashNavMeshBakeSources(*activeScene, eid);
            entry.Set("stale", JsonValue(current != surface->bakedSourceHash));
        }
        surfaces.Push(std::move(entry));
    }

    // Agent 側も併せて返す。「経路が引けない」の原因が Surface 側か Agent 設定側かは、
    // 両方を並べて初めて切り分けられる (agentTypeId の食い違いが典型)。
    JsonValue agents = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::NavMeshAgentComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* agent = activeScene->GetComponent<scene::NavMeshAgentComponent>(eid);
        if (go == nullptr || agent == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("enabled", JsonValue(agent->enabled));
        entry.Set("agentTypeId", JsonValue(agent->agentTypeId));
        entry.Set("areaMask", JsonValue(agent->areaMask));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        entry.Set("hasDestination", JsonValue(agent->hasDestination));
        if (agent->hasDestination) entry.Set("destination", VectorToJson(agent->destination));
        const char* agentState = "idle";
        if (agent->state == scene::NavMeshAgentState::MOVING)                agentState = "moving";
        else if (agent->state == scene::NavMeshAgentState::TRAVERSING_LINK)  agentState = "traversingLink";
        entry.Set("state", JsonValue(agentState));
        entry.Set("pathWaypoints", JsonValue(static_cast<int>(agent->path.size())));
        entry.Set("currentWaypoint", JsonValue(static_cast<int>(agent->currentWaypoint)));
        entry.Set("isStopped", JsonValue(agent->isStopped));
        entry.Set("destinationReached", JsonValue(agent->destinationReached));
        entry.Set("currentSpeed", JsonValue(agent->currentSpeed));
        agents.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceCount", JsonValue(static_cast<int>(surfaces.AsArray().size())));
    result.Set("surfaces", std::move(surfaces));
    result.Set("agentCount", JsonValue(static_cast<int>(agents.AsArray().size())));
    result.Set("agents", std::move(agents));
    return Outcome::Ok(std::move(result));
}

// agentTypeId に一致し、ベイク済みの Surface を選ぶ。id 指定があればそれを優先する。
scene::NavMeshSurfaceComponent* ResolveNavMeshSurface(scene::Scene& activeScene,
                                                      const std::string& nodeId,
                                                      int agentTypeId,
                                                      GameObject** outGo)
{
    for (scene::EntityID eid : activeScene.GetEntities<scene::NavMeshSurfaceComponent>()) {
        GameObject* go = activeScene.GetGameObject(eid);
        auto* surface = activeScene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (go == nullptr || surface == nullptr) continue;
        if (!nodeId.empty()) {
            if (go->instanceId != nodeId) continue;
        } else {
            if (!surface->enabled || surface->agentTypeId != agentTypeId) continue;
            if (!surface->navMesh.IsValid()) continue;
        }
        if (outGo != nullptr) *outGo = go;
        return surface;
    }
    return nullptr;
}

// 2 点間の経路を、Agent が実際に使うのと同じ A* + Funnel で引く。
// 「敵がここへ来ない」の原因は BT の条件・Agent の設定・NavMesh の穴の 3 通りで、
// 3 つ目は経路そのものを引いてみるまで分からない。
Outcome DoNavMeshPath(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    math::Vector3 from;
    math::Vector3 to;
    // from は座標でも Agent の NodeId でも指定できる (「今いる場所から」が最も多い問い合わせ)。
    const std::string fromNodeId = StringField(payload, "fromId");
    int agentTypeId = 0;
    int areaMask = -1;
    if (const JsonValue* v = payload.Find("agentTypeId"); v != nullptr && v->IsNumber())
        agentTypeId = v->AsInt();
    if (const JsonValue* v = payload.Find("areaMask"); v != nullptr && v->IsNumber())
        areaMask = v->AsInt();
    if (!fromNodeId.empty()) {
        GameObject* fromGo = activeScene->FindByGuid(fromNodeId);
        if (fromGo == nullptr) return Outcome::Err("NODE_NOT_FOUND", "fromId が見つかりません: " + fromNodeId);
        from = fromGo->transform.worldPosition;
        // Agent があればその agentTypeId / areaMask を既定として引き継ぐ。
        if (auto* agent = fromGo->GetComponent<scene::NavMeshAgentComponent>()) {
            if (payload.Find("agentTypeId") == nullptr) agentTypeId = agent->agentTypeId;
            if (payload.Find("areaMask") == nullptr)    areaMask = agent->areaMask;
        }
    } else if (!ReadVec3(payload, "from", from)) {
        return Outcome::Err("BAD_ARG", "from ([x,y,z]) または fromId が必要です");
    }
    const std::string toNodeId = StringField(payload, "toId");
    if (!toNodeId.empty()) {
        GameObject* toGo = activeScene->FindByGuid(toNodeId);
        if (toGo == nullptr) return Outcome::Err("NODE_NOT_FOUND", "toId が見つかりません: " + toNodeId);
        to = toGo->transform.worldPosition;
    } else if (!ReadVec3(payload, "to", to)) {
        return Outcome::Err("BAD_ARG", "to ([x,y,z]) または toId が必要です");
    }

    GameObject* surfaceGo = nullptr;
    scene::NavMeshSurfaceComponent* surface =
        ResolveNavMeshSurface(*activeScene, StringField(payload, "surfaceId"), agentTypeId, &surfaceGo);
    if (surface == nullptr) {
        return Outcome::Err("NO_NAVMESH",
            "agentTypeId=" + std::to_string(agentTypeId) + " に対応するベイク済み NavMesh Surface がありません");
    }
    if (!surface->navMesh.IsValid())
        return Outcome::Err("NAVMESH_NOT_BAKED", "NavMesh が未ベイクです。navmesh_bake を実行してください");

    const scene::NavMesh& navMesh = surface->navMesh;
    const int startPoly = scene::FindNearestPolygon(navMesh, from);
    const int goalPoly  = scene::FindNearestPolygon(navMesh, to);

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceId", JsonValue(surfaceGo != nullptr ? surfaceGo->instanceId : std::string{}));
    result.Set("agentTypeId", JsonValue(agentTypeId));
    result.Set("areaMask", JsonValue(areaMask));
    result.Set("from", VectorToJson(from));
    result.Set("to", VectorToJson(to));
    result.Set("startPolygon", JsonValue(startPoly));
    result.Set("goalPolygon", JsonValue(goalPoly));

    if (startPoly < 0 || goalPoly < 0) {
        result.Set("found", JsonValue(false));
        result.Set("reason", JsonValue("START_OR_GOAL_OFF_NAVMESH"));
        return Outcome::Ok(std::move(result));
    }

    std::vector<int> polyPath;
    if (!scene::FindPolygonPath(navMesh, startPoly, goalPoly, polyPath, areaMask, surface->areaCosts, agentTypeId)) {
        result.Set("found", JsonValue(false));
        // 到達不能とエリアマスクによる遮断は別物だが、A* からは区別できない。
        // areaMask を返してあるので、-1 で引き直せば切り分けられる。
        result.Set("reason", JsonValue("NO_PATH"));
        return Outcome::Ok(std::move(result));
    }

    const std::vector<math::Vector3> corners = scene::BuildFunnelPath(navMesh, polyPath, from, to);
    JsonValue cornerArray = JsonValue::MakeArray();
    float length = 0.0f;
    for (size_t i = 0; i < corners.size(); ++i) {
        cornerArray.Push(VectorToJson(corners[i]));
        if (i > 0) length += (corners[i] - corners[i - 1]).Length();
    }
    result.Set("found", JsonValue(true));
    result.Set("corners", std::move(cornerArray));
    result.Set("cornerCount", JsonValue(static_cast<int>(corners.size())));
    result.Set("polygonCount", JsonValue(static_cast<int>(polyPath.size())));
    result.Set("length", JsonValue(length));
    // 直線距離との比。大きいほど遠回り = 障害物か穴を迂回している。
    const float straight = (to - from).Length();
    result.Set("straightDistance", JsonValue(straight));
    result.Set("detourRatio", JsonValue(straight > 0.0001f ? length / straight : 1.0f));
    return Outcome::Ok(std::move(result));
}

// 指定点が NavMesh 上か、面上ならその高さを返す。Agent の湧き位置を決めるのに使う。
Outcome DoNavMeshSample(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* points = payload.Find("points");
    if (points == nullptr || !points->IsArray() || points->AsArray().empty())
        return Outcome::Err("BAD_ARG", "points ([[x,y,z], ...]) が必要です");
    if (points->AsArray().size() > 256) return Outcome::Err("BAD_ARG", "points は 256 点までです");

    int agentTypeId = 0;
    if (const JsonValue* v = payload.Find("agentTypeId"); v != nullptr && v->IsNumber())
        agentTypeId = v->AsInt();
    GameObject* surfaceGo = nullptr;
    scene::NavMeshSurfaceComponent* surface =
        ResolveNavMeshSurface(*activeScene, StringField(payload, "surfaceId"), agentTypeId, &surfaceGo);
    if (surface == nullptr || !surface->navMesh.IsValid())
        return Outcome::Err("NO_NAVMESH", "ベイク済み NavMesh Surface がありません");

    JsonValue samples = JsonValue::MakeArray();
    for (const JsonValue& pointValue : points->AsArray()) {
        if (!pointValue.IsArray() || pointValue.AsArray().size() < 3) continue;
        const auto& array = pointValue.AsArray();
        const math::Vector3 query{
            static_cast<float>(array[0].AsNumber()),
            static_cast<float>(array[1].AsNumber()),
            static_cast<float>(array[2].AsNumber())
        };
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("query", VectorToJson(query));
        const int polygon = scene::FindNearestPolygon(surface->navMesh, query);
        if (polygon < 0) {
            sample.Set("onNavMesh", JsonValue(false));
            samples.Push(std::move(sample));
            continue;
        }
        const bool inside = surface->navMesh.polygons[static_cast<size_t>(polygon)].ContainsXZ(query.x, query.z);
        const float height = scene::SampleNavMeshHeight(surface->navMesh, query);
        sample.Set("onNavMesh", JsonValue(inside));
        sample.Set("polygon", JsonValue(polygon));
        sample.Set("areaType", JsonValue(surface->navMesh.polygons[static_cast<size_t>(polygon)].areaType));
        // 面の外なら最近傍ポリゴンへ寄せた点を返す。Agent を置き直す座標としてそのまま使える。
        sample.Set("nearest", VectorToJson({ query.x, height <= -1e6f ? query.y : height, query.z }));
        if (!inside) {
            sample.Set("distanceXZ", JsonValue(std::sqrt(
                surface->navMesh.polygons[static_cast<size_t>(polygon)].DistanceSqXZ(query.x, query.z))));
        }
        samples.Push(std::move(sample));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceId", JsonValue(surfaceGo != nullptr ? surfaceGo->instanceId : std::string{}));
    result.Set("count", JsonValue(static_cast<int>(samples.AsArray().size())));
    result.Set("samples", std::move(samples));
    return Outcome::Ok(std::move(result));
}

// ── Environment (空・光・霧・ポストプロセス) ─────────────────────────────────

// 環境系コンポーネントの一覧。Reflect 済みの値をそのまま返すので、editor_catalog の
// フィールド定義と 1 対 1 で対応し、component_set でそのまま書き戻せる。
// 空・太陽・霧・IBL・雲・ポストは別々の GameObject に散らばっており、
// 「今この画がなぜこの明るさなのか」を 1 回で読めるようにする。
Outcome DoEnvironmentInspect(editor::EditorContext& ctx)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    // 対象は ComponentCategory::Environment に登録された型すべて。
    // 名前表を書くと、新しい環境コンポーネントが「AI からだけ見えない」状態になる。
    std::unordered_set<std::string> environmentTypes;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::category == scene::ComponentCategory::Environment
                   && Reg::inspectorMode != scene::ComponentInspectorMode::Hidden) {
            environmentTypes.insert(Reg::serializedName);
        }
    });

    JsonValue nodes = JsonValue::MakeArray();
    for (GameObject& gameObject : activeScene->GameObjects()) {
        GameObject* go = &gameObject;
        JsonValue components = SnapshotComponents(*go);
        JsonValue matched = JsonValue::MakeArray();
        for (const JsonValue& component : components.AsArray()) {
            const JsonValue* typeValue = component.Find("type");
            if (typeValue == nullptr || !typeValue->IsString()) continue;
            if (environmentTypes.count(typeValue->AsString()) != 0) matched.Push(component);
        }
        if (matched.AsArray().empty()) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("components", std::move(matched));
        nodes.Push(std::move(entry));
    }

    // ライトは環境の一部だが数が多いので、種別と強度だけの要約にする。
    JsonValue lights = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::LightComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* light = activeScene->GetComponent<scene::LightComponent>(eid);
        if (go == nullptr || light == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        entry.Set("forward", VectorToJson(go->transform.Forward()));
        JsonReadReflector reader;
        light->Reflect(reader);
        entry.Set("fields", reader.Result());
        lights.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("nodeCount", JsonValue(static_cast<int>(nodes.AsArray().size())));
    result.Set("nodes", std::move(nodes));
    result.Set("lightCount", JsonValue(static_cast<int>(lights.AsArray().size())));
    result.Set("lights", std::move(lights));
    return Outcome::Ok(std::move(result));
}

// ── Audio ───────────────────────────────────────────────────────────────────

Outcome DoAudioInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue sources = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::AudioSourceComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* source = activeScene->GetComponent<scene::AudioSourceComponent>(eid);
        if (go == nullptr || source == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        JsonReadReflector reader;
        source->Reflect(reader);
        entry.Set("fields", reader.Result());
        // 再生状態は Reflect に載らない (保存対象ではない) ので明示的に足す。
        // これが無いと「鳴っているのか」を画面のスピーカーアイコン以外で確認できない。
        JsonValue runtime = JsonValue::MakeObject();
        runtime.Set("playing", JsonValue(source->m_isPlaying));
        runtime.Set("paused", JsonValue(source->m_isPaused));
        runtime.Set("voiceId", JsonValue(static_cast<int>(source->m_voiceId)));
        runtime.Set("playOnAwakeFired", JsonValue(source->m_played));
        entry.Set("runtime", std::move(runtime));
        sources.Push(std::move(entry));
    }

    JsonValue listeners = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::AudioListenerComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        if (go == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        listeners.Push(std::move(entry));
    }

    // AudioSource の busName が指せる名前の一覧。未知の名前は Master へ落ちるだけで
    // エラーにならず、「なぜか音量設定が効かない」形でしか現れない。
    JsonValue buses = JsonValue::MakeArray();
    int voiceLimit   = 0;
    int activeVoices = 0;
    if (auto* audioManager = core::Application::Get().GetAudioManager()) {
        voiceLimit   = static_cast<int>(audioManager->VoiceLimit());
        activeVoices = static_cast<int>(audioManager->ActiveVoiceCount());
        for (const audio::BusDesc& desc : audioManager->BusLayout()) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("name", JsonValue(desc.name));
            entry.Set("parent", JsonValue(desc.parent));
            entry.Set("volume", JsonValue(desc.volume));
            entry.Set("lowPassCutoff", JsonValue(desc.lowPassCutoff));
            // AudioReverbZone が効くのは reverb=true のバスへ出している音だけ。
            entry.Set("reverb", JsonValue(desc.reverb));
            buses.Push(std::move(entry));
        }
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("sourceCount", JsonValue(static_cast<int>(sources.AsArray().size())));
    result.Set("sources", std::move(sources));
    result.Set("listenerCount", JsonValue(static_cast<int>(listeners.AsArray().size())));
    result.Set("listeners", std::move(listeners));
    result.Set("buses", std::move(buses));
    // 同時発音の上限に張り付いていると、優先度の低い音から畳まれて鳴らなくなる。
    result.Set("voiceLimit", JsonValue(voiceLimit));
    result.Set("activeVoices", JsonValue(activeVoices));
    // 3D 減衰は Listener が無いと成立しない。「音が聞こえない」の最頻出原因なので明示する。
    if (listeners.AsArray().empty())
        result.Set("warning", JsonValue("AudioListener がシーンにありません (3D 音の距離減衰が効きません)"));
    return Outcome::Ok(std::move(result));
}

// ── UI ──────────────────────────────────────────────────────────────────────

// Canvas を根とする UI ツリーを、矩形と描画順が読める形で返す。
// UI は矩形で決まるので、scene_get_tree の階層だけでは「画面のどこに何が出るか」が
// 分からず、viewport_capture の絵と突き合わせる相手が無い。
JsonValue UIElementJson(GameObject& go)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue(go.instanceId));
    entry.Set("name", JsonValue(go.name));
    entry.Set("active", JsonValue(go.activeSelf()));
    entry.Set("components", SnapshotComponents(go));
    JsonValue children = JsonValue::MakeArray();
    for (int i = 0; i < go.GetChildCount(); ++i) {
        GameObject* child = go.GetChild(i);
        if (child == nullptr || child->runtimeGenerated) continue;
        children.Push(UIElementJson(*child));
    }
    entry.Set("children", std::move(children));
    return entry;
}

Outcome DoUIInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue canvases = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::UICanvas>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        if (go == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        canvases.Push(UIElementJson(*go));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(canvases.AsArray().size())));
    result.Set("canvases", std::move(canvases));
    // 編集対象として選ばれている Canvas。UIViewport の操作対象と AI の対象を一致させる。
    if (ctx.activeUICanvas.IsValid()) {
        if (GameObject* activeCanvas = activeScene->GetGameObject(ctx.activeUICanvas))
            result.Set("activeCanvasId", JsonValue(activeCanvas->instanceId));
    }
    // UI は Game View の解像度で座標が決まるので、基準の画面サイズも返す。
    JsonValue viewport = JsonValue::MakeObject();
    viewport.Set("width", JsonValue(ctx.gameViewportWidth));
    viewport.Set("height", JsonValue(ctx.gameViewportHeight));
    result.Set("gameViewport", std::move(viewport));
    return Outcome::Ok(std::move(result));
}

// ── Build (スクリプト DLL / HLSL のコンパイル状態) ───────────────────────────

// BuildConsole の履歴と診断を返す。shader_get_compile_diagnostics の Script 版。
// コンパイルが通っていないと Play も component_add も無意味な結果になるが、
// console_get_logs の断片からは何行目で落ちたのかを組み立て直す必要がある。
Outcome DoBuildStatus(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.buildConsole == nullptr) return Outcome::Err("NO_BUILD_CONSOLE", "BuildConsole が未設定です");
    const editor::BuildConsole& console = *ctx.buildConsole;

    int limit = 5;
    if (const JsonValue* v = payload.Find("limit"); v != nullptr && v->IsNumber())
        limit = std::clamp(v->AsInt(), 1, 20);

    auto recordJson = [](const editor::BuildRecord& record) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("kind", JsonValue(record.kind == editor::BuildRecord::Kind::Hlsl ? "hlsl" : "script"));
        const char* resultName = "building";
        switch (record.result) {
        case editor::BuildRecord::Result::Success:   resultName = "success"; break;
        case editor::BuildRecord::Result::Failed:    resultName = "failed"; break;
        case editor::BuildRecord::Result::Cancelled: resultName = "cancelled"; break;
        case editor::BuildRecord::Result::Building:  resultName = "building"; break;
        }
        entry.Set("result", JsonValue(resultName));
        entry.Set("exitCode", JsonValue(record.exitCode));
        entry.Set("startClock", JsonValue(record.startClock));
        entry.Set("durationSec", JsonValue(record.durationSec));
        entry.Set("errorCount", JsonValue(record.errorCount));
        entry.Set("warnCount", JsonValue(record.warnCount));
        JsonValue diagnostics = JsonValue::MakeArray();
        for (const editor::BuildDiagnostic& diagnostic : record.diagnostics) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("severity", JsonValue(
                diagnostic.severity == editor::BuildDiagnostic::Severity::Warning ? "warning" : "error"));
            item.Set("file", JsonValue(diagnostic.file));
            item.Set("line", JsonValue(diagnostic.line));
            item.Set("column", JsonValue(diagnostic.column));
            item.Set("code", JsonValue(diagnostic.code));
            item.Set("message", JsonValue(diagnostic.message));
            diagnostics.Push(std::move(item));
        }
        entry.Set("diagnostics", std::move(diagnostics));
        return entry;
    };

    JsonValue result = JsonValue::MakeObject();
    result.Set("building", JsonValue(console.IsBuilding()));
    result.Set("currentFile", JsonValue(console.CurrentFile()));
    result.Set("hasActiveFailure", JsonValue(console.HasActiveFailure()));
    // Play 開始が弾かれる理由そのもの。build_run の直後に見るのはここ。
    result.Set("scriptReloadBusy", JsonValue(ctx.scriptReloadBusy));
    if (const editor::BuildRecord* latest = console.Latest())
        result.Set("latest", recordJson(*latest));
    JsonValue history = JsonValue::MakeArray();
    const auto& records = console.History();
    for (auto it = records.rbegin(); it != records.rend() && static_cast<int>(history.AsArray().size()) < limit; ++it)
        history.Push(recordJson(*it));
    result.Set("history", std::move(history));
    return Outcome::Ok(std::move(result));
}

// スクリプト DLL の再ビルドを要求する。完了は build.status のポーリングで確認する。
// WHY 非同期のままにするか: MSBuild は数十秒かかる。ここで待つとバスの drain
//      (メインスレッド) が止まり、Editor が固まったまま応答も返らない。
Outcome DoBuildRun(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    const std::string target = LowerAscii(StringField(payload, "target"));
    if (!target.empty() && target != "script")
        return Outcome::Err("BAD_ARG", "target は script のみ対応しています (HLSL はファイル保存で自動コンパイルされます)");
    if (ctx.buildConsole != nullptr && ctx.buildConsole->IsBuilding())
        return Outcome::Err("BUILD_BUSY", "既にビルド中です");
    if (ctx.scriptReloadBusy)
        return Outcome::Err("BUILD_BUSY", "Script DLL の再読み込み中です");
    if (dryRun) return DryRunPreview("build.run");

    // 実体は script.reload operator。Play ツールバーの Reload Scripts と同じ経路を通る。
    // フラグを直に立てると、ホットリロード中のコンパイルにもう 1 本重ねられる。
    if (const editor::OpResult result = editor::InvokeOperator(ctx, "script.reload"); !result.ok)
        return Outcome::Err(result.errorCode.empty() ? "BUILD_BUSY" : result.errorCode,
                            result.message);

    JsonValue result = JsonValue::MakeObject();
    result.Set("requested", JsonValue("script"));
    // 同期完了を返せないことを明示する。待ち方を書かないと AI は即座に結果を読みに行く。
    result.Set("async", JsonValue(true));
    result.Set("poll", JsonValue("build_get_status で building=false になるまで確認してください"));
    return Outcome::Ok(std::move(result));
}

// ── 流体 (.fluid) ───────────────────────────────────────────────────────────
// レシピの読み書きは ReflectFluidRecipe 1 本に通す。キー名は .fluid (TOML) と同じなので、
// fluid.get で読んだ形をそのまま fluid.set へ返せる。焼き・プレビューは FluidBakeService の
// ジョブで、ここは受け付けて id を返すだけ (バスの drain をベイクで止めない)。

// WHY 識別子の表を別に持つか: FluidPresetName は «Fire (loop)» のような表示名で、
//     空白や括弧を含むため AI が引数として書き写す識別子には向かない。
struct FluidPresetEntry {
    asset::FluidPreset preset;
    const char*        id;
};

constexpr FluidPresetEntry kFluidPresets[] = {
    { asset::FluidPreset::Smoke,       "Smoke" },
    { asset::FluidPreset::Fire,        "Fire" },
    { asset::FluidPreset::Explosion,   "Explosion" },
    { asset::FluidPreset::Steam,       "Steam" },
    { asset::FluidPreset::DustBurst,   "DustBurst" },
    { asset::FluidPreset::Ink,         "Ink" },
    { asset::FluidPreset::MagicWisp,   "MagicWisp" },
    { asset::FluidPreset::HeatHaze,    "HeatHaze" },
    { asset::FluidPreset::WaterSplash, "WaterSplash" },
    { asset::FluidPreset::WaterJet,    "WaterJet" },
    { asset::FluidPreset::BloodBurst,  "BloodBurst" },
    { asset::FluidPreset::LavaBlob,    "LavaBlob" },
    { asset::FluidPreset::PlasmaBurst, "PlasmaBurst" },
    { asset::FluidPreset::ArcHaze,     "ArcHaze" },
    { asset::FluidPreset::GroundRing,  "GroundRing" },
    { asset::FluidPreset::ColdMist,    "ColdMist" },
    { asset::FluidPreset::ChargeVortex,"ChargeVortex" },
    { asset::FluidPreset::EmberBurst,  "EmberBurst" },
    { asset::FluidPreset::SigilFlare,  "SigilFlare" },
};

bool FindFluidPreset(const std::string& requested, asset::FluidPreset& outPreset)
{
    const std::string wanted = LowerAscii(requested);
    for (const FluidPresetEntry& entry : kFluidPresets) {
        if (wanted == LowerAscii(entry.id) || wanted == LowerAscii(asset::FluidPresetName(entry.preset))) {
            outPreset = entry.preset;
            return true;
        }
    }
    return false;
}

std::string FluidPresetIdList()
{
    std::string list;
    for (const FluidPresetEntry& entry : kFluidPresets) {
        if (!list.empty()) list += ", ";
        list += entry.id;
    }
    return list;
}

// LoadFluidRecipe / SaveFluidRecipe / FluidBakeService は UTF-8 のパス文字列を受け取る。
std::string FluidAbsolutePath(const std::filesystem::path& path)
{
    std::string text = util::FileSystem::PathToUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// FluidBakeService が返す実パスを、要求と同じ projectRoot 相対へ戻す (外なら実パスのまま)。
std::string FluidProjectRelative(const editor::EditorContext& ctx, const std::string& absolute)
{
    namespace fs = std::filesystem;
    if (absolute.empty() || ctx.projectRoot.empty()) return absolute;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    const fs::path path = fs::weakly_canonical(util::FileSystem::PathFromUtf8(absolute), ec);
    const fs::path relative = fs::relative(path, root, ec);
    if (ec || relative.empty() || relative.begin()->generic_string() == "..") return absolute;
    return relative.generic_string();
}

bool ResolveFluidPath(const editor::EditorContext& ctx, const std::string& requested,
                      std::filesystem::path& outFile, std::string& outRelative, Outcome& err)
{
    if (ctx.projectRoot.empty()) {
        err = Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
        return false;
    }
    std::error_code ec;
    if (!ResolveProjectFile(ctx, requested, outFile, outRelative)
        || std::filesystem::is_directory(outFile, ec)) {
        err = Outcome::Err("BAD_PATH", "projectRoot 配下の .fluid を指定してください: " + requested);
        return false;
    }
    if (LowerAscii(outFile.extension().string()) != ".fluid") {
        err = Outcome::Err("BAD_PATH", ".fluid のパスを指定してください: " + requested);
        return false;
    }
    return true;
}

bool LoadFluidAt(const std::filesystem::path& file, const std::string& relative,
                 asset::FluidRecipe& outRecipe, Outcome& err)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        err = Outcome::Err("FLUID_NOT_FOUND", ".fluid が見つかりません: " + relative);
        return false;
    }
    std::string reason;
    if (!asset::LoadFluidRecipe(FluidAbsolutePath(file), outRecipe, &reason)) {
        err = Outcome::Err("FLUID_READ_FAILED", ".fluid を読み取れません: " + relative
                           + (reason.empty() ? std::string{} : " (" + reason + ")"));
        return false;
    }
    return true;
}

JsonValue FluidRecipeToJson(asset::FluidRecipe recipe)
{
    JsonReadReflector reader;
    asset::ReflectFluidRecipe(recipe, reader);
    return reader.Result();
}

// JsonWriteReflector が扱わない 2 つ ── オブジェクト配列の要素数と、enum のラベル指定 ──
// をドット区切りのパスで 1 箇所だけ書く。パスの数え方は JsonWriteReflector と同じ
// (入れ子は名前、配列要素は添字) にして、同じキーが両方で同じ場所を指すようにする。
class FluidPathReflector final : public scene::IReflector {
public:
    enum class Mode { ResizeList, EnumLabel };

    FluidPathReflector(Mode mode, std::string target) : m_mode(mode), m_target(std::move(target)) {}

    void SetCount(std::size_t count) { m_count = count; }
    void SetLabel(std::string label) { m_label = std::move(label); }

    // 目標のパスがレシピに実在したか (Applied と違い、ラベル違いでも true)。
    [[nodiscard]] bool Found() const { return m_found; }
    [[nodiscard]] bool Applied() const { return m_applied; }
    [[nodiscard]] const std::string& Error() const { return m_error; }

    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

    void BeginObject(const char* name) override { m_path.emplace_back(PersistentKey(name)); }
    void EndObject() override { Leave(); }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        m_path.emplace_back(PersistentKey(name));
        if (m_mode != Mode::ResizeList || Joined() != m_target) return count;
        m_found = true;
        m_applied = true;
        return m_count;
    }
    void BeginObjectElement(std::size_t index) override { m_path.push_back(std::to_string(index)); }
    void EndObjectElement() override { Leave(); }
    std::size_t EndObjectList() override
    {
        Leave();
        return NO_REMOVE;
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (m_mode != Mode::EnumLabel || m_found) return;
        std::string path = Joined();
        if (!path.empty()) path += ".";
        path += PersistentKey(name);
        if (path != m_target) return;
        m_found = true;
        const std::string wanted = LowerAscii(m_label);
        std::string accepted;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const std::string label = labels[i] != nullptr ? labels[i] : "";
            if (LowerAscii(label) == wanted) {
                v = static_cast<int>(i);
                m_applied = true;
                return;
            }
            if (!accepted.empty()) accepted += " | ";
            accepted += label;
        }
        m_error = "'" + m_target + "' は " + accepted + " のいずれか (または添字の数値) を指定してください";
    }

private:
    std::string Joined() const
    {
        std::string joined;
        for (const std::string& segment : m_path) {
            if (!joined.empty()) joined += ".";
            joined += segment;
        }
        return joined;
    }

    void Leave()
    {
        if (!m_path.empty()) m_path.pop_back();
    }

    Mode                     m_mode;
    std::string              m_target;
    std::size_t              m_count = 0;
    std::string              m_label;
    std::vector<std::string> m_path;
    bool                     m_found = false;
    bool                     m_applied = false;
    std::string              m_error;
};

struct FluidFieldReport {
    std::vector<std::string> applied;
    std::vector<std::string> unknown;
    std::vector<std::string> errors;
    /// 上限で切り詰めた配列 ("source: 20 -> 16")。
    std::vector<std::string> clamped;
};

// 部品リストの上限。GPU の定数バッファに載る数と揃えてあり、超えて置くと CPU と GPU で絵が変わる。
std::size_t FluidListLimit(const std::string& path)
{
    if (path == "source") return static_cast<std::size_t>(asset::kMaxFluidSources);
    if (path == "force") return static_cast<std::size_t>(asset::kMaxFluidForces);
    if (path == "collider") return static_cast<std::size_t>(asset::kMaxFluidColliders);
    if (path.ends_with(".motion.key")) return static_cast<std::size_t>(asset::kMaxFluidMotionKeys);
    if (path.ends_with(".amount.key")) return static_cast<std::size_t>(asset::kMaxFluidAmountKeys);
    return (std::numeric_limits<std::size_t>::max)();
}

JsonValue FluidStringArray(const std::vector<std::string>& items)
{
    JsonValue array = JsonValue::MakeArray();
    for (const std::string& item : items) array.Push(JsonValue(item));
    return array;
}

// fields の 1 項目をレシピへ書く。オブジェクトは入れ子として潜り、オブジェクトの配列は
// «要素数をその長さにしてから各要素へ部分適用» する (配列内で省いたキーは既存値のまま)。
void ApplyFluidValue(asset::FluidRecipe& recipe, const std::string& path,
                     const JsonValue& value, FluidFieldReport& report)
{
    if (value.IsObject()) {
        for (const auto& member : value.AsObject())
            ApplyFluidValue(recipe, path.empty() ? member.first : path + "." + member.first,
                            member.second, report);
        return;
    }
    if (path.empty()) return;

    if (value.IsArray()) {
        const auto& items = value.AsArray();
        const bool objectList = std::all_of(items.begin(), items.end(),
                                            [](const JsonValue& item) { return item.IsObject(); });
        if (objectList) {
            const std::size_t count = (std::min)(items.size(), FluidListLimit(path));
            FluidPathReflector resize(FluidPathReflector::Mode::ResizeList, path);
            resize.SetCount(count);
            asset::ReflectFluidRecipe(recipe, resize);
            if (resize.Found()) {
                report.applied.push_back(path);
                // WHY 失敗にせず切り詰めるか: 上限は焼き側の都合で、AI が知らずに多めに並べることがある。
                //     全部拒否すると残りの変更まで捨てることになる。何を落としたかは clamped で返す。
                if (count < items.size())
                    report.clamped.push_back(path + ": " + std::to_string(items.size()) + " -> " + std::to_string(count));
                for (std::size_t i = 0; i < count; ++i)
                    ApplyFluidValue(recipe, path + "." + std::to_string(i), items[i], report);
                return;
            }
            // 空配列はベクトル型の誤りとして JsonWriteReflector に型エラーを言わせる。
            if (!items.empty()) {
                report.unknown.push_back(path);
                return;
            }
        }
    }

    if (value.IsString()) {
        // enum のラベルは TOML に書く名前と同じ ("smoke" / "3d")。fluid.get が添字で返した値を
        // TOML と同じ綴りでも書き戻せるようにする。
        FluidPathReflector label(FluidPathReflector::Mode::EnumLabel, path);
        label.SetLabel(value.AsString());
        asset::ReflectFluidRecipe(recipe, label);
        if (label.Found()) {
            if (label.Applied()) report.applied.push_back(path);
            else report.errors.push_back(label.Error());
            return;
        }
    }

    JsonWriteReflector writer(path, value);
    asset::ReflectFluidRecipe(recipe, writer);
    if (writer.Applied()) report.applied.push_back(path);
    else if (!writer.Error().empty()) report.errors.push_back(writer.Error());
    else report.unknown.push_back(path);
}

// 1 つでも書けないキーがあれば何も書かない。
// WHY 全部か無しか: 一部だけ書いて成功を返すと、AI は «効かなかったキー» を見落としたまま
//     焼き直しに進み、何度焼いても狙いの絵にならない。
// basePath を渡すと fields をその下 ("source.3" など) への部分指定として読む。
bool ApplyFluidFields(asset::FluidRecipe& recipe, const JsonValue& fields,
                      std::vector<std::string>& outChanged, Outcome& err,
                      std::vector<std::string>* outClamped = nullptr,
                      const std::string& basePath = std::string{})
{
    FluidFieldReport report;
    asset::FluidRecipe working = recipe;
    ApplyFluidValue(working, basePath, fields, report);

    const auto join = [](const std::vector<std::string>& items) {
        std::string text;
        for (const std::string& item : items) {
            if (!text.empty()) text += ", ";
            text += item;
        }
        return text;
    };
    if (!report.unknown.empty()) {
        err = Outcome::Err("UNKNOWN_FIELD", "未知のキー: " + join(report.unknown)
                           + " (fluid_schema で項目名を確認してください。何も書き込んでいません)");
        return false;
    }
    if (!report.errors.empty()) {
        err = Outcome::Err("TYPE_MISMATCH", join(report.errors) + " (何も書き込んでいません)");
        return false;
    }
    recipe = std::move(working);
    outChanged = std::move(report.applied);
    if (outClamped != nullptr) *outClamped = std::move(report.clamped);
    return true;
}

constexpr std::size_t kNoFluidSource = (std::numeric_limits<std::size_t>::max)();

struct FluidTextureCheck {
    std::size_t index = 0;
    /// texture そのものを今回書いたか (shape だけ変えたなら false)。
    bool        textureWritten = false;
};

// changed ("source.3.texture" / "source.3.shape") から、画像を確かめ直す発生源を拾う。
void CollectFluidTextureChecks(const std::vector<std::string>& changed, std::vector<FluidTextureCheck>& out)
{
    constexpr std::string_view kPrefix = "source.";
    for (const std::string& path : changed) {
        const std::string_view view(path);
        if (!view.starts_with(kPrefix)) continue;
        const std::size_t dot = view.find('.', kPrefix.size());
        if (dot == std::string_view::npos || dot == kPrefix.size()) continue;
        const std::string_view digits = view.substr(kPrefix.size(), dot - kPrefix.size());
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const std::string_view key = view.substr(dot + 1);
        if (key != "texture" && key != "shape") continue;
        // changed に載るのは実在した要素の添字だけなので、桁あふれは起きない (上限 kMaxFluidSources)。
        std::size_t index = 0;
        for (const char digit : digits) index = index * 10 + static_cast<std::size_t>(digit - '0');
        auto found = std::find_if(out.begin(), out.end(),
                                  [index](const FluidTextureCheck& check) { return check.index == index; });
        if (found == out.end()) found = out.insert(out.end(), FluidTextureCheck{ index, false });
        if (key == "texture") found->textureWritten = true;
    }
}

// texture 形の発生源の画像を確かめる。projectRoot の外を新しく書くのは BAD_PATH、
// 見つからない画像は書いたうえで warnings に載せる。
// WHY 無くても書くか: レシピを先に組み、画像は後から描き起こして置く運用がある。
// WHY 外を拒むのは今回書いたときだけか: 既存の .fluid が実パスを持っていても、shape の変更まで止めないため。
bool CheckFluidTextureSources(const editor::EditorContext& ctx, const asset::FluidRecipe& recipe,
                              const std::vector<std::string>& changed, std::vector<std::string>& outWarnings,
                              Outcome& err, std::size_t addedSource = kNoFluidSource)
{
    namespace fs = std::filesystem;
    std::vector<FluidTextureCheck> checks;
    CollectFluidTextureChecks(changed, checks);
    if (addedSource != kNoFluidSource
        && std::none_of(checks.begin(), checks.end(),
                        [addedSource](const FluidTextureCheck& check) { return check.index == addedSource; }))
        checks.push_back({ addedSource, false });

    for (const FluidTextureCheck& check : checks) {
        if (check.index >= recipe.sources.size()) continue;
        const asset::FluidSource& source = recipe.sources[check.index];
        if (source.shape != asset::FluidSourceShape::Texture) continue;
        const std::string key = "source." + std::to_string(check.index) + ".texture";
        if (source.texture.empty()) {
            outWarnings.push_back(key + " is empty (shape=texture は画像が無いと何も湧きません)");
            continue;
        }
        // Sprite 参照 ("<画像>::sprite::<ID>") は元画像を確かめ、コマの在処は別に見る。
        // 参照のまま存在チェックへ回すと、実在する画像まで «見つからない» になる。
        std::string imagePath;
        std::string spriteToken;
        const bool isSprite = asset::ParseSpriteReference(source.texture, imagePath, spriteToken);
        if (imagePath.starts_with("guid:")) {
            if (asset::AssetManager::ResolveAssetPath(imagePath).empty())
                outWarnings.push_back("texture not found: " + source.texture + " (" + key + ")");
            continue;
        }
        fs::path file;
        std::string relative;
        if (!ResolveProjectFile(ctx, imagePath, file, relative)) {
            if (check.textureWritten) {
                err = Outcome::Err("BAD_PATH", key + " は projectRoot 相対の画像パスで指定してください (例 \"Assets/Textures/Logo.png\"。何も書き込んでいません): "
                                   + source.texture);
                return false;
            }
            outWarnings.push_back("texture not found: " + source.texture + " (" + key + ")");
            continue;
        }
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            outWarnings.push_back("texture not found: " + relative + " (" + key + ")");
            continue;
        }
        // 切れた Sprite 参照は «アトラス全面» ではなく «読めない» になる (LoadFluidSourceMask)。
        // 黙って板の形で湧くので、書いた側に見える形で言う。
        if (!isSprite) continue;
        const std::string image = file.generic_string();
        asset::TextureImportSettings settings;
        if (!asset::GetCachedTextureImportSettings(image, settings)) {
            outWarnings.push_back("sprite meta not found: " + relative
                                  + " (" + key + " — Texture Type を Sprite にしてください)");
        } else if (asset::FindSprite(settings, spriteToken) == nullptr
                   && !IsImplicitSingleSprite(image, spriteToken)) {
            outWarnings.push_back("sprite not found: " + spriteToken + " in " + relative
                                  + " (" + key + " — sprite_list で ID を確かめてください)");
        }
    }
    return true;
}

// 書き込み前の姿。Undo はこれへ戻す (無かったなら消す)。
struct FluidFileSnapshot {
    bool        existed = false;
    std::string text;
    bool        metaExisted = false;
};

FluidFileSnapshot CaptureFluidFile(const std::filesystem::path& file)
{
    FluidFileSnapshot snapshot;
    std::error_code ec;
    snapshot.existed = std::filesystem::is_regular_file(file, ec);
    if (snapshot.existed) (void)util::FileSystem::ReadText(file, snapshot.text);
    std::filesystem::path metaFile = file;
    metaFile += ".meta";
    snapshot.metaExisted = std::filesystem::exists(metaFile, ec);
    return snapshot;
}

void RestoreFluidFile(const std::filesystem::path& file, const FluidFileSnapshot& snapshot)
{
    if (snapshot.existed) {
        (void)util::FileSystem::WriteText(file, snapshot.text);
        return;
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    // 書いた直後に Asset Browser が .meta を発行する。本体だけ消すと持ち主の無い GUID が残る。
    if (!snapshot.metaExisted) {
        std::filesystem::path metaFile = file;
        metaFile += ".meta";
        std::filesystem::remove(metaFile, ec);
    }
}

// written は Execute が実際に書けたか (nullptr 可)。
std::unique_ptr<ICommand> MakeFluidWriteCommand(editor::EditorContext& ctx, std::string label,
                                                const std::filesystem::path& file,
                                                const asset::FluidRecipe& recipe,
                                                std::shared_ptr<bool> written)
{
    const FluidFileSnapshot before = CaptureFluidFile(file);
    editor::EditorContext* context = &ctx;
    return std::make_unique<LambdaCommand>(std::move(label),
        [context, file, recipe, written]() {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const bool ok = asset::SaveFluidRecipe(FluidAbsolutePath(file), recipe);
            if (written) *written = ok;
            if (ok) context->requestAssetBrowserRefresh = true;
        },
        [context, file, before]() {
            RestoreFluidFile(file, before);
            context->requestAssetBrowserRefresh = true;
        });
}

// Undo で戻せる .fluid の書き込み。DoFluidCommand と editor.transaction (BuildCommand) の両方が通す。
bool IsFluidAssetCommand(const std::string& type)
{
    return type == "fluid.create" || type == "fluid.set" || type == "fluid.addOperator"
        || type == "fluid.removeOperator" || type == "fluid.moveOperator";
}

// 部品リスト。name は ReflectFluidRecipe の配列キー、typeField は種類を決める enum のキー。
struct FluidOperatorList {
    std::string name;
    std::string typeField;
    std::size_t limit = 0;
    /// Undo の表示名 ("AI: Add Fluid Collider")。
    const char* noun = "";
};

bool ReadFluidOperatorList(const JsonValue& payload, FluidOperatorList& out, Outcome& err)
{
    const std::string list = LowerAscii(StringField(payload, "list"));
    if (list == "source") {
        out = { list, "shape", static_cast<std::size_t>(asset::kMaxFluidSources), "Source" };
        return true;
    }
    if (list == "force") {
        out = { list, "type", static_cast<std::size_t>(asset::kMaxFluidForces), "Force" };
        return true;
    }
    if (list == "collider") {
        out = { list, "shape", static_cast<std::size_t>(asset::kMaxFluidColliders), "Collider" };
        return true;
    }
    err = Outcome::Err("BAD_ARG", "list は \"source\" (発生源) / \"force\" (力) / \"collider\" (障害物) のいずれかを指定してください: "
                       + StringField(payload, "list"));
    return false;
}

// 部品の配列は種類ごとに要素の型が違う。list の名前で 1 本選んで fn へ渡す
// (ReadFluidOperatorList を通った名前だけが来る)。
template <typename Fn>
auto WithFluidOperatorList(asset::FluidRecipe& recipe, const std::string& list, Fn&& fn)
{
    if (list == "source") return fn(recipe.sources);
    if (list == "force") return fn(recipe.forces);
    return fn(recipe.colliders);
}

// 省略なら hasValue=false で成功。数値でない・整数でない・[0, maxInclusive] の外は BAD_ARG。
bool ReadFluidOperatorIndex(const JsonValue& payload, const char* key, std::size_t maxInclusive,
                            std::size_t& out, bool& hasValue, Outcome& err)
{
    const JsonValue* value = payload.Find(key);
    hasValue = value != nullptr && !value->IsNull();
    if (!hasValue) return true;
    const bool integral = value->IsNumber() && value->AsNumber() >= 0.0
        && std::floor(value->AsNumber()) == value->AsNumber()
        && value->AsNumber() <= static_cast<double>(maxInclusive);
    if (!integral) {
        err = Outcome::Err("BAD_ARG", std::string(key) + " は 0〜" + std::to_string(maxInclusive)
                           + " の整数で指定してください");
        return false;
    }
    out = static_cast<std::size_t>(value->AsNumber());
    return true;
}

// ReflectFluidRecipe の ReflectList (Inspector の並べ替え) と同じ «取り出して差し込む» 意味にそろえる。
template <typename T>
void MoveFluidOperator(std::vector<T>& items, std::size_t from, std::size_t to)
{
    if (from == to) return;
    T moved = std::move(items[from]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
}

// fluid.addOperator / removeOperator / moveOperator。レシピを直してから fluid.set と同じ書き込みにする。
std::unique_ptr<ICommand> BuildFluidOperatorCommand(editor::EditorContext& ctx, const std::string& type,
                                                    const JsonValue& payload, const std::filesystem::path& file,
                                                    const std::string& relative, Outcome& err,
                                                    JsonValue* detailSink, std::shared_ptr<bool> written)
{
    asset::FluidRecipe recipe;
    if (!LoadFluidAt(file, relative, recipe, err)) return nullptr;
    FluidOperatorList list;
    if (!ReadFluidOperatorList(payload, list, err)) return nullptr;
    const std::size_t count = WithFluidOperatorList(recipe, list.name,
                                                    [](const auto& items) { return items.size(); });

    JsonValue detail = JsonValue::MakeObject();
    detail.Set("path", JsonValue(relative));
    detail.Set("list", JsonValue(list.name));
    std::string label;

    if (type == "fluid.addOperator") {
        if (count >= list.limit) {
            err = Outcome::Err("OPERATOR_LIMIT", list.name + " は " + std::to_string(list.limit)
                               + " 個までです (今 " + std::to_string(count) + " 個)。不要な部品を fluid_remove_operator で消してください");
            return nullptr;
        }
        std::size_t index = count;
        bool hasIndex = false;
        if (!ReadFluidOperatorIndex(payload, "index", count, index, hasIndex, err)) return nullptr;
        if (!hasIndex) index = count;
        WithFluidOperatorList(recipe, list.name, [index](auto& items) {
            using Item = typename std::decay_t<decltype(items)>::value_type;
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(index), Item{});
        });
        const std::string elementPath = list.name + "." + std::to_string(index);

        if (const JsonValue* typeValue = payload.Find("type"); typeValue != nullptr && !typeValue->IsNull()) {
            if (!typeValue->IsString() && !typeValue->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "type はラベル文字列 (fluid_schema の operators." + list.name + ".types) で指定してください");
                return nullptr;
            }
            FluidFieldReport report;
            ApplyFluidValue(recipe, elementPath + "." + list.typeField, *typeValue, report);
            if (!report.errors.empty() || report.applied.empty()) {
                err = Outcome::Err("BAD_ARG", "未知の type です"
                                   + (report.errors.empty() ? std::string{} : ": " + report.errors.front()));
                return nullptr;
            }
        }

        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        if (const JsonValue* fields = payload.Find("fields"); fields != nullptr && !fields->IsNull()) {
            if (!fields->IsObject()) {
                err = Outcome::Err("BAD_ARG", "fields はオブジェクト (新しい部品 1 つぶんの部分指定) で指定してください");
                return nullptr;
            }
            if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped, elementPath)) return nullptr;
        }
        std::vector<std::string> warnings;
        if (list.name == "source"
            && !CheckFluidTextureSources(ctx, recipe, changed, warnings, err, index)) return nullptr;
        detail.Set("index", JsonValue(static_cast<int>(index)));
        detail.Set("count", JsonValue(static_cast<int>(count + 1)));
        detail.Set("changed", FluidStringArray(changed));
        if (!clamped.empty()) detail.Set("clamped", FluidStringArray(clamped));
        if (!warnings.empty()) detail.Set("warnings", FluidStringArray(warnings));
        label = std::string("AI: Add Fluid ") + list.noun;
    } else if (type == "fluid.removeOperator") {
        if (count == 0) {
            err = Outcome::Err("BAD_ARG", list.name + " に部品がありません");
            return nullptr;
        }
        std::size_t index = 0;
        bool hasIndex = false;
        if (!ReadFluidOperatorIndex(payload, "index", count - 1, index, hasIndex, err)) return nullptr;
        if (!hasIndex) {
            err = Outcome::Err("BAD_ARG", "index (消す部品の添字) が必要です");
            return nullptr;
        }
        WithFluidOperatorList(recipe, list.name, [index](auto& items) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
        });
        detail.Set("removed", JsonValue(static_cast<int>(index)));
        detail.Set("count", JsonValue(static_cast<int>(count - 1)));
        label = std::string("AI: Remove Fluid ") + list.noun;
    } else {
        if (count == 0) {
            err = Outcome::Err("BAD_ARG", list.name + " に部品がありません");
            return nullptr;
        }
        std::size_t from = 0;
        std::size_t to = 0;
        bool hasFrom = false;
        bool hasTo = false;
        if (!ReadFluidOperatorIndex(payload, "from", count - 1, from, hasFrom, err)) return nullptr;
        if (!ReadFluidOperatorIndex(payload, "to", count - 1, to, hasTo, err)) return nullptr;
        if (!hasFrom || !hasTo) {
            err = Outcome::Err("BAD_ARG", "from と to (部品の添字) が必要です");
            return nullptr;
        }
        WithFluidOperatorList(recipe, list.name, [from, to](auto& items) { MoveFluidOperator(items, from, to); });
        detail.Set("from", JsonValue(static_cast<int>(from)));
        detail.Set("to", JsonValue(static_cast<int>(to)));
        label = std::string("AI: Move Fluid ") + list.noun;
    }

    if (detailSink != nullptr)
        for (const auto& member : detail.AsObject()) detailSink->Set(member.first, member.second);
    return MakeFluidWriteCommand(ctx, std::move(label), file, recipe, std::move(written));
}

// Fluid Editor が未保存で開いていると、ここで書いた内容は人が保存した瞬間に消える。
// 書けたことにして黙るより、消えうると言う (AI は次の書き込みを諦めるか、人に知らせられる)。
void AppendFluidEditorWarning(const std::filesystem::path& file, std::vector<std::string>& warnings)
{
    if (!editor::FluidDocument::HasUnsavedChanges(FluidAbsolutePath(file))) return;
    warnings.emplace_back("この .fluid は Fluid Editor が未保存の変更を抱えたまま開いています。"
                          "人が保存すると今の書き込みは消えます (エディター側で Reload を選んでください)");
}

// fluid.create / fluid.set / 部品の増減。Undo で戻せるファイル書き込みだけを作る。
// editor.transaction からも組めるよう BuildCommand からも呼ぶ。
std::unique_ptr<ICommand> BuildFluidAssetCommand(editor::EditorContext& ctx, const std::string& type,
                                                 const JsonValue& payload, Outcome& err,
                                                 JsonValue* detailSink, std::shared_ptr<bool> written)
{
    namespace fs = std::filesystem;
    fs::path file;
    std::string relative;
    if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return nullptr;

    if (type == "fluid.create") {
        std::string presetName = StringField(payload, "preset");
        if (presetName.empty()) presetName = "Smoke";
        asset::FluidPreset preset = asset::FluidPreset::Smoke;
        if (!FindFluidPreset(presetName, preset)) {
            err = Outcome::Err("UNKNOWN_PRESET", "未知のプリセットです: " + presetName
                               + " (" + FluidPresetIdList() + ")");
            return nullptr;
        }
        const JsonValue* overwriteValue = payload.Find("overwrite");
        const bool overwrite = overwriteValue != nullptr && overwriteValue->AsBool();
        std::error_code ec;
        const bool exists = fs::exists(file, ec);
        if (exists && !overwrite) {
            err = Outcome::Err("FLUID_EXISTS", "既にあります: " + relative + " (置き換えるなら overwrite=true)");
            return nullptr;
        }
        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("preset", JsonValue(presetName));
            detailSink->Set("overwrote", JsonValue(exists));
            std::vector<std::string> warnings;
            AppendFluidEditorWarning(file, warnings);
            if (!warnings.empty()) detailSink->Set("warnings", FluidStringArray(warnings));
        }
        return MakeFluidWriteCommand(ctx, "AI: Create Fluid", file, asset::MakeFluidPreset(preset),
                                     std::move(written));
    }

    if (type == "fluid.set") {
        asset::FluidRecipe recipe;
        if (!LoadFluidAt(file, relative, recipe, err)) return nullptr;
        const JsonValue* fields = payload.Find("fields");
        if (fields == nullptr || !fields->IsObject() || fields->AsObject().empty()) {
            err = Outcome::Err("BAD_ARG", "fields に 1 つ以上の項目を持つオブジェクトが必要です");
            return nullptr;
        }
        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped)) return nullptr;
        std::vector<std::string> warnings;
        if (!CheckFluidTextureSources(ctx, recipe, changed, warnings, err)) return nullptr;
        AppendFluidEditorWarning(file, warnings);
        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("changed", FluidStringArray(changed));
            if (!clamped.empty()) detailSink->Set("clamped", FluidStringArray(clamped));
            if (!warnings.empty()) detailSink->Set("warnings", FluidStringArray(warnings));
        }
        return MakeFluidWriteCommand(ctx, "AI: Set Fluid Fields", file, recipe, std::move(written));
    }

    if (type == "fluid.addOperator" || type == "fluid.removeOperator" || type == "fluid.moveOperator")
        return BuildFluidOperatorCommand(ctx, type, payload, file, relative, err, detailSink, std::move(written));

    err = Outcome::Err("UNKNOWN_COMMAND", "未対応の fluid コマンドです: " + type);
    return nullptr;
}

// UndoStack が無い文脈 (単体の VFX Editor 等) でもファイル操作そのものは成立させる。
void ExecuteFluidCommand(editor::EditorContext& ctx, std::unique_ptr<ICommand> command)
{
    if (ctx.undoStack != nullptr) ctx.undoStack->Execute(std::move(command));
    else command->Execute();
}

bool ReadFluidJobId(const JsonValue& payload, std::uint32_t& outId, Outcome& err)
{
    const JsonValue* value = payload.Find("job");
    if (value == nullptr || !value->IsNumber() || value->AsNumber() < 1.0
        || value->AsNumber() > 4294967295.0 || std::floor(value->AsNumber()) != value->AsNumber()) {
        err = Outcome::Err("BAD_ARG", "job (1 以上の整数) が必要です");
        return false;
    }
    outId = static_cast<std::uint32_t>(value->AsNumber());
    return true;
}

// createEffect の name はそのままファイル名になる。区切り文字を許すと dir の外へ書けてしまう。
bool IsFluidEffectName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    // Windows は末尾の空白とドットを黙って落とすため、書いた名前と実ファイル名がずれる。
    if (name.back() == '.' || name.back() == ' ') return false;
    for (const char character : name) {
        if (static_cast<unsigned char>(character) < 0x20) return false;
        if (std::string_view("\\/:*?\"<>|").find(character) != std::string_view::npos) return false;
    }
    return true;
}

const char* FluidJobStateName(editor::FluidJobState state)
{
    switch (state) {
    case editor::FluidJobState::Queued:    return "queued";
    case editor::FluidJobState::Running:   return "running";
    case editor::FluidJobState::Encoding:  return "encoding";
    case editor::FluidJobState::Done:      return "done";
    case editor::FluidJobState::Failed:    return "failed";
    case editor::FluidJobState::Cancelled: return "cancelled";
    }
    return "failed";
}

Outcome FluidServiceUnavailable()
{
    return Outcome::Err("SERVICE_UNAVAILABLE", "流体の焼きサービスがありません (エディター本体でのみ使えます)");
}

JsonValue FluidCatalog(asset::FluidRecipe recipe)
{
    JsonCatalogReflector catalog;
    asset::ReflectFluidRecipe(recipe, catalog);
    return catalog.Result();
}

const JsonValue* FindFluidCatalogField(const JsonValue* fields, std::string_view name)
{
    if (fields == nullptr || !fields->IsArray()) return nullptr;
    for (const JsonValue& field : fields->AsArray()) {
        const JsonValue* fieldName = field.Find("name");
        if (fieldName != nullptr && fieldName->IsString() && fieldName->AsString() == name) return &field;
    }
    return nullptr;
}

const JsonValue* FluidOperatorElementFields(const JsonValue& catalog, std::string_view list)
{
    const JsonValue* listField = FindFluidCatalogField(&catalog, list);
    return listField != nullptr ? listField->Find("elementFields") : nullptr;
}

// 部品 1 つぶんの «効いている» 項目名。
// WHY 反射し直して visible を読むか: JsonCatalogReflector は FieldIf で隠れた項目も落とさず
//     visible=false で載せる (保存は種類によらず全項目)。どれが効くかは種類ごとの要素を反射しないと分からない。
// 見え方は kind (気体 / 液体) でも変わりうるので、両方で見えた項目の和を返す。
JsonValue VisibleFluidOperatorFields(asset::FluidRecipe recipe, std::string_view list)
{
    std::vector<std::string> names;
    for (const asset::FluidKind kind : { asset::FluidKind::Gas, asset::FluidKind::Liquid }) {
        recipe.kind = kind;
        const JsonValue catalog = FluidCatalog(recipe);
        const JsonValue* elementFields = FluidOperatorElementFields(catalog, list);
        if (elementFields == nullptr || !elementFields->IsArray()) continue;
        for (const JsonValue& field : elementFields->AsArray()) {
            const JsonValue* name = field.Find("name");
            const JsonValue* visible = field.Find("visible");
            if (name == nullptr || !name->IsString()) continue;
            if (visible != nullptr && visible->IsBool() && !visible->AsBool()) continue;
            if (std::find(names.begin(), names.end(), name->AsString()) == names.end())
                names.push_back(name->AsString());
        }
    }
    return FluidStringArray(names);
}

// 種類の一覧は enum のラベル (= TOML の綴り) から採る。makeElement(recipe, typeIndex) は
// その種類の要素を 1 つだけ持つレシピへ整える。
template <typename MakeElement>
JsonValue FluidOperatorSchema(const JsonValue& baseCatalog, std::string_view list, std::string_view typeField,
                              int limit, MakeElement makeElement)
{
    JsonValue types = JsonValue::MakeArray();
    JsonValue fields = JsonValue::MakeObject();
    const JsonValue* typeDescriptor = FindFluidCatalogField(FluidOperatorElementFields(baseCatalog, list), typeField);
    const JsonValue* labels = typeDescriptor != nullptr ? typeDescriptor->Find("enumLabels") : nullptr;
    if (labels != nullptr && labels->IsArray()) {
        int typeIndex = 0;
        for (const JsonValue& label : labels->AsArray()) {
            asset::FluidRecipe recipe;
            makeElement(recipe, typeIndex++);
            if (!label.IsString()) continue;
            types.Push(label);
            fields.Set(label.AsString(), VisibleFluidOperatorFields(std::move(recipe), list));
        }
    }
    JsonValue section = JsonValue::MakeObject();
    section.Set("typeField", JsonValue(std::string(typeField)));
    section.Set("limit", JsonValue(limit));
    section.Set("types", std::move(types));
    section.Set("fields", std::move(fields));
    return section;
}

Outcome DoFluidSchema()
{
    asset::FluidRecipe recipe;
    // 配列のスキーマは先頭要素から採る (JsonCatalogReflector)。空のままだと要素の項目が 1 つも出ない。
    // motion.key / amount.key も配列なので、部品ごとに 1 キーを持たせる (障害物に amount は無い)。
    recipe.sources.emplace_back();
    recipe.sources.back().motion.keys.emplace_back();
    recipe.sources.back().amount.keys.emplace_back();
    recipe.forces.emplace_back();
    recipe.forces.back().motion.keys.emplace_back();
    recipe.forces.back().amount.keys.emplace_back();
    recipe.colliders.emplace_back();
    recipe.colliders.back().motion.keys.emplace_back();
    const JsonValue catalog = FluidCatalog(recipe);

    JsonValue operators = JsonValue::MakeObject();
    operators.Set("source", FluidOperatorSchema(catalog, "source", "shape", asset::kMaxFluidSources,
        [](asset::FluidRecipe& target, int typeIndex) {
            target.sources.emplace_back();
            target.sources.back().shape = static_cast<asset::FluidSourceShape>(typeIndex);
        }));
    operators.Set("force", FluidOperatorSchema(catalog, "force", "type", asset::kMaxFluidForces,
        [](asset::FluidRecipe& target, int typeIndex) {
            target.forces.emplace_back();
            target.forces.back().type = static_cast<asset::FluidForceType>(typeIndex);
        }));
    operators.Set("collider", FluidOperatorSchema(catalog, "collider", "shape", asset::kMaxFluidColliders,
        [](asset::FluidRecipe& target, int typeIndex) {
            target.colliders.emplace_back();
            target.colliders.back().shape = static_cast<asset::FluidColliderShape>(typeIndex);
        }));
    JsonValue limits = JsonValue::MakeObject();
    limits.Set("source", JsonValue(asset::kMaxFluidSources));
    limits.Set("force", JsonValue(asset::kMaxFluidForces));
    limits.Set("collider", JsonValue(asset::kMaxFluidColliders));
    limits.Set("motionKey", JsonValue(asset::kMaxFluidMotionKeys));

    JsonValue presets = JsonValue::MakeArray();
    JsonValue presetLabels = JsonValue::MakeArray();
    for (const FluidPresetEntry& entry : kFluidPresets) {
        presets.Push(JsonValue(entry.id));
        presetLabels.Push(JsonValue(asset::FluidPresetName(entry.preset)));
    }
    JsonValue bakeModes = JsonValue::MakeArray();
    bakeModes.Push(JsonValue("2d"));
    bakeModes.Push(JsonValue("3d"));

    JsonValue result = JsonValue::MakeObject();
    result.Set("fields", catalog);
    result.Set("operators", std::move(operators));
    result.Set("limits", std::move(limits));
    result.Set("presets", std::move(presets));
    result.Set("presetLabels", std::move(presetLabels));
    result.Set("bakeModes", std::move(bakeModes));
    result.Set("hint", JsonValue(std::string(
        "流体は部品の組み合わせで作ります: source (発生源。気体は密度・温度・燃料を注ぎ、液体は粒子を撃ち出す。最大 16)、"
        "force (流れにかかる力。最大 8)、collider (障害物。流体が入り込めない形。最大 8) の 3 つのリスト。"
        "部品は fluid_add_operator で足し (type に形 / 力の種類のラベル)、fluid_remove_operator / fluid_move_operator で消す・並べ替えます。"
        "種類ごとに効く項目は operators.<source|force|collider>.fields.<種類> です (他の項目も保存はされますが、その種類では効きません)。"
        "fields の source / force / collider は既定の種類 (sphere / wind / sphere) で採った目録で、visible もその種類のものです。"
        "部品の motion.key は {time, offset} の配列 (最大 8、time の昇順) で、部品の中心を時間で動かします "
        "(motion.inherit_velocity で動きの速さを流れに足す)。"
        "collider の shape は sphere (size.x = 半径) / box (size = 各軸の半分。回転なし) / plane (center を通り direction を法線とする面。"
        "法線の反対側がすべて固体で、壁や斜めの床になる) / capsule (center を通り direction を軸とする線分に肉を付けた形。"
        "size.x = 半径、size.y = 芯の半分の長さで両端は半球。腕・脚・棒・パイプ) / cylinder (同じ size で両端が平らな柱)。"
        "動く collider (motion.key) は流体を押しのけます。"
        "friction は液体が表面を滑るときの減速、start_time / duration で居る時間を区切れます。"
        "床は collider ではなく今までどおり gas.floor / liquid.floor で別に持ちます。"
        "source の shape \"texture\" は画像の形に湧きます (文字・ロゴ・魔法陣): direction を法線とする板に texture "
        "(projectRoot 相対の画像パス。例 \"Assets/Textures/Logo.png\") を貼り、白く不透明なところほど強く注ぎます (輝度 × α)。"
        "size.x / size.y が板の半幅 / 半高さ、size.z が厚みの半分。画像が見つからなくても書き込みは通り、応答の warnings に載ります。"
        "source の shape \"capsule\" は direction を軸とする線分に肉を付けた形 (size.x = 半径、size.y = 芯の半分の長さ。両端は半球。"
        "腕・脚・棒から湧く煙)、\"cylinder\" は両端が平らな柱 (煙突・通気口から柱状に立ち上る煙。cone は広がり、ring は輪になる)。"
        "fluid_set の fields は fluid_get の recipe と同じ形です (部分指定可・省いたキーは既存値のまま)。"
        "入れ子は {\"gas\":{\"buoyancy\":2}} でも \"gas.buoyancy\" でも書けます。"
        "オブジェクト配列は配列ごと渡すと要素数がその長さになり (上限を超えた分は切り詰めて clamped で返す)、"
        "1 要素だけなら \"source.0.density\"。"
        "enum は添字の数値かラベル文字列 (大文字小文字は問わない。例 \"source.0.shape\": \"cone\" / \"force.0.type\": \"vortex\" / "
        "\"collider.0.shape\": \"plane\")、"
        "焼き方は \"bake.mode\": \"2d\" | \"3d\"。"
        "発生源ごとに色を変えるには render.use_albedo_ramp=true にし、render.albedo_ramp (4 点固定の "
        "{color: リニア RGB, position: 0〜1 の昇順} の配列) に色を並べて、各 source の color_key (0〜1) でどの色かを選びます "
        "(例 \"source.1.color_key\": 1)。気体は色が煙に乗って運ばれ、複数の発生源の煙が混ざると色も混ざります。"
        "液体は粒子ごとに撃ち出した発生源の色を持ちます (水と血を 1 枚に焼き分けられる)。"
        "use_albedo_ramp が false なら color_key は効かず、smoke_color / liquid_color の 1 色です。"
        "3d でもループ (output.loop) と歪み (render.shading=\"distortion\") を焼け、全プリセットが 3d で焼けます。"
        "bake.solver は \"auto\" (既定。GPU で解き、使えなければ CPU へ落ちる) / \"gpu\" (落とさず失敗させる。"
        "同じ .fluid から必ず同じ絵が欲しいとき) / \"cpu\" (常に CPU・96³ まで) の 3 つです。"
        "どれで解いたかは fluid_job_status の solverUsed に出ます。"
        "焼きとプレビューは同じ経路で解くので、fluid_preview の frame と fluid_bake のコマは同じ絵です。"
        "絵が変わったかどうかは fingerprint (16 桁) の一致で判定できます。")));
    return Outcome::Ok(std::move(result));
}

Outcome DoFluidGet(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    Outcome err;
    if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return err;
    asset::FluidRecipe recipe;
    if (!LoadFluidAt(file, relative, recipe, err)) return err;

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("bakeMode", JsonValue(recipe.bake.mode == asset::FluidBakeMode::Volume3D ? "3d" : "2d"));
    result.Set("recipe", FluidRecipeToJson(recipe));
    return Outcome::Ok(std::move(result));
}

Outcome DoFluidJobStatus(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::uint32_t id = 0;
    Outcome err;
    if (!ReadFluidJobId(payload, id, err)) return err;
    // サービスが無ければジョブも存在し得ない。区別すると AI が «待てば出る» と誤読する。
    const editor::FluidJobStatus* status = ctx.fluidBake != nullptr ? ctx.fluidBake->Find(id) : nullptr;
    if (status == nullptr)
        return Outcome::Err("FLUID_JOB_NOT_FOUND", "ジョブが見つかりません: " + std::to_string(id)
                            + " (終わったジョブは直近 32 件だけ残ります)");

    JsonValue outputs = JsonValue::MakeArray();
    for (const std::string& output : status->outputs) outputs.Push(JsonValue(FluidProjectRelative(ctx, output)));

    JsonValue result = JsonValue::MakeObject();
    result.Set("job", JsonValue(static_cast<std::int64_t>(status->id)));
    result.Set("kind", JsonValue(status->kind == editor::FluidJobKind::Preview ? "preview" : "bake"));
    result.Set("state", JsonValue(FluidJobStateName(status->state)));
    result.Set("finished", JsonValue(status->Finished()));
    result.Set("progress", JsonValue(static_cast<double>(status->progress)));
    result.Set("fluidPath", JsonValue(FluidProjectRelative(ctx, status->fluidPath)));
    result.Set("message", JsonValue(status->message));
    result.Set("outputs", std::move(outputs));
    result.Set("materialPath", JsonValue(FluidProjectRelative(ctx, status->materialPath)));
    result.Set("vfxPath", JsonValue(FluidProjectRelative(ctx, status->vfxPath)));
    result.Set("previewPngPath", JsonValue(FluidProjectRelative(ctx, status->previewPngPath)));
    // 指紋は «前と同じ絵か» を画像を見比べずに決めるための値。ソルバーとフォールバックの理由も返し、
    // «同じレシピなのに絵が違う» の原因 (GPU が使えず CPU で解かれた) を AI 側で切り分けられるようにする。
    if (!status->fingerprint.empty()) result.Set("fingerprint", JsonValue(status->fingerprint));
    if (!status->solverUsed.empty()) result.Set("solverUsed", JsonValue(status->solverUsed));
    if (!status->fallbackReason.empty()) result.Set("fallbackReason", JsonValue(status->fallbackReason));
    result.Set("seed", JsonValue(static_cast<std::int64_t>(status->seed)));
    if (status->kind == editor::FluidJobKind::Preview)
        result.Set("frame", JsonValue(static_cast<std::int64_t>(status->previewFrame)));

    const JsonValue* includeValue = payload.Find("includeImage");
    const bool includeImage = includeValue == nullptr || includeValue->AsBool(true);
    if (includeImage && status->kind == editor::FluidJobKind::Preview
        && status->state == editor::FluidJobState::Done && !status->previewPngPath.empty()) {
        std::vector<std::uint8_t> bytes;
        if (util::FileSystem::ReadBinary(util::FileSystem::PathFromUtf8(status->previewPngPath), bytes)
            && !bytes.empty() && bytes.size() <= 16u * 1024u * 1024u) {
            // asset.thumbnail と同じ形。MCP 側はこれをそのまま image content へ移す。
            JsonValue image = JsonValue::MakeObject();
            image.Set("mimeType", JsonValue("image/png"));
            image.Set("base64", JsonValue(Base64Encode(bytes)));
            image.Set("path", JsonValue(FluidProjectRelative(ctx, status->previewPngPath)));
            result.Set("image", std::move(image));
        }
    }
    return Outcome::Ok(std::move(result));
}

// fluid.* の Command。create / set は Undo 可能、焼き系 (preview / bake / cancel と
// createEffect の焼き部分) は build.run と同じくジョブを受け付けるだけで Undo に載せない。
Outcome DoFluidCommand(editor::EditorContext& ctx, const std::string& type, const JsonValue& payload, bool dryRun)
{
    namespace fs = std::filesystem;
    const char* kPoll = "fluid_job_status";

    if (IsFluidAssetCommand(type)) {
        Outcome err;
        JsonValue detail = JsonValue::MakeObject();
        auto written = std::make_shared<bool>(false);
        std::unique_ptr<ICommand> command = BuildFluidAssetCommand(ctx, type, payload, err, &detail, written);
        if (command == nullptr) return err;
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("detail", std::move(detail));
            return preview;
        }
        ExecuteFluidCommand(ctx, std::move(command));
        if (!*written) return Outcome::Err("FLUID_WRITE_FAILED", ".fluid を書き込めません: " + StringField(payload, "path"));
        return Outcome::Ok(std::move(detail));
    }

    if (type == "fluid.preview" || type == "fluid.bake") {
        if (ctx.fluidBake == nullptr) return FluidServiceUnavailable();
        fs::path file;
        std::string relative;
        Outcome err;
        if (!ResolveFluidPath(ctx, StringField(payload, "path"), file, relative, err)) return err;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return Outcome::Err("FLUID_NOT_FOUND", ".fluid が見つかりません: " + relative);

        const JsonValue* updateValue = payload.Find("updateMaterial");
        const bool updateMaterial = updateValue == nullptr || updateValue->AsBool(true);
        fs::path materialRelative(relative);
        materialRelative.replace_extension(".mat");
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("path", JsonValue(relative));
            return preview;
        }

        editor::FluidJobError jobError;
        std::uint32_t id = 0;
        if (type == "fluid.preview") {
            editor::FluidPreviewRequest request;
            request.fluidPath = FluidAbsolutePath(file);
            // コマ番号が第一級。秒しか来なければ一番近いコマへ吸着させる (どのコマでもない絵を作らない)。
            if (const JsonValue* frame = payload.Find("frame"); frame != nullptr && frame->IsNumber()) {
                request.frame = std::clamp(frame->AsInt(), 0, 1023);
            } else if (const JsonValue* time = payload.Find("time"); time != nullptr && time->IsNumber()) {
                request.frame = -1;
                request.time = std::clamp(static_cast<float>(time->AsNumber()), 0.0f, 600.0f);
            }
            if (const JsonValue* size = payload.Find("size"); size != nullptr && size->IsNumber())
                request.size = std::clamp(size->AsInt(), 32, 2048);
            if (const JsonValue* sheet = payload.Find("contactSheet"); sheet != nullptr)
                request.contactSheet = sheet->AsBool(false);
            if (const JsonValue* variants = payload.Find("variants"); variants != nullptr && variants->IsNumber())
                request.variants = std::clamp(variants->AsInt(), 1, 16);
            if (const JsonValue* seed = payload.Find("seed"); seed != nullptr && seed->IsNumber())
                request.seed = static_cast<std::uint32_t>(std::clamp(seed->AsNumber(), 0.0, 4294967295.0));
            if (request.variants > 1) request.contactSheet = true;
            id = ctx.fluidBake->EnqueuePreview(ctx, request, jobError);
        } else {
            editor::FluidBakeRequest request;
            request.fluidPath = FluidAbsolutePath(file);
            request.updateMaterial = updateMaterial;
            if (const JsonValue* seed = payload.Find("seed"); seed != nullptr && seed->IsNumber())
                request.seed = static_cast<std::uint32_t>(std::clamp(seed->AsNumber(), 0.0, 4294967295.0));
            id = ctx.fluidBake->EnqueueBake(ctx, request, jobError);
        }
        if (id == 0)
            return Outcome::Err(jobError.code.empty() ? "FLUID_BAKE_FAILED" : jobError.code, jobError.message);

        JsonValue result = JsonValue::MakeObject();
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("async", JsonValue(true));
        result.Set("poll", JsonValue(kPoll));
        result.Set("path", JsonValue(relative));
        if (type == "fluid.bake" && updateMaterial)
            result.Set("materialPath", JsonValue(materialRelative.generic_string()));
        return Outcome::Ok(std::move(result));
    }

    if (type == "fluid.cancel") {
        std::uint32_t id = 0;
        Outcome err;
        if (!ReadFluidJobId(payload, id, err)) return err;
        if (ctx.fluidBake == nullptr) return FluidServiceUnavailable();
        if (dryRun) return DryRunPreview(type);
        JsonValue result = JsonValue::MakeObject();
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("cancelled", JsonValue(ctx.fluidBake->Cancel(id)));
        return Outcome::Ok(std::move(result));
    }

    if (type == "fluid.createEffect") {
        const std::string name = StringField(payload, "name");
        if (!IsFluidEffectName(name))
            return Outcome::Err("BAD_ARG", "name はファイル名 1 つ分 (64 文字以内・区切り文字と先頭の . は不可) で指定してください");
        std::string dir = StringField(payload, "dir");
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        if (dir.empty()) dir = "Assets/VFX/Fluid";

        fs::path file;
        std::string relative;
        Outcome err;
        if (!ResolveFluidPath(ctx, dir + "/" + name + ".fluid", file, relative, err)) return err;
        std::error_code ec;
        if (fs::exists(file, ec)) return Outcome::Err("FLUID_EXISTS", "既にあります: " + relative);

        std::string presetName = StringField(payload, "preset");
        if (presetName.empty()) presetName = "Smoke";
        asset::FluidPreset preset = asset::FluidPreset::Smoke;
        if (!FindFluidPreset(presetName, preset))
            return Outcome::Err("UNKNOWN_PRESET", "未知のプリセットです: " + presetName + " (" + FluidPresetIdList() + ")");
        asset::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        std::vector<std::string> changed;
        std::vector<std::string> clamped;
        std::vector<std::string> warnings;
        if (const JsonValue* fields = payload.Find("fields"); fields != nullptr) {
            if (!fields->IsObject()) return Outcome::Err("BAD_ARG", "fields はオブジェクトで指定してください");
            if (!ApplyFluidFields(recipe, *fields, changed, err, &clamped)) return err;
            if (!CheckFluidTextureSources(ctx, recipe, changed, warnings, err)) return err;
        }

        const JsonValue* bakeValue = payload.Find("bake");
        const bool bake = bakeValue == nullptr || bakeValue->AsBool(true);
        // 焼けないと分かっているのに .fluid だけ書くと、成功に見えて何も出ない。書く前に止める。
        if (bake && ctx.fluidBake == nullptr) return FluidServiceUnavailable();

        fs::path vfxFile = file;
        vfxFile.replace_extension(".vfx");
        fs::path vfxRelative(relative);
        vfxRelative.replace_extension(".vfx");
        fs::path materialRelative(relative);
        materialRelative.replace_extension(".mat");

        JsonValue result = JsonValue::MakeObject();
        result.Set("fluidPath", JsonValue(relative));
        result.Set("preset", JsonValue(presetName));
        result.Set("changed", FluidStringArray(changed));
        if (!clamped.empty()) result.Set("clamped", FluidStringArray(clamped));
        if (!warnings.empty()) result.Set("warnings", FluidStringArray(warnings));
        if (bake) {
            result.Set("materialPath", JsonValue(materialRelative.generic_string()));
            result.Set("vfxPath", JsonValue(vfxRelative.generic_string()));
        }
        if (dryRun) {
            Outcome preview = DryRunPreview(type);
            preview.result.Set("detail", std::move(result));
            return preview;
        }

        auto written = std::make_shared<bool>(false);
        ExecuteFluidCommand(ctx, MakeFluidWriteCommand(ctx, "AI: Create Fluid Effect", file, recipe, written));
        if (!*written) return Outcome::Err("FLUID_WRITE_FAILED", ".fluid を書き込めません: " + relative);
        if (!bake) return Outcome::Ok(std::move(result));

        editor::FluidBakeRequest request;
        request.fluidPath = FluidAbsolutePath(file);
        request.updateMaterial = true;
        request.vfxPath = FluidAbsolutePath(vfxFile);
        request.vfxRootName = name;
        editor::FluidJobError jobError;
        const std::uint32_t id = ctx.fluidBake->EnqueueBake(ctx, request, jobError);
        if (id == 0) {
            // .fluid は書けているので失敗にはしない。焼きだけを fluid_bake でやり直せる。
            JsonValue bakeError = JsonValue::MakeObject();
            bakeError.Set("code", JsonValue(jobError.code.empty() ? std::string("FLUID_BAKE_FAILED") : jobError.code));
            bakeError.Set("message", JsonValue(jobError.message));
            result.Set("bakeError", std::move(bakeError));
            return Outcome::Ok(std::move(result));
        }
        result.Set("job", JsonValue(static_cast<std::int64_t>(id)));
        result.Set("async", JsonValue(true));
        result.Set("poll", JsonValue(kPoll));
        return Outcome::Ok(std::move(result));
    }

    return Outcome::Err("UNKNOWN_COMMAND", "未対応の fluid コマンドです: " + type);
}

std::unique_ptr<ICommand> BuildCommand(editor::EditorContext& ctx, const std::string& type,
                                       const JsonValue& payload, Outcome& err,
                                       std::shared_ptr<std::string> createdSink,
                                       JsonValue* detailSink = nullptr)
{
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
        if (const JsonValue* rowValue = payload.Find("rowSequences"); rowValue != nullptr)
            settings.rowSequences = rowValue->AsBool();

        // 任意: 生成した MV と推奨 strength を .mat へ書き込む。
        // WHY: strength は生成結果 (最大移動量) でしか決まらず、AI が別途設定する手段が無い。
        //      生成は dryRun でもファイルを書かないよう Lambda 内で行うため、適用も同じ場所でやる。
        fs::path materialFile;
        std::string materialPath;
        asset::MaterialAsset oldMaterial;
        const bool applyToMaterial = !StringField(payload, "materialPath").empty();
        if (applyToMaterial) {
            if (!ResolveProjectFile(ctx, StringField(payload, "materialPath"), materialFile, materialPath)
                || LowerAscii(materialFile.extension().string()) != ".mat"
                || !fs::is_regular_file(materialFile)) {
                err = Outcome::Err("MATERIAL_NOT_FOUND", "projectRoot 配下の .mat を指定してください");
                return nullptr;
            }
            if (!asset::LoadMaterialAssetFromFile(materialFile.generic_string(), oldMaterial)) {
                err = Outcome::Err("MATERIAL_READ_FAILED", "MaterialAsset を読み取れません: " + materialPath);
                return nullptr;
            }
        }
        fs::path motionRelative(texturePath);
        motionRelative.replace_extension();
        motionRelative += "_mv.png";

        // MV の生成は新規ファイルを足すだけなので、Undo で消さない。
        // WHY: 同名の MV が既にあった場合、Undo で消すとユーザーが手で用意した
        //      アトラスを破壊しうる。生成物の削除は AssetBrowser から明示的に行わせる。
        //      .mat を書き換えた場合だけ、その書き換えを元に戻す。
        editor::EditorContext* context = &ctx;
        const std::string resolved = textureFile.generic_string();
        const std::string motionPath = motionRelative.generic_string();
        return std::make_unique<LambdaCommand>("AI: Generate Motion Vectors",
            [context, resolved, settings, applyToMaterial, materialFile, oldMaterial, motionPath]() {
                const auto result = asset::GenerateFlipbookMotionVectors(resolved, settings);
                if (!result.success) return;
                context->requestAssetBrowserRefresh = true;
                if (!applyToMaterial) return;
                asset::MaterialAsset updated = oldMaterial;
                updated.textures["tex5"] = motionPath;
                updated.particle.flipbook.motionVectorFlipbook = true;
                updated.particle.flipbook.motionVectorStrength = result.recommendedStrength;
                updated.particle.flipbook.flipbookFrameBlending = true;
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), updated))
                    asset::AssetManager::ReloadPath(materialFile.generic_string());
            },
            [context, applyToMaterial, materialFile, oldMaterial]() {
                if (!applyToMaterial) return;
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), oldMaterial))
                    asset::AssetManager::ReloadPath(materialFile.generic_string());
                context->requestAssetBrowserRefresh = true;
            });
    }
    // .fluid の作成・編集 (部品の増減を含む) もファイル単体で完結する (Scene 不要)。
    if (IsFluidAssetCommand(type))
        return BuildFluidAssetCommand(ctx, type, payload, err, detailSink, nullptr);
    // 焼き系はジョブを積むだけで Undo できない。transaction に混ぜると «まとめて戻したのに
    // 焼きだけ走り続ける» ことになるので、Scene 不在の NO_SCENE より先に理由付きで断る。
    if (type.starts_with("fluid.")) {
        err = Outcome::Err("UNSUPPORTED",
            "この fluid コマンドは transaction 内で使用できません (焼き・プレビューは Undo できないため単独で呼んでください): " + type);
        return nullptr;
    }
    // Sprite の編集は .meta の中だけで完結する。Scene 不要 (Sprite Editor と同じ)。
    if (type.starts_with("sprite.")) return BuildSpriteCommand(ctx, type, payload, err, detailSink);
    // .behaviortree の編集も Scene を必要としない (アセット単体で完結する)。
    if (type.starts_with("bt.")) return BuildBehaviorTreeCommand(ctx, type, payload, err, detailSink);
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    // ── Add Object プリセット ──────────────────────────────────────────────
    // Hierarchy の Add Object と同じ生成関数を通す。作られるものが人の操作と完全に一致する。
    if (type == "preset.create") {
        const std::string presetId = StringField(payload, "preset");
        const editor::ObjectPreset* preset = editor::FindObjectPreset(presetId);
        if (preset == nullptr) {
            err = Outcome::Err("UNKNOWN_PRESET",
                "未知のプリセットです: " + presetId + " (preset_catalog で一覧を確認してください)");
            return nullptr;
        }
        const std::string parentId = StringField(payload, "parent");
        if (!parentId.empty() && scene->FindByGuid(parentId) == nullptr) {
            err = Outcome::Err("NODE_NOT_FOUND", "parent が見つかりません: " + parentId);
            return nullptr;
        }
        const std::string requestedName = StringField(payload, "name");
        math::Vector3 position;
        const bool hasPosition = ReadVec3(payload, "position", position);

        if (detailSink != nullptr) {
            detailSink->Set("preset", JsonValue(std::string(preset->id)));
            detailSink->Set("label", JsonValue(std::string(preset->label)));
            detailSink->Set("contents", JsonValue(std::string(preset->description)));
        }

        // node.create と同じ方式: 生成した NodeId を shared_ptr へ記録し、Undo はそれを消す。
        auto guid = std::make_shared<std::string>();
        auto* contextPtr = &ctx;
        return std::make_unique<LambdaCommand>("AI: Create " + std::string(preset->label),
            [contextPtr, scene, presetId, parentId, requestedName, hasPosition, position, guid, createdSink, markDirty]() {
                scene::EntityID parentEntity{};
                if (!parentId.empty()) {
                    if (GameObject* parentObject = scene->FindByGuid(parentId)) parentEntity = parentObject->GetID();
                }
                GameObject* created = editor::CreateObjectFromPreset(*contextPtr, presetId, parentEntity);
                if (created == nullptr) return;
                if (!requestedName.empty()) created->name = requestedName;
                // position はローカル座標。親付けした場合は親からの相対になる (Transform と同じ規則)。
                if (hasPosition) created->transform.position = position;
                *guid = created->instanceId;
                if (createdSink) *createdSink = created->instanceId;
                markDirty();
            },
            [scene, guid, markDirty]() {
                if (!guid->empty()) {
                    if (GameObject* go = scene->FindByGuid(*guid)) scene->DestroyGameObject(go->GetID());
                }
                markDirty();
            });
    }

    // ── Terrain: ブラシ操作 ─────────────────────────────────────────────────
    // マウスドラッグを持たない AI のために、ストローク 1 回ぶんを 1 コマンドとして受ける。
    // iterations は「押し続けた回数」。Smooth / Flatten は 1 回では収束しない。
    if (type == "terrain.sculpt" || type == "terrain.paint") {
        math::Vector3 center;
        if (!ReadVec3(payload, "position", center)) {
            err = Outcome::Err("BAD_ARG", "position ([x,y,z] ワールド座標) が必要です"); return nullptr;
        }
        TerrainBrush brush;
        std::string brushError;
        if (!ReadTerrainBrush(payload, brush, brushError)) { err = Outcome::Err("BAD_ARG", brushError); return nullptr; }
        int iterations = 1;
        if (const JsonValue* v = payload.Find("iterations"); v != nullptr && v->IsNumber())
            iterations = std::clamp(v->AsInt(), 1, 64);

        const std::string restrictTo = StringField(payload, "id");
        std::vector<TerrainHit> hits = CollectTerrainsUnderBrush(*scene, center, brush.radius, restrictTo);
        if (hits.empty()) {
            err = Outcome::Err("NO_TERRAIN", restrictTo.empty()
                ? "その位置に重なる TerrainComponent がありません"
                : "指定ノードの Terrain はブラシ範囲と重なりません: " + restrictTo);
            return nullptr;
        }

        const bool isSculpt = (type == "terrain.sculpt");
        TerrainSculptOp sculptOp = TerrainSculptOp::Raise;
        float flattenTarget = 0.0f;
        int paintLayer = 0;
        if (isSculpt) {
            const std::string op = LowerAscii(StringField(payload, "op"));
            if (op.empty() || op == "raise")   sculptOp = TerrainSculptOp::Raise;
            else if (op == "lower")            sculptOp = TerrainSculptOp::Lower;
            else if (op == "smooth")           sculptOp = TerrainSculptOp::Smooth;
            else if (op == "flatten")          sculptOp = TerrainSculptOp::Flatten;
            else if (op == "stamp")            sculptOp = TerrainSculptOp::Stamp;
            else { err = Outcome::Err("BAD_ARG", "op は raise/lower/smooth/flatten/stamp です"); return nullptr; }
            if (sculptOp == TerrainSculptOp::Flatten) {
                // 対話ツールは「最初にクリックした高さ」を基準にする。AI にはクリックが無いので、
                // 明示指定が無ければブラシ中心の現在高さを基準にする (同じ意味論になる)。
                if (const JsonValue* v = payload.Find("targetHeight"); v != nullptr && v->IsNumber()) {
                    flattenTarget = static_cast<float>(v->AsNumber());
                } else {
                    const TerrainHit& primary = hits.front();
                    flattenTarget = primary.terrain->GetHeightAt(primary.local.x, primary.local.z);
                }
            }
        } else {
            const JsonValue* layerValue = payload.Find("layer");
            if (layerValue == nullptr || !layerValue->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "layer (0〜3) が必要です"); return nullptr;
            }
            paintLayer = layerValue->AsInt();
            if (paintLayer < 0 || paintLayer > 3) { err = Outcome::Err("BAD_ARG", "layer は 0〜3 です"); return nullptr; }
        }

        // Paint は Terrain ごとに塗る層を解決する。隣接 Terrain は layerMaterials の並びが
        // 異なり得るため、同じ index を塗ると別マテリアルへ塗ってしまう (TerrainTool と同じ規則)。
        const std::string sourceMaterial = isSculpt ? std::string{}
            : hits.front().terrain->layerMaterials[static_cast<size_t>(paintLayer)];

        // before/after を先に作り、コマンドは「スナップショットの入れ替え」だけを行う。
        std::vector<TerrainSnapshot> before;
        std::vector<TerrainSnapshot> after;
        std::vector<std::string> touched;
        before.reserve(hits.size());
        after.reserve(hits.size());
        for (const TerrainHit& hit : hits) {
            int layerForThisTerrain = paintLayer;
            if (!isSculpt && hit.terrain != hits.front().terrain) {
                layerForThisTerrain = ResolvePaintLayerForTerrain(*hit.terrain, sourceMaterial, paintLayer);
                if (layerForThisTerrain < 0) continue; // 対応する層が無い Terrain には塗らない
            }
            before.push_back({ hit.go->instanceId, *hit.terrain });
            scene::TerrainComponent edited = *hit.terrain;
            for (int i = 0; i < iterations; ++i) {
                if (isSculpt) ApplyTerrainSculpt(edited, hit.local, brush, sculptOp, flattenTarget, 1.0f);
                else          ApplyTerrainPaint(edited, hit.local, brush, layerForThisTerrain, 1.0f);
            }
            after.push_back({ hit.go->instanceId, std::move(edited) });
            touched.push_back(hit.go->instanceId);
        }
        if (touched.empty()) {
            err = Outcome::Err("NO_TERRAIN", "塗る対象のレイヤーを持つ Terrain がありません");
            return nullptr;
        }

        if (detailSink != nullptr) {
            JsonValue terrainIds = JsonValue::MakeArray();
            for (const std::string& touchedId : touched) terrainIds.Push(JsonValue(touchedId));
            detailSink->Set("terrains", std::move(terrainIds));
            detailSink->Set("iterations", JsonValue(iterations));
            if (isSculpt && sculptOp == TerrainSculptOp::Flatten)
                detailSink->Set("targetHeight", JsonValue(flattenTarget));
        }
        return MakeTerrainEditCommand(scene, isSculpt ? "AI: Sculpt Terrain" : "AI: Paint Terrain",
                                      std::move(before), std::move(after), markDirty);
    }

    // Terrain レイヤーへ .mat を割り当てる。Paint する前にレイヤーの中身を決められないと、
    // 「塗ったのに見た目が変わらない (レイヤーが空)」という状態にしかならない。
    if (type == "terrain.setLayerMaterial") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr) { err = Outcome::Err("NOT_PRESENT", "TerrainComponent が装着されていません"); return nullptr; }
        const JsonValue* layerValue = payload.Find("layer");
        if (layerValue == nullptr || !layerValue->IsNumber()) { err = Outcome::Err("BAD_ARG", "layer (0〜3) が必要です"); return nullptr; }
        const int layer = layerValue->AsInt();
        if (layer < 0 || layer > 3) { err = Outcome::Err("BAD_ARG", "layer は 0〜3 です"); return nullptr; }
        const std::string materialPath = StringField(payload, "material");
        if (!materialPath.empty()) {
            std::filesystem::path resolved;
            std::string relative;
            if (!ResolveProjectFile(ctx, materialPath, resolved, relative)
                || LowerAscii(resolved.extension().string()) != ".mat") {
                err = Outcome::Err("BAD_PATH", "material は projectRoot 配下の .mat で指定してください"); return nullptr;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(resolved, ec)) {
                err = Outcome::Err("MATERIAL_NOT_FOUND", "マテリアルが見つかりません: " + relative); return nullptr;
            }
        }
        const std::string oldMaterial = terrain->layerMaterials[static_cast<size_t>(layer)];
        return std::make_unique<LambdaCommand>("AI: Set Terrain Layer Material",
            [scene, id, layer, materialPath, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id))
                    if (auto* t = g->GetComponent<scene::TerrainComponent>()) t->SetLayerMaterial(layer, materialPath);
                markDirty();
            },
            [scene, id, layer, oldMaterial, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id))
                    if (auto* t = g->GetComponent<scene::TerrainComponent>()) t->SetLayerMaterial(layer, oldMaterial);
                markDirty();
            });
    }

    // ── NavMesh: 再ベイク要求 ──────────────────────────────────────────────
    // 地形を彫った直後の NavMesh は古い形のままで、その状態で経路を引くと
    // 「壁を通り抜ける経路」が返る。sculpt の後は必ずこれを通す運用にする。
    if (type == "navmesh.bake") {
        const std::string id = StringField(payload, "id");
        std::vector<std::string> targets;
        for (scene::EntityID eid : scene->GetEntities<scene::NavMeshSurfaceComponent>()) {
            GameObject* surfaceGo = scene->GetGameObject(eid);
            auto* surface = scene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
            if (surfaceGo == nullptr || surface == nullptr) continue;
            if (!id.empty() && surfaceGo->instanceId != id) continue;
            if (id.empty() && !surface->enabled) continue;
            targets.push_back(surfaceGo->instanceId);
        }
        if (targets.empty()) {
            err = Outcome::Err("NO_NAVMESH_SURFACE", id.empty()
                ? "有効な NavMeshSurfaceComponent がシーンにありません"
                : "NavMeshSurfaceComponent が見つかりません: " + id);
            return nullptr;
        }
        if (detailSink != nullptr) {
            JsonValue surfaceIds = JsonValue::MakeArray();
            for (const std::string& target : targets) surfaceIds.Push(JsonValue(target));
            detailSink->Set("surfaces", std::move(surfaceIds));
            // ベイクはバックグラウンドスレッドで走る。完了は navmesh_get_state で確認させる。
            detailSink->Set("async", JsonValue(true));
            detailSink->Set("poll", JsonValue("navmesh_get_state で bakeState=done を確認してください"));
        }
        auto targetsShared = std::make_shared<std::vector<std::string>>(std::move(targets));
        // Undo は「ベイク要求」を取り消せない (結果は Terrain/Collider から再生成されるキャッシュで、
        // シーンにも保存されない)。履歴に残すのは、AI が自分の操作列を追えるようにするため。
        auto request = [scene, targetsShared]() {
            for (const std::string& target : *targetsShared) {
                if (GameObject* g = scene->FindByGuid(target))
                    if (auto* surface = g->GetComponent<scene::NavMeshSurfaceComponent>()) surface->needsBake = true;
            }
        };
        return std::make_unique<LambdaCommand>("AI: Bake NavMesh", request, request);
    }

    // ── Audio: 再生制御 ────────────────────────────────────────────────────
    // AudioSource は pending フラグを立てると AudioSystem が次フレームに実行する。
    // Undo 可能にするのは、履歴が飛び飛びだと run_transaction で束ねたとき戻せないため。
    if (type == "audio.control") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        auto* source = go->GetComponent<scene::AudioSourceComponent>();
        if (source == nullptr) { err = Outcome::Err("NOT_PRESENT", "AudioSourceComponent が装着されていません"); return nullptr; }
        const std::string action = LowerAscii(StringField(payload, "action"));
        if (action != "play" && action != "stop" && action != "pause" && action != "resume") {
            err = Outcome::Err("BAD_ARG", "action は play / stop / pause / resume です"); return nullptr;
        }
        if (action == "play" && source->clipPath.empty()) {
            err = Outcome::Err("NO_CLIP", "clipPath が空です (component_set で設定してください)"); return nullptr;
        }
        auto request = [scene, id](const std::string& requestedAction) {
            GameObject* g = scene->FindByGuid(id);
            if (g == nullptr) return;
            auto* audio = g->GetComponent<scene::AudioSourceComponent>();
            if (audio == nullptr) return;
            if (requestedAction == "play")        audio->m_pendingPlay = true;
            else if (requestedAction == "stop")   audio->m_pendingStop = true;
            else if (requestedAction == "pause")  audio->m_pendingPause = true;
            else if (requestedAction == "resume") audio->m_pendingPlay = true;
        };
        // 取り消し方向は「再生なら停止 / それ以外は再生」。元の再生状態へ戻す。
        const std::string undoAction = (action == "play" || action == "resume") ? "stop"
            : (source->m_isPlaying ? "play" : "stop");
        return std::make_unique<LambdaCommand>("AI: Audio Control",
            [request, action]()     { request(action); },
            [request, undoAction]() { request(undoAction); });
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
        if (!editor::IsInstantiableAssetExtension(LowerAscii(prefabPath.extension().string()))) {
            err = Outcome::Err("BAD_PATH", "保存先は .prefab / .vfx 拡張子にしてください");
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
        // Propagate は他インスタンスを作り直すため、アセットだけ戻しても
        // シーンは新定義のままになる。シーンのスナップショットも併せて持つ。
        auto beforeScene = std::make_shared<std::string>();
        auto afterScene  = std::make_shared<std::string>();
        auto captured    = std::make_shared<bool>(false);
        return std::make_unique<LambdaCommand>("AI: Apply Prefab",
            [scene, id, projectRoot, beforeContent, existedBefore,
             beforeScene, afterScene, captured, markDirty]() {
                GameObject* target = scene->FindByGuid(id);
                if (target == nullptr || target->prefabAssetPath.empty()) return;
                if (!*captured) {
                    // Apply 前のアセット内容とシーンを退避して undo で書き戻せるようにする。
                    const std::string disk =
                        editor::ToProjectAssetDiskPath(projectRoot, target->prefabAssetPath);
                    *existedBefore = util::FileSystem::ReadText(disk, *beforeContent);
                    *beforeScene = editor::SceneIO::Serialize(*scene);
                    // Apply だけだと同じ .prefab の他インスタンスが新定義へ揃わず、
                    // 「ファイルは変わったのに画面の実体は古いまま」になる。
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
                // アセットを戻し、伝播で作り直された実体もシーンごと戻す。
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
        // Unload はスロットを解放して世代を進めるので、配ってある AssetHandle が一斉に死ぬ。
        // ReloadPath はハンドルを保ったまま両方の実体を差し替える。
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

    // 実体は SceneEditUtils の MakeRenameNodeCommand。Hierarchy パネルのインライン
    // リネームも同じものを通るので、Undo の重さも復元の仕方も経路で変わらない。
    // Docs/design/editor-operator-model.md
    if (type == "node.rename") {
        const std::string id = StringField(payload, "id");
        const std::string name = StringField(payload, "name");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        if (name.empty()) { err = Outcome::Err("BAD_ARG", "name が空です"); return nullptr; }
        // 履歴ラベルだけ AI 用にする — editor_get_undo_history で自分の編集を識別できるため。
        // dryRun でもここまでは通るため、適用は UndoStack::Execute に任せる
        // (applyNow=true にすると dryRun が実際にシーンを書き換えてしまう)。
        auto command = MakeRenameNodeCommand(ctx, go->GetID(), name,
                                             "AI: Rename Node", /*applyNow=*/false);
        if (!command) { err = Outcome::Err("NO_CHANGE", "名前が変わりません: " + name); return nullptr; }
        return command;
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
        // Sprite 参照は «解決できないと静かにアトラス全面» になる。書けてしまう前に止める。
        // WHY ここで見るか: 成功を返してから絵だけが違う、が一番追えない壊れ方で、
        //     AI からは «割り当てたのに効かない» としか見えない。
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
            const JsonValue* defaultValue  = payload.Find("defaultStateName");
            const JsonValue* addSrcValue   = payload.Find("additiveSourcePath");
            const JsonValue* addClipValue  = payload.Find("additiveClipName");
            const JsonValue* addTimeValue  = payload.Find("additiveTime");

            const float weight = (weightValue && weightValue->IsNumber())
                ? std::clamp(static_cast<float>(weightValue->AsNumber()), 0.0f, 1.0f) : 0.0f;
            const bool  enabled = (enabledValue && enabledValue->IsBool()) && enabledValue->AsBool();
            const std::string maskPath = (maskValue && maskValue->IsString()) ? maskValue->AsString() : std::string{};
            const std::string defaultStateName = (defaultValue && defaultValue->IsString()) ? defaultValue->AsString() : std::string{};
            const std::string addSrc = (addSrcValue && addSrcValue->IsString()) ? addSrcValue->AsString() : std::string{};
            const std::string addClip = (addClipValue && addClipValue->IsString()) ? addClipValue->AsString() : std::string{};
            const float addTime = (addTimeValue && addTimeValue->IsNumber()) ? static_cast<float>(addTimeValue->AsNumber()) : 0.0f;

            const bool hasWeight  = weightValue && weightValue->IsNumber();
            const bool hasEnabled = enabledValue && enabledValue->IsBool();
            const bool hasMask    = maskValue && maskValue->IsString();
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
                    if (hasDefault) l->defaultStateName = defaultStateName;
                    if (hasAddSrc)  l->additiveReference.sourcePath = addSrc;
                    if (hasAddClip) l->additiveReference.clipName = addClip;
                    if (hasAddTime) l->additiveReference.time = addTime;
                    if (!newName.empty()) l->name = newName;
                });
        }

        // ── Slot ────────────────────────────────────────────────────────────
        // Slot はランタイム状態だが、他の編集と同じ Command 経路に載せる。dryRun 判定と
        // Undo への積み込みを呼び出し側へ任せられる。
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

    // .fluid はアセットファイル操作 + 焼きジョブ。応答に job / path を載せるため汎用経路へ流さない。
    if (type.starts_with("fluid.")) return DoFluidCommand(ctx, type, payload, dryRun);

    // シーンの入出力とビルド要求は「シーンの中身の変更」ではないため UndoStack へ載せない。
    // (Undo でシーンが閉じたり保存が巻き戻ったりする方が事故になる)
    if (type == "scene.open") return DoSceneOpen(ctx, payload, dryRun);
    if (type == "scene.save") return DoSceneSave(ctx, payload, dryRun);
    if (type == "build.run")  return DoBuildRun(ctx, payload, dryRun);

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

    // Operator へ移送済み (Step 3)。2 つ目の実装を残すと「実行できるか」の判定が
    // AI 側だけ別式になる。応答の形 (applied / description) は互換のまま維持する。
    // Docs/design/editor-operator-model.md
    if (type == "editor.undo" || type == "editor.redo") {
        if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
        if (ctx.operators == nullptr) return Outcome::Err("NO_REGISTRY", "Operator レジストリが未初期化です");

        const bool isUndo = (type == "editor.undo");
        const char* operatorId = isUndo ? "edit.undo" : "edit.redo";
        // 説明は実行前に読む (実行するとカーソルが動いて別のエントリを指す)。
        const std::string desc = isUndo ? ctx.undoStack->GetUndoDescription()
                                        : ctx.undoStack->GetRedoDescription();

        OpContext opContext{ ctx, *ctx.undoStack };
        if (!ctx.operators->CanInvoke(operatorId, opContext)) {
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

        const OpResult opResult = ctx.operators->Invoke(operatorId, opContext);
        if (!opResult.ok)
            return Outcome::Err(opResult.errorCode.empty() ? "OP_FAILED" : opResult.errorCode,
                                opResult.message);

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
        // AI が選んだものは人が確かめる対象なので、畳まれた親を開いてでも Hierarchy に出す。
        editor::SelectEntities(ctx, std::move(resolved));
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
                        type == "prefab.instantiate" || type == "prefab.create" ||
                        type == "preset.create")
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
    // 空行は NDJSON の区切りとして正常。要求ではないので黙って捨てる。
    if (requestLine.find_first_not_of(" \t\r\n") == std::string::npos) return {};

    std::string parseError;
    std::optional<JsonValue> root = ParseJson(requestLine, &parseError);
    if (!root.has_value()) {
        // 相関 id を取り出せないが、応答は返す。
        //
        // WHY 黙らないか: 以前は «id が無いので誰への応答か言えない» として何も返さず、
        //     送信側の timeout に委ねていた。実際には受け手が数十秒固まるだけで、
        //     しかも «届いていない» のか «壊れていた» のか区別が付かない。
        //     id を空で返せば «この接続で何かが壊れた» とその場で分かる。
        return SerializeJson(MakeErrorResponse({}, "BAD_JSON", parseError));
    }

    std::optional<BusRequest> request = ParseBusRequest(*root, &parseError);
    if (!request.has_value()) {
        // id が取れればそれを載せる。取れなくても «壊れている» ことは返す。
        const JsonValue* id = root->Find("id");
        const std::string correlationId =
            (id != nullptr && id->IsString()) ? id->AsString() : std::string{};
        return SerializeJson(MakeErrorResponse(correlationId, "BAD_REQUEST", parseError));
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
                // includeGenerated: 実行時に作られた GO (VFX ノード実体、Water splash) を
                // ツリーへ含めるか。既定は含めない — 生成物は編集しても保存されないので、
                // 混ぜると context を食い潰したうえで無駄な編集を誘発する。
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
        } else if (type == "sprite.list") {
            outcome = DoSpriteList(m_context, payload);
        } else if (type == "sprite.thumbnail") {
            outcome = DoSpriteThumbnail(m_context, payload);
        } else if (type == "bt.tree") {
            outcome = DoBehaviorTree(m_context, payload);
        } else if (type == "bt.lint") {
            outcome = DoBehaviorTreeLint(m_context, payload);
        } else if (type == "bt.guide") {
            outcome = DoBehaviorTreeGuide();
        } else if (type == "bt.schema") {
            outcome = DoBehaviorTreeSchema(payload);
        } else if (type == "bt.nodeField") {
            outcome = DoBehaviorTreeNodeField(m_context, payload);
        } else if (type == "bt.runtime") {
            outcome = DoBehaviorTreeRuntime(m_context, payload);
        } else if (type == "bt.diff") {
            outcome = DoBehaviorTreeDiff(m_context, payload);
        } else if (type == "bt.templateCatalog") {
            outcome = DoBehaviorTreeTemplateCatalog(m_context);
        } else if (type == "shader.inspect") {
            outcome = DoShaderInspect(m_context, payload);
        } else if (type == "shader.diagnostics") {
            outcome = DoShaderCompileDiagnostics();
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
        } else if (type == "scene.list") {
            outcome = DoSceneList(m_context);
        } else if (type == "preset.catalog") {
            outcome = DoPresetCatalog(payload);
        } else if (type == "terrain.inspect") {
            outcome = DoTerrainInspect(m_context, payload);
        } else if (type == "terrain.sample") {
            outcome = DoTerrainSample(m_context, payload);
        } else if (type == "navmesh.state") {
            outcome = DoNavMeshState(m_context, payload);
        } else if (type == "navmesh.path") {
            outcome = DoNavMeshPath(m_context, payload);
        } else if (type == "navmesh.sample") {
            outcome = DoNavMeshSample(m_context, payload);
        } else if (type == "environment.inspect") {
            outcome = DoEnvironmentInspect(m_context);
        } else if (type == "audio.inspect") {
            outcome = DoAudioInspect(m_context, payload);
        } else if (type == "ui.inspect") {
            outcome = DoUIInspect(m_context, payload);
        } else if (type == "build.status") {
            outcome = DoBuildStatus(m_context, payload);
        } else if (type == "fluid.schema") {
            outcome = DoFluidSchema();
        } else if (type == "fluid.get") {
            outcome = DoFluidGet(m_context, payload);
        } else if (type == "fluid.jobStatus") {
            outcome = DoFluidJobStatus(m_context, payload);
        } else if (type == "viewport.capture") {
            std::string view = StringField(payload, "view");
            if (view.empty()) view = "scene";
            // WHY "vfx" ビューが無くなったか: .vfx はプレファブになり、中身は
            // Prefab 編集モードで «普通のシーン» として開く。専用のプレビュー面が
            // 無いので、撮る対象も scene ビューそのものになる。
            if (view != "scene" && view != "game") {
                outcome = Outcome::Err("BAD_ARG", "view は scene か game で指定してください");
            } else {
                // EditorApp は画面に出ているビューポートしか描かないため、キャプチャ中だけは
                // 隠れているビューも描き続けさせる。連続キャプチャで古い絵を掴まないための猶予。
                m_context.aiViewportRenderUntilFrame = Time::frameCount + 8;
                const auto target = view == "game" ? m_gameViewportRT : m_sceneViewportRT;
                outcome = DoViewportCapture(m_context, target);
                if (outcome.ok) outcome.result.Set("view", JsonValue(view));
            }
        } else if (type == "viewport.semantic") {
            std::string view = StringField(payload, "view");
            if (view.empty()) view = "scene";
            // viewport.capture と同じ理由で、隠れているビューも描き続けさせる。
            m_context.aiViewportRenderUntilFrame = Time::frameCount + 8;
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
        } else if (type == "editor.op.list") {
            // Operator モデルの目録 (Docs/design/editor-operator-model.md)。
            // メニュー・ホットキー・コマンドパレットが読むのと同じ登録簿を返すため、
            // AI からだけ見えない操作も、AI にだけできる操作も原理的に作れない。
            outcome = FromBridge(ListOperators(m_context, payload));
        } else if (type == "editor.op.query") {
            // kind=query の Operator を実行して結果データを返す。
            // editor.op.invoke は write 権限のツールなので、読むだけの操作を混ぜると
            // read 権限の接続から「目録には出るが呼べない Query」に見える。
            outcome = FromBridge(QueryOperator(m_context, payload));
        } else {
            outcome = Outcome::Err("UNKNOWN_QUERY", "未対応の Query: " + type);
        }
    }
    // ── Command (Undo 可能に適用 / dryRun は試算のみ) ─────────────────────
    else if (type == "editor.op.invoke") {
        // 実行可否 (poll) の判定は Operator 側にあり、人が使う面と同じ述語が効く。
        outcome = FromBridge(InvokeOperator(m_context, payload, request->dryRun));
    }
    else {
        outcome = DoCommand(m_context, type, payload, request->dryRun);
    }

    if (outcome.ok) return SerializeJson(MakeOkResponse(request->id, std::move(outcome.result)));
    return SerializeJson(MakeErrorResponse(request->id, outcome.code, outcome.message));
}

} // namespace fbzz::editor::ai
