/// @file    BusCommon.cpp
/// @brief   Command Bus のハンドラーが共有する JSON 取り出し・コンポーネント操作・座標変換の補助。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

std::string StringField(const JsonValue& obj, const char* key)
{
    const JsonValue* v = obj.Find(key);
    return (v != nullptr && v->IsString()) ? v->AsString() : std::string{};
}

/// @brief payload の key を長さ3の数値配列として Vector3 に読む。
bool ReadVec3(const JsonValue& obj, const char* key, math::Vector3& out)
{
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || !v->IsArray() || v->AsArray().size() < 3) return false;
    const auto& a = v->AsArray();
    if (!a[0].IsNumber() || !a[1].IsNumber() || !a[2].IsNumber()) return false;
    out = { static_cast<float>(a[0].AsNumber()), static_cast<float>(a[1].AsNumber()), static_cast<float>(a[2].AsNumber()) };
    return true;
}

/// @brief AI へ公開可能な登録コンポーネント名かを調べる。
/// @note Hidden 型は内部実装なので検索条件にも露出しない。
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
        /// @note AddComponent は値渡し (move) するため、既定構築かつ move 可能な型のみ追加可能とみなす。
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

/// @brief 指定コンポーネントの反射フィールドを JSON オブジェクトで返す。
/// @return 型不明・未装着・非反射なら nullopt。
std::optional<JsonValue> ReadComponentFields(GameObject& go, const std::string& comp)
{
    std::optional<JsonValue> result;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if (result.has_value()) return;
        if (std::string_view(Reg::serializedName) != comp) return;
        if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) {
            if (T* component = go.GetComponent<T>()) {
                JsonReadReflector reader(true);
                component->Reflect(reader);
                result = reader.Result();
            }
        }
    });
    return result;
}

/// @brief 1 フィールドを JSON 値で上書きする。
/// @return 成功なら true。失敗時は errMsg に理由を書く。
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

/// @brief 全反射コンポーネントを `[{type, fields}]` 配列に写す。
/// @note node.components と delete スナップショットで共用する。
JsonValue SnapshotComponents(GameObject& go, bool observations)
{
    JsonValue array = JsonValue::MakeArray();
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::inspectorMode == scene::ComponentInspectorMode::Hidden) {
            /// @note Bone / ScriptComponent 等の内部型は公開しない。
            return;
        } else {
            if (T* component = go.GetComponent<T>()) {
                JsonValue entry = JsonValue::MakeObject();
                entry.Set("type", JsonValue(Reg::serializedName));
                if constexpr (requires(T& c, scene::IReflector& r) { c.Reflect(r); }) {
                    JsonReadReflector reader(observations);
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

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

/// @brief OperatorBridge の結果を Outcome へ詰め替える。
/// @note Outcome はこの翻訳単位に閉じた型なので、境界で 1 度だけ変換する。
Outcome FromBridge(OperatorBridgeResult bridge)
{
    if (bridge.ok) return Outcome::Ok(std::move(bridge.result));
    return Outcome::Err(std::move(bridge.code), std::move(bridge.message));
}

/// @brief dryRun 応答として、変更せず「何をする予定か」を返す。
Outcome DryRunPreview(const std::string& type)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("dryRun", JsonValue(true));
    result.Set("would", JsonValue(type));
    return Outcome::Ok(std::move(result));
}

/// @brief 2つの JSON 値が同じ大分類 (数値/真偽/文字列/配列/オブジェクト) かを判定する。
/// @note component.set の型チェックに使う。
bool CompatibleJsonType(const JsonValue& a, const JsonValue& b)
{
    const auto category = [](const JsonValue& v) {
        if (v.IsNumber()) return 0;
        if (v.IsBool())   return 1;
        if (v.IsString()) return 2;
        if (v.IsArray())  return 3;
        if (v.IsObject()) return 4;
        /// @note 上記いずれでもなければ null。
        return 5;
    };
    return category(a) == category(b);
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

const char* PlayStateName(const editor::PlayModeController& playMode)
{
    if (playMode.IsPlaying()) return "playing";
    if (playMode.IsPaused()) return "paused";
    return "editor";
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

JsonValue MatrixToJson(const math::Matrix4& matrix)
{
    JsonValue result = JsonValue::MakeArray();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) result.Push(JsonValue(matrix.m[row][column]));
    }
    return result;
}

int VirtualKeyFromName(std::string name)
{
    name = LowerAscii(std::move(name));
    /// @note "#<仮想キーコード>" は入力の記録 (InputRecorder) が名前の無いキーに使う形。
    if (name.size() >= 2 && name[0] == '#') {
        int code = 0;
        for (size_t index = 1; index < name.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(name[index]))) return -1;
            code = code * 10 + (name[index] - '0');
            if (code > 255) return -1;
        }
        return code;
    }
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

} /// namespace fbzz::editor::ai::bus
