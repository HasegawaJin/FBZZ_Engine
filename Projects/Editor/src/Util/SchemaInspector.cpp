/// @file    SchemaInspector.cpp
/// @brief   ITypeSchema 走査による汎用 Inspector の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/SchemaInspector.hpp>

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>

#include <imgui.h>
#include <algorithm>
#include <any>
#include <cstdio>
#include <functional>

namespace fbzz::editor::widgets {
namespace {

using reflection::PropertyDesc;
using reflection::PropertyType;

// leaf の値を型付きで取り出す。型が食い違う (スキーマ側の宣言ミス) 場合は fallback を返す。
template<typename T>
T ReadValue(const PropertyDesc& property, const void* owner, const T& fallback)
{
    if (property.get == nullptr) return fallback;
    const std::any value = property.get(owner);
    const T* typed = std::any_cast<T>(&value);
    return typed != nullptr ? *typed : fallback;
}

template<typename T>
bool WriteValue(const PropertyDesc& property, void* owner, const T& value)
{
    return property.set != nullptr && property.set(owner, std::any(value));
}

std::string JoinPath(std::string_view prefix, std::string_view key)
{
    if (prefix.empty()) return std::string(key);
    std::string joined(prefix);
    joined += '.';
    joined += key;
    return joined;
}

// skipPaths は前方一致で判定する。"particle.colorGradient" のような親指定で
// その配下をまとめて手書き UI へ委ねられるようにするため。
bool IsSkipped(const std::vector<std::string>& skipPaths, const std::string& path)
{
    for (const std::string& skip : skipPaths) {
        if (path.size() < skip.size()) continue;
        if (path.compare(0, skip.size(), skip) != 0) continue;
        if (path.size() == skip.size() || path[skip.size()] == '.' || path[skip.size()] == '[')
            return true;
    }
    return false;
}

// 数値レンジが宣言されていればゲージ付きフィールド、なければ自由入力の DragFloat を使う。
bool DrawFloat(const PropertyDesc& property, void* owner, const char* label)
{
    float value = ReadValue<float>(property, owner, 0.0f);
    const bool changed = property.range.enabled
        ? RangeField(label, value, property.range.minimum, property.range.maximum)
        : ImGui::DragFloat(label, &value, 0.01f);
    return changed && WriteValue(property, owner, value);
}

bool DrawInt(const PropertyDesc& property, void* owner, const char* label)
{
    int value = ReadValue<int>(property, owner, 0);
    bool changed = false;
    if (property.range.enabled) {
        // レンジ宣言があるなら float 版と同じゲージ付きフィールドで描く
        // (同じスキーマ Inspector の中で範囲付き数値の見た目を 1 つに保つ)。
        const int minimum = static_cast<int>(property.range.minimum);
        const int maximum = static_cast<int>(property.range.maximum);
        changed = RangeField(label, value, minimum, maximum);
    } else {
        changed = ImGui::DragInt(label, &value);
    }
    return changed && WriteValue(property, owner, value);
}

// 値名があれば Combo、無ければ数値のまま。名前の欠落で編集不能にはしない。
bool DrawEnum(const PropertyDesc& property, void* owner, const char* label)
{
    int value = ReadValue<int>(property, owner, 0);
    const int maximum = static_cast<int>(property.range.maximum);
    if (property.enumNames.empty()) return DrawInt(property, owner, label);

    const auto nameAt = [&property](int index) -> const char* {
        if (index < 0 || static_cast<std::size_t>(index) >= property.enumNames.size())
            return "<invalid>";
        // string_view は非終端の可能性があるが、ここは静的な文字列リテラル由来なので安全。
        return property.enumNames[static_cast<std::size_t>(index)].data();
    };

    bool changed = false;
    if (ImGui::BeginCombo(label, nameAt(value))) {
        const int count = (std::min)(maximum + 1, static_cast<int>(property.enumNames.size()));
        for (int index = 0; index < count; ++index) {
            const bool selected = index == value;
            if (ImGui::Selectable(nameAt(index), selected)) { value = index; changed = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed && WriteValue(property, owner, value);
}

bool DrawLeaf(const PropertyDesc& property, void* owner, const std::string& projectRoot)
{
    // 表示名は string_view だが、由来は静的文字列リテラルなので data() をそのまま使える。
    const char* label = property.display.empty() ? property.key.data() : property.display.data();

    switch (property.type) {
    case PropertyType::Float:
        return DrawFloat(property, owner, label);
    case PropertyType::Int:
        return DrawInt(property, owner, label);
    case PropertyType::Enum:
        return DrawEnum(property, owner, label);
    case PropertyType::Bool: {
        bool value = ReadValue<bool>(property, owner, false);
        return ImGui::Checkbox(label, &value) && WriteValue(property, owner, value);
    }
    case PropertyType::Vector3: {
        math::Vector3 value = ReadValue<math::Vector3>(property, owner, {});
        return DragVec3(label, value) && WriteValue(property, owner, value);
    }
    case PropertyType::Color: {
        math::Vector4 value = ReadValue<math::Vector4>(property, owner, { 1, 1, 1, 1 });
        return ColorEdit4(label, value) && WriteValue(property, owner, value);
    }
    case PropertyType::String: {
        std::string value = ReadValue<std::string>(property, owner, {});
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        if (!ImGui::InputText(label, buffer, sizeof(buffer))) return false;
        return WriteValue(property, owner, std::string(buffer));
    }
    case PropertyType::AssetRef: {
        std::string value = ReadValue<std::string>(property, owner, {});
        if (!AssetPathField(label, value, "", projectRoot)) return false;
        return WriteValue(property, owner, value);
    }
    case PropertyType::Curve: {
        scene::ParticleCurve value = ReadValue<scene::ParticleCurve>(property, owner, {});
        // 縦軸の上限はレンジ宣言があればそれに合わせる (Size は 1.0、Velocity は 10.0 等)。
        const float maximum = property.range.enabled && property.range.maximum > 0.0f
            ? property.range.maximum : 1.0f;
        if (!CurveEditor(label, value, maximum)) return false;
        return WriteValue(property, owner, value);
    }
    case PropertyType::Gradient: {
        scene::ParticleGradient value = ReadValue<scene::ParticleGradient>(property, owner, {});
        if (!GradientEditor(label, value)) return false;
        return WriteValue(property, owner, value);
    }
    default:
        // Vector2 / Quaternion は VFX authoring に登場しないため未対応。
        // 追加する場合はここへウィジェットを足すだけでよい。
        ImGui::TextDisabled("%s (unsupported type)", label);
        return false;
    }
}

// 配列 leaf: 要素数の増減と、各要素の入れ子描画。
// 要素型は常に構造体なので、描画自体は DrawSchemaProperties の再帰へ委ねる。
bool DrawArray(const PropertyDesc& property, void* owner, const std::string& projectRoot,
               const std::vector<std::string>& skipPaths, const std::string& path)
{
    if (property.childSchema == nullptr || property.arraySize == nullptr
        || property.getElement == nullptr || property.resizeArray == nullptr) {
        return false;
    }
    bool changed = false;
    const std::size_t count = property.arraySize(owner);

    char header[160] = {};
    std::snprintf(header, sizeof(header), "%s (%zu)",
                  property.display.empty() ? property.key.data() : property.display.data(), count);
    if (!ImGui::TreeNode(header)) return false;

    if (ImGui::SmallButton("+")) {
        changed = property.resizeArray(owner, count + 1);
    }
    ImGui::SameLine();
    // 空配列で "-" を押しても resize(-1) にならないよう、要素があるときだけ有効にする。
    ImGui::BeginDisabled(count == 0);
    if (ImGui::SmallButton("-")) {
        changed = property.resizeArray(owner, count - 1);
    }
    ImGui::EndDisabled();

    // resize でポインタが無効化されうるため、増減があったフレームは要素描画を行わない。
    if (changed) { ImGui::TreePop(); return true; }

    for (std::size_t index = 0; index < count; ++index) {
        void* element = property.getElement(owner, index);
        if (element == nullptr) continue;
        ImGui::PushID(static_cast<int>(index));
        char elementLabel[64] = {};
        std::snprintf(elementLabel, sizeof(elementLabel), "[%zu]", index);
        if (ImGui::TreeNode(elementLabel)) {
            changed |= DrawSchemaProperties(*property.childSchema, element, projectRoot, skipPaths,
                                            path + '[' + std::to_string(index) + ']');
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
    return changed;
}

} // namespace

bool DrawSchemaProperties(const reflection::ITypeSchema& schema, void* owner,
                          const std::string& projectRoot,
                          const std::vector<std::string>& skipPaths,
                          std::string_view pathPrefix)
{
    if (owner == nullptr) return false;
    bool changed = false;

    for (const PropertyDesc& property : schema.Properties()) {
        const std::string path = JoinPath(pathPrefix, property.key);
        if (IsSkipped(skipPaths, path)) continue;
        ImGui::PushID(static_cast<int>(std::hash<std::string>{}(path) & 0x7fffffff));

        if (property.type == PropertyType::Struct) {
            void* child = property.getChild != nullptr ? property.getChild(owner) : nullptr;
            if (child != nullptr && property.childSchema != nullptr
                && ImGui::TreeNode(property.display.data())) {
                changed |= DrawSchemaProperties(*property.childSchema, child, projectRoot,
                                                skipPaths, path);
                ImGui::TreePop();
            }
        } else if (property.type == PropertyType::Array) {
            changed |= DrawArray(property, owner, projectRoot, skipPaths, path);
        } else {
            changed |= DrawLeaf(property, owner, projectRoot);
        }

        ImGui::PopID();
    }
    return changed;
}

} // namespace fbzz::editor::widgets
