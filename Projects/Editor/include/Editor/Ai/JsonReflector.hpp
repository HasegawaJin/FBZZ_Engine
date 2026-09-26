/// @file    JsonReflector.hpp
/// @brief   IReflector を実装し、コンポーネントの反射フィールドと JSON を相互変換する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note Inspector の ImGuiReflector・SceneSerializer の TOML リフレクタと同じ IReflector ビジターを AI 境界にも
///       通し、component.set / node.components を既存の FBZZ_FIELD 宣言に自動追従させる。
/// @note Script 観測は getter を明示的に有効化し、参照解決を呼び出し元から注入する。保存用収集では getter を呼ばない。
#pragma once
#include <Editor/Ai/Json.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor::ai {

/// コンポーネントの Reflect() を1回通し、フィールドを JSON オブジェクトへ吸い出す (読み取り専用)。
class JsonReadReflector final : public scene::IReflector {
public:
    using EntityResolver = std::function<JsonValue(scene::EntityID, const char*)>;
    /// @note 既定は保存用。実行状態の観測を要求する呼び出し元だけ true を渡す。
    explicit JsonReadReflector(bool observations = false, EntityResolver resolver = {})
        : m_observations(observations), m_entityResolver(std::move(resolver)) {}
    bool BeginObservation(const char*, const char*) override { return m_observations; }
    [[nodiscard]] const std::string& Error() const { return m_error; }
    void Field(const char* name, scene::EntityID& value) override
    {
        if (m_entityResolver) Store(PersistentKey(name), ResolveEntity(value));
    }
    void RefField(const char* name, scene::EntityRef& value, const char* typeName) override
    {
        if (m_entityResolver) Store(PersistentKey(name), ResolveEntity(value.id, typeName));
    }
    void RefListField(const char* name, std::vector<scene::EntityRef>& values, const char* typeName) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values) array.Push(ResolveEntity(value.id, typeName));
        Store(PersistentKey(name), std::move(array));
    }

    void Field(const char* name, input::KeyCode& value) override
    {
        if (m_observations) Store(PersistentKey(name), JsonValue(static_cast<int>(value)));
    }
    void Field(const char* name, scene::ParticleCurve&) override { Unsupported(name, "particleCurve"); }
    void Field(const char* name, scene::ParticleGradient&) override { Unsupported(name, "particleGradient"); }

    /// 収集済みの {フィールド名: 値} オブジェクトを返す。
    const JsonValue& Result() const { return m_result; }

    void Field(const char* name, float& v) override        { Store(PersistentKey(name), JsonValue(static_cast<double>(v))); }
    void Field(const char* name, int& v) override          { Store(PersistentKey(name), JsonValue(v)); }
    void Field(const char* name, bool& v) override         { Store(PersistentKey(name), JsonValue(v)); }
    void Field(const char* name, math::Vector2& v) override { Store(PersistentKey(name), MakeVec({ v.x, v.y })); }
    void Field(const char* name, math::Vector3& v) override { Store(PersistentKey(name), MakeVec({ v.x, v.y, v.z })); }
    void Field(const char* name, math::Vector4& v) override { Store(PersistentKey(name), MakeVec({ v.x, v.y, v.z, v.w })); }
    void Field(const char* name, std::string& v) override  { Store(PersistentKey(name), JsonValue(v)); }
    void Field(const char* name, math::Quaternion& v) override { Store(PersistentKey(name), MakeVec({ v.x, v.y, v.z, v.w })); }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& value,
                    scene::ScriptAssetType) override
    {
        JsonValue asset = JsonValue::MakeObject();
        asset.Set("guid", JsonValue(value.guid));
        asset.Set("path", JsonValue(value.ResolvePath()));
        Store(PersistentKey(name), std::move(asset));
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (float value : values) array.Push(JsonValue(static_cast<double>(value)));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (int value : values) array.Push(JsonValue(value));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (bool value : values) array.Push(JsonValue(value));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values) array.Push(JsonValue(value));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values) array.Push(MakeVec({ value.x, value.y }));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values) array.Push(MakeVec({ value.x, value.y, value.z }));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values)
            array.Push(MakeVec({ value.x, value.y, value.z, value.w }));
        Store(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (const auto& value : values) {
            array.Push(ResolveEntity(value.id));
        }
        Store(PersistentKey(name), std::move(array));
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType type) override
    {
        JsonValue array = JsonValue::MakeArray();
        for (auto& value : values) {
            JsonReadReflector child(m_observations, m_entityResolver);
            child.AssetField("value", value, type);
            if (const JsonValue* asset = child.Result().Find("value"))
                array.Push(*asset);
        }
        Store(PersistentKey(name), std::move(array));
    }
    /// ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    /// @name 入れ子オブジェクト / 構造体配列
    /// @{
    /// @note JsonValue::Object は std::vector で members を持つため、Set のたびに再確保が起きて既存要素へのポインタが無効化されうる。書き込み先をポインタで覚えず、子を丸ごと積んで EndObject の時点で親へ move する。
    void BeginObject(const char* name) override
    {
        m_pending.emplace_back(PersistentKey(name), JsonValue::MakeObject());
    }

    void EndObject() override
    {
        if (m_pending.empty()) return;
        auto entry = std::move(m_pending.back());
        m_pending.pop_back();
        Store(std::move(entry.first), std::move(entry.second));
    }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        m_pendingLists.emplace_back(PersistentKey(name), JsonValue::MakeArray());
        return count;
    }

    void BeginObjectElement(std::size_t index) override
    {
        (void)index;
        /// @note 配列要素はキーを持たない。空キーで積み、EndObjectElement で配列へ Push する。
        m_pending.emplace_back(std::string{}, JsonValue::MakeObject());
    }

    void EndObjectElement() override
    {
        if (m_pending.empty()) return;
        auto entry = std::move(m_pending.back());
        m_pending.pop_back();
        if (!m_pendingLists.empty())
            m_pendingLists.back().second.Push(std::move(entry.second));
    }

    std::size_t EndObjectList() override
    {
        if (m_pendingLists.empty()) return NO_REMOVE;
        auto entry = std::move(m_pendingLists.back());
        m_pendingLists.pop_back();
        Store(std::move(entry.first), std::move(entry.second));
        /// @note 観測専用のリフレクタは要素を削除しない
        return NO_REMOVE;
    }

    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        JsonValue reference = JsonValue::MakeObject();
        reference.Set("type", JsonValue(value.type));
        if (value.value) {
            JsonReadReflector child(m_observations, m_entityResolver);
            value.value->Reflect(child);
            if (!child.Error().empty()) m_error = child.Error();
            reference.Set("fields", child.Result());
        }
        Store(PersistentKey(name), std::move(reference));
    }

    /// Readonly (計算値) も AI の観測材料として含める。
    void Readonly(const char* name, const std::string& v) override { if (m_observations) Store(PersistentKey(name), JsonValue(v)); }
    void Readonly(const char* name, float v) override { if (m_observations) Store(PersistentKey(name), JsonValue(static_cast<double>(v))); }
    void Readonly(const char* name, int v) override { if (m_observations) Store(PersistentKey(name), JsonValue(v)); }
    /// @}

private:
    void Unsupported(const char* name, const char* type)
    {
        if (!m_observations) return;
        JsonValue value = JsonValue::MakeObject();
        value.Set("status", JsonValue("unsupported"));
        value.Set("type", JsonValue(type));
        Store(PersistentKey(name), std::move(value));
    }
    JsonValue ResolveEntity(scene::EntityID id, const char* typeName = "") const
    {
        if (m_entityResolver) return m_entityResolver(id, typeName);
        JsonValue value = JsonValue::MakeArray();
        value.Push(JsonValue(static_cast<int64_t>(id.index)));
        value.Push(JsonValue(static_cast<int64_t>(id.generation)));
        return value;
    }
    void Store(std::string key, JsonValue value)
    {
        if (m_observations && Current().Find(key) != nullptr)
            m_error = "Duplicate script field: " + key;
        Current().Set(std::move(key), std::move(value));
    }
    bool m_observations = false;
    EntityResolver m_entityResolver;
    std::string m_error;

    static JsonValue MakeVec(std::initializer_list<float> components)
    {
        JsonValue array = JsonValue::MakeArray();
        for (const float c : components) array.Push(JsonValue(static_cast<double>(c)));
        return array;
    }

    /// 現在の書き込み先。入れ子スコープの内側なら構築中の子オブジェクト。
    JsonValue& Current()
    {
        return m_pending.empty() ? m_result : m_pending.back().second;
    }

    JsonValue m_result = JsonValue::MakeObject();

    /// 構築中の入れ子オブジェクト (キー, 値)。配列要素はキーが空。
    std::vector<std::pair<std::string, JsonValue>> m_pending;
    std::vector<std::pair<std::string, JsonValue>> m_pendingLists;
};

/// Reflect() が公開する編集契約を値ではなくスキーマとして収集する。
/// @note Inspector と同じ IReflector を正本にすることで、AI 向けカタログがフィールド追加や enum/range 変更へ自動追従し、名前や許容値を推測する必要をなくす。
class JsonCatalogReflector final : public scene::IReflector {
public:
    explicit JsonCatalogReflector(bool includeDefaults = true) : m_includeDefaults(includeDefaults) {}
    bool BeginObservation(const char* name, const char* type) override
    {
        CurrentFields().Push(MakeField(name, type, JsonValue{}, false));
        return false;
    }
    void Field(const char* name, scene::EntityID&) override { Add(name, "entityRef", JsonValue{}); }
    void Field(const char* name, input::KeyCode& value) override { Add(name, "keyCode", JsonValue(static_cast<int>(value))); }
    void RefField(const char* name, scene::EntityRef&, const char* typeName) override
    {
        JsonValue field = MakeField(name, "entityRef", JsonValue{});
        field.Set("targetType", JsonValue(typeName ? typeName : ""));
        CurrentFields().Push(std::move(field));
    }
    void RefListField(const char* name, std::vector<scene::EntityRef>& values, const char* typeName) override
    {
        JsonValue field = MakeField(name, "list", JsonValue::MakeArray());
        field.Set("elementType", JsonValue("entityRef"));
        field.Set("targetType", JsonValue(typeName ? typeName : ""));
        field.Set("count", JsonValue(static_cast<int>(values.size())));
        CurrentFields().Push(std::move(field));
    }
    void Field(const char* name, scene::ParticleCurve&) override { Unsupported(name, "particleCurve"); }
    void Field(const char* name, scene::ParticleGradient&) override { Unsupported(name, "particleGradient"); }
    void Readonly(const char* name, const std::string&) override { BeginObservation(name, "string"); }
    void Readonly(const char* name, float) override { BeginObservation(name, "float"); }
    void Readonly(const char* name, int) override { BeginObservation(name, "int"); }
    const JsonValue& Result() const { return m_fields; }

    void Field(const char* name, float& v) override { Add(name, "float", JsonValue(static_cast<double>(v))); }
    void Field(const char* name, int& v) override { Add(name, "int", JsonValue(v)); }
    void Field(const char* name, bool& v) override { Add(name, "bool", JsonValue(v)); }
    void Field(const char* name, math::Vector2& v) override { Add(name, "vector2", MakeVec({ v.x, v.y })); }
    void Field(const char* name, math::Vector3& v) override { Add(name, "vector3", MakeVec({ v.x, v.y, v.z })); }
    void Field(const char* name, math::Vector4& v) override { Add(name, "vector4", MakeVec({ v.x, v.y, v.z, v.w })); }
    void Field(const char* name, std::string& v) override { Add(name, "string", JsonValue(v)); }
    void Field(const char* name, math::Quaternion& v) override { Add(name, "quaternion", MakeVec({ v.x, v.y, v.z, v.w })); }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& value,
                    scene::ScriptAssetType type) override
    {
        JsonValue defaultValue = JsonValue::MakeObject();
        defaultValue.Set("guid", JsonValue(value.guid));
        defaultValue.Set("path", JsonValue(value.ResolvePath()));
        JsonValue field = MakeField(name, "asset", std::move(defaultValue));
        field.Set("assetType", JsonValue(AssetTypeName(type)));
        CurrentFields().Push(std::move(field));
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        AddList(name, "float", values.size());
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        AddList(name, "int", values.size());
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        AddList(name, "bool", values.size());
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        AddList(name, "string", values.size());
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        AddList(name, "vector2", values.size());
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        AddList(name, "vector3", values.size());
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        AddList(name, "vector4", values.size());
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        AddList(name, "entityRef", values.size());
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType type) override
    {
        JsonValue field = MakeField(name, "list", JsonValue::MakeArray());
        field.Set("elementType", JsonValue("asset"));
        field.Set("assetType", JsonValue(AssetTypeName(type)));
        field.Set("count", JsonValue(static_cast<int>(values.size())));
        CurrentFields().Push(std::move(field));
    }
    /// ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    /// @name 入れ子オブジェクト / 構造体配列
    /// @{
    /// スキーマ収集も値収集と同じく「子を完成させてから親へ積む」方式にする。
    void BeginObject(const char* name) override
    {
        m_scopeFields.push_back(MakeField(name, "object", JsonValue::MakeObject()));
        m_scopeGroups.push_back(m_group);
        m_pending.emplace_back(PersistentKey(name), JsonValue::MakeArray());
    }

    void EndObject() override
    {
        if (m_pending.empty()) return;
        auto entry = std::move(m_pending.back());
        m_pending.pop_back();

        JsonValue field = FinishScope();
        field.Set("fields", std::move(entry.second));
        CurrentFields().Push(std::move(field));
    }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        /// @note 配列は「要素 1 個ぶんのスキーマ」だけを記述する。AI に渡すのは編集契約であって値ではなく、全要素を列挙しても同じ構造が繰り返されるだけでトークンを浪費するため。
        m_scopeFields.push_back(MakeField(name, "objectList", JsonValue::MakeArray()));
        m_scopeGroups.push_back(m_group);
        m_pending.emplace_back(PersistentKey(name), JsonValue::MakeArray());
        m_listElementCaptured.push_back(false);
        m_listCounts.push_back(count);
        return count;
    }

    void BeginObjectElement(std::size_t index) override
    {
        /// @note 先頭要素のスキーマだけを収集し、以降は捨てる。
        const bool capture = !m_listElementCaptured.empty()
            && !m_listElementCaptured.back() && index == 0;
        m_captureDepth.push_back(capture);
        if (!capture) m_suppress += 1;
    }

    void EndObjectElement() override
    {
        if (m_captureDepth.empty()) return;
        const bool captured = m_captureDepth.back();
        m_captureDepth.pop_back();
        if (captured) {
            if (!m_listElementCaptured.empty()) m_listElementCaptured.back() = true;
        } else if (m_suppress > 0) {
            m_suppress -= 1;
            /// @note 捨てたスキーマを溜め込まない (長い配列でメモリが膨らむのを防ぐ)。
            if (m_suppress == 0) m_discard = JsonValue::MakeArray();
        }
    }

    std::size_t EndObjectList() override
    {
        if (m_pending.empty()) return NO_REMOVE;
        auto entry = std::move(m_pending.back());
        m_pending.pop_back();

        const std::size_t count = m_listCounts.empty() ? 0u : m_listCounts.back();
        if (!m_listCounts.empty())          m_listCounts.pop_back();
        if (!m_listElementCaptured.empty()) m_listElementCaptured.pop_back();

        JsonValue field = FinishScope();
        field.Set("count", JsonValue(static_cast<int>(count)));
        field.Set("elementFields", std::move(entry.second));
        CurrentFields().Push(std::move(field));
        return NO_REMOVE;
    }
    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        JsonValue field = MakeField(name, "reference", JsonValue(value.type));
        JsonValue types = JsonValue::MakeArray();
        for (const auto& typeName : scene::ScriptSerializableFactory::RegisteredTypeNames())
            types.Push(JsonValue(typeName));
        field.Set("types", std::move(types));
        if (value.value) {
            JsonCatalogReflector child(m_includeDefaults);
            value.value->Reflect(child);
            field.Set("fields", child.Result());
        }
        CurrentFields().Push(std::move(field));
    }

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
        CurrentFields().Push(std::move(field));
    }

    void Flags(const char* name, int& v, std::span<const char* const> labels) override
    {
        Enum(name, v, labels);
        if (!CurrentFields().AsArray().empty())
            CurrentFields().AsArray().back().Set("flags", JsonValue(true));
    }

    void Tooltip(const char* text) override
    {
        if (!CurrentFields().AsArray().empty() && text != nullptr && text[0] != '\0')
            CurrentFields().AsArray().back().Set("description", JsonValue(text));
    }

    void Group(const char* label) override
    {
        m_group = label != nullptr ? label : "";
    }
    /// @}

private:
    JsonValue FinishScope()
    {
        JsonValue field = std::move(m_scopeFields.back());
        m_scopeFields.pop_back();
        m_group = std::move(m_scopeGroups.back());
        m_scopeGroups.pop_back();
        return field;
    }
    void Unsupported(const char* name, const char* type)
    {
        JsonValue field = MakeField(name, type, JsonValue{});
        field.Set("supported", JsonValue(false));
        CurrentFields().Push(std::move(field));
    }
    std::vector<JsonValue> m_scopeFields;
    std::vector<std::string> m_scopeGroups;
    bool m_includeDefaults = true;
    static const char* AssetTypeName(scene::ScriptAssetType type)
    {
        switch (type) {
        case scene::ScriptAssetType::Material:      return "Material";
        case scene::ScriptAssetType::Texture:       return "Texture";
        case scene::ScriptAssetType::Sprite:        return "Sprite";
        case scene::ScriptAssetType::AudioClip:     return "AudioClip";
        case scene::ScriptAssetType::AnimationClip: return "AnimationClip";
        case scene::ScriptAssetType::Scene:         return "Scene";
        case scene::ScriptAssetType::Shader:        return "Shader";
        case scene::ScriptAssetType::VFX:           return "VFX";
        }
        return "Unknown";
    }

    static JsonValue MakeVec(std::initializer_list<float> components)
    {
        JsonValue array = JsonValue::MakeArray();
        for (const float component : components) array.Push(JsonValue(static_cast<double>(component)));
        return array;
    }

    JsonValue MakeField(const char* name, const char* type, JsonValue defaultValue, bool stored = true) const
    {
        JsonValue field = JsonValue::MakeObject();
        field.Set("name", JsonValue(PersistentKey(name)));
        field.Set("displayName", JsonValue(DisplayName(name)));
        field.Set("type", JsonValue(type));
        if (stored && m_includeDefaults) field.Set("default", std::move(defaultValue));
        field.Set("stored", JsonValue(stored));
        field.Set("role", JsonValue(stored ? "configuration" : "observation"));
        if (!m_group.empty()) field.Set("group", JsonValue(m_group));
        field.Set("visible", JsonValue(FieldVisible()));
        field.Set("enabled", JsonValue(stored && FieldEnabled()));
        field.Set("readOnly", JsonValue(!stored || FieldReadOnly()));
        if (HasFieldMin()) field.Set("minimum", JsonValue(static_cast<double>(FieldMin())));
        if (FieldStep() > 0.0f) field.Set("step", JsonValue(static_cast<double>(FieldStep())));
        if (!FileExtensions().empty())
            field.Set("fileExtensions", JsonValue(FileExtensions()));
        return field;
    }

    void Add(const char* name, const char* type, JsonValue defaultValue)
    {
        CurrentFields().Push(MakeField(name, type, std::move(defaultValue)));
    }

    void AddList(const char* name, const char* elementType, std::size_t count)
    {
        JsonValue field = MakeField(name, "list", JsonValue::MakeArray());
        field.Set("elementType", JsonValue(elementType));
        field.Set("count", JsonValue(static_cast<int>(count)));
        CurrentFields().Push(std::move(field));
    }

    template<typename Number>
    void AddRange(const char* name, const char* type, JsonValue defaultValue, Number min, Number max)
    {
        JsonValue field = MakeField(name, type, std::move(defaultValue));
        JsonValue range = JsonValue::MakeObject();
        range.Set("min", JsonValue(static_cast<double>(min)));
        range.Set("max", JsonValue(static_cast<double>(max)));
        field.Set("range", std::move(range));
        CurrentFields().Push(std::move(field));
    }

    /// 現在スキーマを積む先。入れ子スコープの内側なら構築中の子配列。
    /// 抑制中 (配列の 2 要素目以降) は捨てるためのスクラッチを返す。
    JsonValue& CurrentFields()
    {
        if (m_suppress > 0) return m_discard;
        return m_pending.empty() ? m_fields : m_pending.back().second;
    }

    JsonValue   m_fields = JsonValue::MakeArray();
    std::string m_group;

    /// 構築中の入れ子スキーマ (フィールド名, フィールド配列)
    std::vector<std::pair<std::string, JsonValue>> m_pending;

    /// 配列は先頭要素のスキーマだけを採る。2 要素目以降はここへ捨てる。
    JsonValue m_discard = JsonValue::MakeArray();
    int       m_suppress = 0;

    std::vector<bool>        m_listElementCaptured;
    std::vector<std::size_t> m_listCounts;
    std::vector<bool>        m_captureDepth;
};

/// 目標フィールド名に一致した1フィールドだけを JSON から書き込む。他フィールドは素通しする。
class JsonWriteReflector final : public scene::IReflector {
public:
    JsonWriteReflector(std::string targetField, const JsonValue& value)
        : m_target(std::move(targetField)), m_value(value) {}

    /// 目標フィールドを発見し値を代入できたか。
    bool Applied() const { return m_applied; }
    /// 発見したが型不一致で代入しなかった場合の理由 (空 = なし)。
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
    void AssetField(const char* name,
                    scene::ScriptAssetReference& value,
                    scene::ScriptAssetType) override
    {
        if (!Match(name)) return;
        if (m_value.IsString()) {
            value.SetPath(m_value.AsString());
            m_applied = true;
            return;
        }
        if (!m_value.IsObject()) {
            TypeError("asset path string or {guid,path}");
            return;
        }
        bool suppliedGuid = false;
        if (const JsonValue* guid = m_value.Find("guid"); guid && guid->IsString()) {
            value.guid = guid->AsString();
            suppliedGuid = true;
        }
        if (const JsonValue* path = m_value.Find("path"); path && path->IsString()) {
            if (suppliedGuid) value.path = path->AsString();
            else value.SetPath(path->AsString());
        }
        m_applied = true;
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        if (!Match(name)) return;
        if (!ReadNumberList(values)) TypeError("number array");
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        if (!Match(name)) return;
        if (!ReadIntegerList(values)) TypeError("integer array");
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        if (!Match(name)) return;
        if (!m_value.IsArray()) { TypeError("boolean array"); return; }
        std::vector<bool> result;
        for (const auto& item : m_value.AsArray()) {
            if (!item.IsBool()) { TypeError("boolean array"); return; }
            result.push_back(item.AsBool());
        }
        values = std::move(result);
        m_applied = true;
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        if (!Match(name)) return;
        if (!m_value.IsArray()) { TypeError("string array"); return; }
        std::vector<std::string> result;
        for (const auto& item : m_value.AsArray()) {
            if (!item.IsString()) { TypeError("string array"); return; }
            result.push_back(item.AsString());
        }
        values = std::move(result);
        m_applied = true;
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        if (!Match(name)) return;
        if (!ReadVectorList(values, 2)) TypeError("vector2 array");
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        if (!Match(name)) return;
        if (!ReadVectorList(values, 3)) TypeError("vector3 array");
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        if (!Match(name)) return;
        if (!ReadVectorList(values, 4)) TypeError("vector4 array");
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        if (!Match(name)) return;
        if (!m_value.IsArray()) { TypeError("entity reference array"); return; }
        std::vector<scene::EntityRef> result;
        for (const auto& item : m_value.AsArray()) {
            if (!item.IsArray() || item.AsArray().size() < 2 ||
                !item.AsArray()[0].IsNumber() || !item.AsArray()[1].IsNumber()) {
                TypeError("entity reference array");
                return;
            }
            scene::EntityRef value;
            value.id.index = static_cast<uint32_t>(item.AsArray()[0].AsNumber());
            value.id.generation = static_cast<uint32_t>(item.AsArray()[1].AsNumber());
            result.push_back(value);
        }
        values = std::move(result);
        m_applied = true;
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType) override
    {
        if (!Match(name)) return;
        if (!m_value.IsArray()) { TypeError("asset array"); return; }
        std::vector<scene::ScriptAssetReference> result;
        for (const auto& item : m_value.AsArray()) {
            scene::ScriptAssetReference asset;
            if (item.IsString()) {
                asset.SetPath(item.AsString());
            } else if (item.IsObject()) {
                bool suppliedGuid = false;
                if (const JsonValue* guid = item.Find("guid"); guid && guid->IsString())
                {
                    asset.guid = guid->AsString();
                    suppliedGuid = true;
                }
                if (const JsonValue* path = item.Find("path"); path && path->IsString()) {
                    if (suppliedGuid) asset.path = path->AsString();
                    else asset.SetPath(path->AsString());
                }
            } else {
                TypeError("asset array");
                return;
            }
            result.push_back(std::move(asset));
        }
        values = std::move(result);
        m_applied = true;
    }
    /// ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    /// @name 入れ子オブジェクト / 構造体配列
    /// @{
    /// このリフレクタは「ドット区切りのパスで指定された 1 フィールドだけを書く」設計。スコープに入るたびに m_target の先頭セグメントを削り、抜けるときに戻す。配列要素は `"customEffects.0.intensity"` のように索引をセグメントとして扱う。
    /// @note スコープ対は「同じリフレクタが状態を変えながら潜る」形のため子リフレクタ (ApplyNested) を作れず、代わりに m_target を退避・復元する。
    void BeginObject(const char* name) override
    {
        EnterSegment(PersistentKey(name));
    }

    void EndObject() override
    {
        LeaveSegment();
    }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        EnterSegment(PersistentKey(name));
        /// @note AI からの要素数変更は別コマンドで扱う
        return count;
    }

    void BeginObjectElement(std::size_t index) override
    {
        EnterSegment(std::to_string(index));
    }

    void EndObjectElement() override { LeaveSegment(); }

    std::size_t EndObjectList() override
    {
        LeaveSegment();
        return NO_REMOVE;
    }
    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        const std::string key = PersistentKey(name);
        if (m_target == key && m_value.IsObject()) {
            if (const JsonValue* type = m_value.Find("type"); type && type->IsString())
                value.SetType(type->AsString());
            m_applied = true;
            return;
        }
        if (value.value)
            ApplyNested(name, [&](JsonWriteReflector& child) { value.value->Reflect(child); });
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
    /// @}

private:
    template<typename Callback>
    void ApplyNested(const char* name, Callback callback)
    {
        const std::string prefix = std::string(PersistentKey(name)) + ".";
        if (!m_target.starts_with(prefix)) return;
        JsonWriteReflector child(m_target.substr(prefix.size()), m_value);
        callback(child);
        m_applied = child.m_applied;
        m_error = child.m_error;
    }

    /// スコープに入る。target が `"<segment>."` で始まっていればその分だけ削り、そうでなければ target を空にしてスコープ内の全フィールドをマッチさせない。
    /// @note マッチしない場合も必ずスタックへ積む。BeginObject / EndObject は必ず対で呼ばれるため、積まずに抜けると EndObject でスタックが破綻し、以降のスコープ復元がすべてずれる。
    void EnterSegment(const std::string& segment)
    {
        m_segmentStack.push_back(m_target);

        const std::string prefix = segment + ".";
        if (m_target.starts_with(prefix)) m_target = m_target.substr(prefix.size());
        else                              m_target.clear();
    }

    void LeaveSegment()
    {
        if (m_segmentStack.empty()) return;
        m_target = std::move(m_segmentStack.back());
        m_segmentStack.pop_back();
    }

    std::vector<std::string> m_segmentStack;

    bool ReadNumberList(std::vector<float>& values)
    {
        if (!m_value.IsArray()) return false;
        std::vector<float> result;
        for (const auto& item : m_value.AsArray()) {
            if (!item.IsNumber()) return false;
            result.push_back(static_cast<float>(item.AsNumber()));
        }
        values = std::move(result);
        m_applied = true;
        return true;
    }

    bool ReadIntegerList(std::vector<int>& values)
    {
        if (!m_value.IsArray()) return false;
        std::vector<int> result;
        for (const auto& item : m_value.AsArray()) {
            if (!item.IsNumber()) return false;
            result.push_back(item.AsInt());
        }
        values = std::move(result);
        m_applied = true;
        return true;
    }

    bool ReadVectorList(std::vector<math::Vector2>& values, int)
    {
        if (!m_value.IsArray()) return false;
        std::vector<math::Vector2> result;
        for (const auto& item : m_value.AsArray()) {
            float components[4];
            if (!ReadNumbersFrom(item, 2, components)) return false;
            result.push_back({ components[0], components[1] });
        }
        values = std::move(result);
        m_applied = true;
        return true;
    }

    bool ReadVectorList(std::vector<math::Vector3>& values, int)
    {
        if (!m_value.IsArray()) return false;
        std::vector<math::Vector3> result;
        for (const auto& item : m_value.AsArray()) {
            float components[4];
            if (!ReadNumbersFrom(item, 3, components)) return false;
            result.push_back({ components[0], components[1], components[2] });
        }
        values = std::move(result);
        m_applied = true;
        return true;
    }

    bool ReadVectorList(std::vector<math::Vector4>& values, int)
    {
        if (!m_value.IsArray()) return false;
        std::vector<math::Vector4> result;
        for (const auto& item : m_value.AsArray()) {
            float components[4];
            if (!ReadNumbersFrom(item, 4, components)) return false;
            result.push_back({ components[0], components[1], components[2], components[3] });
        }
        values = std::move(result);
        m_applied = true;
        return true;
    }

    static bool ReadNumbersFrom(const JsonValue& value, int count, float out[4])
    {
        if (!value.IsArray() || static_cast<int>(value.AsArray().size()) < count)
            return false;
        for (int i = 0; i < count; ++i) {
            const JsonValue& element = value.AsArray()[static_cast<std::size_t>(i)];
            if (!element.IsNumber()) return false;
            out[i] = static_cast<float>(element.AsNumber());
        }
        return true;
    }

    bool Match(const char* name)
    {
        if (m_applied || !m_error.empty()) return false;
        if (m_target == PersistentKey(name)) {
            if (FieldReadOnly()) {
                m_error = "field '" + m_target + "' is read-only";
                return false;
            }
            return true;
        }
        return false;
    }

    /// JsonValue 配列の先頭 count 要素を数値として out[0..count) へ読む。要素不足・非数値なら false。
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

} /// namespace fbzz::editor::ai
