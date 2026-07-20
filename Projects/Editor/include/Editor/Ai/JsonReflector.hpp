// FBZZ Engine
// JsonReflector.hpp | fbzz::editor::ai
// IReflector を実装し、コンポーネントの反射フィールドと JSON を相互変換する。
//
// 設計 (WHY):
//   Inspector の ImGuiReflector・SceneSerializer の TOML リフレクタと同じ IReflector ビジターを
//   AI 境界にも通す。これにより component.set / node.components が既存の FBZZ_FIELD 宣言に自動追従し、
//   コンポーネントごとの手書き JSON マッピングを排除する (単一の真実)。
//
//   対応範囲は数値・真偽・文字列・ベクトル・クォータニオン (= AI が調整したい大半のパラメータ)。
//   EntityID/参照型はシーン解決を要するため本リフレクタでは扱わず (読みは省略・書きは無視)、
//   構造的な変更は node.reparent 等の専用 Command 側に委ねる。
#pragma once
#include <Editor/Ai/Json.hpp>
#include <Engine/Scene/Script.hpp> // IReflector
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace fbzz::editor::ai {

// コンポーネントの Reflect() を1回通し、フィールドを JSON オブジェクトへ吸い出す (読み取り専用)。
class JsonReadReflector final : public scene::IReflector {
public:
    // 収集済みの {フィールド名: 値} オブジェクトを返す。
    const JsonValue& Result() const { return m_result; }

    void Field(const char* name, float& v) override        { m_result.Set(name, JsonValue(static_cast<double>(v))); }
    void Field(const char* name, int& v) override          { m_result.Set(name, JsonValue(v)); }
    void Field(const char* name, bool& v) override         { m_result.Set(name, JsonValue(v)); }
    void Field(const char* name, math::Vector2& v) override { m_result.Set(name, MakeVec({ v.x, v.y })); }
    void Field(const char* name, math::Vector3& v) override { m_result.Set(name, MakeVec({ v.x, v.y, v.z })); }
    void Field(const char* name, math::Vector4& v) override { m_result.Set(name, MakeVec({ v.x, v.y, v.z, v.w })); }
    void Field(const char* name, std::string& v) override  { m_result.Set(name, JsonValue(v)); }
    void Field(const char* name, math::Quaternion& v) override { m_result.Set(name, MakeVec({ v.x, v.y, v.z, v.w })); }

    // Readonly (計算値) も AI の観測材料として含める。
    void Readonly(const char* name, const std::string& v) override { m_result.Set(name, JsonValue(v)); }
    void Readonly(const char* name, float v) override { m_result.Set(name, JsonValue(static_cast<double>(v))); }
    void Readonly(const char* name, int v) override { m_result.Set(name, JsonValue(v)); }

private:
    static JsonValue MakeVec(std::initializer_list<float> components)
    {
        JsonValue array = JsonValue::MakeArray();
        for (const float c : components) array.Push(JsonValue(static_cast<double>(c)));
        return array;
    }

    JsonValue m_result = JsonValue::MakeObject();
};

// Reflect() が公開する編集契約を値ではなくスキーマとして収集する。
// WHY: Inspector と同じ IReflector を正本にすることで、AI 向けカタログがフィールド追加や
//      enum/range 変更へ自動追従し、名前や許容値を推測する必要をなくす。
class JsonCatalogReflector final : public scene::IReflector {
public:
    const JsonValue& Result() const { return m_fields; }

    void Field(const char* name, float& v) override { Add(name, "float", JsonValue(static_cast<double>(v))); }
    void Field(const char* name, int& v) override { Add(name, "int", JsonValue(v)); }
    void Field(const char* name, bool& v) override { Add(name, "bool", JsonValue(v)); }
    void Field(const char* name, math::Vector2& v) override { Add(name, "vector2", MakeVec({ v.x, v.y })); }
    void Field(const char* name, math::Vector3& v) override { Add(name, "vector3", MakeVec({ v.x, v.y, v.z })); }
    void Field(const char* name, math::Vector4& v) override { Add(name, "vector4", MakeVec({ v.x, v.y, v.z, v.w })); }
    void Field(const char* name, std::string& v) override { Add(name, "string", JsonValue(v)); }
    void Field(const char* name, math::Quaternion& v) override { Add(name, "quaternion", MakeVec({ v.x, v.y, v.z, v.w })); }

    void FloatRange(const char* name, float& v, float min, float max) override
    {
        AddRange(name, "float", JsonValue(static_cast<double>(v)), min, max);
    }

    void IntRange(const char* name, int& v, int min, int max) override
    {
        AddRange(name, "int", JsonValue(v), min, max);
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        JsonValue field = MakeField(name, "int", JsonValue(v));
        JsonValue enumLabels = JsonValue::MakeArray();
        for (const char* label : labels) enumLabels.Push(JsonValue(label != nullptr ? label : ""));
        field.Set("enumLabels", std::move(enumLabels));
        JsonValue range = JsonValue::MakeObject();
        range.Set("min", JsonValue(0));
        range.Set("max", JsonValue(labels.empty() ? 0 : static_cast<int>(labels.size() - 1)));
        field.Set("range", std::move(range));
        m_fields.Push(std::move(field));
    }

    void Tooltip(const char* text) override
    {
        if (!m_fields.AsArray().empty() && text != nullptr && text[0] != '\0')
            m_fields.AsArray().back().Set("description", JsonValue(text));
    }

    void Group(const char* label) override
    {
        m_group = label != nullptr ? label : "";
    }

private:
    static JsonValue MakeVec(std::initializer_list<float> components)
    {
        JsonValue array = JsonValue::MakeArray();
        for (const float component : components) array.Push(JsonValue(static_cast<double>(component)));
        return array;
    }

    JsonValue MakeField(const char* name, const char* type, JsonValue defaultValue) const
    {
        JsonValue field = JsonValue::MakeObject();
        field.Set("name", JsonValue(name));
        field.Set("type", JsonValue(type));
        field.Set("default", std::move(defaultValue));
        if (!m_group.empty()) field.Set("group", JsonValue(m_group));
        return field;
    }

    void Add(const char* name, const char* type, JsonValue defaultValue)
    {
        m_fields.Push(MakeField(name, type, std::move(defaultValue)));
    }

    template<typename Number>
    void AddRange(const char* name, const char* type, JsonValue defaultValue, Number min, Number max)
    {
        JsonValue field = MakeField(name, type, std::move(defaultValue));
        JsonValue range = JsonValue::MakeObject();
        range.Set("min", JsonValue(static_cast<double>(min)));
        range.Set("max", JsonValue(static_cast<double>(max)));
        field.Set("range", std::move(range));
        m_fields.Push(std::move(field));
    }

    JsonValue   m_fields = JsonValue::MakeArray();
    std::string m_group;
};

// 目標フィールド名に一致した1フィールドだけを JSON から書き込む。他フィールドは素通しする。
class JsonWriteReflector final : public scene::IReflector {
public:
    JsonWriteReflector(std::string targetField, const JsonValue& value)
        : m_target(std::move(targetField)), m_value(value) {}

    // 目標フィールドを発見し値を代入できたか。
    bool Applied() const { return m_applied; }
    // 発見したが型不一致で代入しなかった場合の理由 (空 = なし)。
    const std::string& Error() const { return m_error; }

    void Field(const char* name, float& v) override
    {
        if (!Match(name)) return;
        if (m_value.IsNumber()) { v = static_cast<float>(m_value.AsNumber()); m_applied = true; }
        else TypeError("number");
    }
    void Field(const char* name, int& v) override
    {
        if (!Match(name)) return;
        if (m_value.IsNumber()) { v = m_value.AsInt(); m_applied = true; }
        else TypeError("number");
    }
    void Field(const char* name, bool& v) override
    {
        if (!Match(name)) return;
        if (m_value.IsBool()) { v = m_value.AsBool(); m_applied = true; }
        else TypeError("boolean");
    }
    void Field(const char* name, math::Vector2& v) override
    {
        if (!Match(name)) return;
        float c[4];
        if (ReadNumbers(2, c)) { v.x = c[0]; v.y = c[1]; m_applied = true; } else TypeError("array[2]");
    }
    void Field(const char* name, math::Vector3& v) override
    {
        if (!Match(name)) return;
        float c[4];
        if (ReadNumbers(3, c)) { v.x = c[0]; v.y = c[1]; v.z = c[2]; m_applied = true; } else TypeError("array[3]");
    }
    void Field(const char* name, math::Vector4& v) override
    {
        if (!Match(name)) return;
        float c[4];
        if (ReadNumbers(4, c)) { v.x = c[0]; v.y = c[1]; v.z = c[2]; v.w = c[3]; m_applied = true; } else TypeError("array[4]");
    }
    void Field(const char* name, std::string& v) override
    {
        if (!Match(name)) return;
        if (m_value.IsString()) { v = m_value.AsString(); m_applied = true; }
        else TypeError("string");
    }
    void Field(const char* name, math::Quaternion& v) override
    {
        if (!Match(name)) return;
        float c[4];
        if (ReadNumbers(4, c)) { v.x = c[0]; v.y = c[1]; v.z = c[2]; v.w = c[3]; m_applied = true; } else TypeError("array[4]");
    }

    void FloatRange(const char* name, float& v, float min, float max) override
    {
        if (!Match(name)) return;
        if (m_value.IsNumber()) {
            v = std::clamp(static_cast<float>(m_value.AsNumber()), min, max);
            m_applied = true;
        } else TypeError("number");
    }

    void IntRange(const char* name, int& v, int min, int max) override
    {
        if (!Match(name)) return;
        if (m_value.IsNumber()) {
            v = std::clamp(m_value.AsInt(), min, max);
            m_applied = true;
        } else TypeError("number");
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (!Match(name)) return;
        if (m_value.IsNumber()) {
            const int maxValue = labels.empty() ? 0 : static_cast<int>(labels.size() - 1);
            v = std::clamp(m_value.AsInt(), 0, maxValue);
            m_applied = true;
        } else TypeError("number");
    }

private:
    bool Match(const char* name) const { return !m_applied && m_error.empty() && m_target == name; }

    // JsonValue 配列の先頭 count 要素を数値として out[0..count) へ読む。要素不足・非数値なら false。
    bool ReadNumbers(int count, float out[4]) const
    {
        if (!m_value.IsArray()) return false;
        const auto& array = m_value.AsArray();
        if (static_cast<int>(array.size()) < count) return false;
        for (int i = 0; i < count; ++i) {
            const JsonValue& element = array[static_cast<std::size_t>(i)];
            if (!element.IsNumber()) return false;
            out[i] = static_cast<float>(element.AsNumber());
        }
        return true;
    }

    void TypeError(const char* expected)
    {
        m_error = std::string("field '") + m_target + "' は " + expected + " を期待します";
    }

    std::string      m_target;
    const JsonValue& m_value;
    bool             m_applied = false;
    std::string      m_error;
};

} // namespace fbzz::editor::ai
