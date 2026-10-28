/// @file    ManagedFieldCodec.hpp
/// @brief   managed 配列の値バッファと既存 TOML の相互変換。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include "ManagedPacketRead.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <toml++/toml.hpp>

namespace fbzz::scene::managed_detail {

inline constexpr uint32_t kMaximumArrayElements = std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t kMaximumStructuredElements = std::numeric_limits<uint32_t>::max();
static_assert(std::endian::native == std::endian::little);

template<typename T>
void Append(std::string& bytes, T value)
{
    const auto bits = std::bit_cast<std::array<char, sizeof(T)>>(value);
    bytes.append(bits.data(), bits.size());
}

template<typename T>
bool Read(std::string_view bytes, size_t& offset, T& value)
{
    return ReadManagedValue(bytes, offset, value);
}

inline bool IsArray(ManagedValueKind kind)
{
    return kind >= ManagedValueKind::FLOAT_ARRAY && kind <= ManagedValueKind::GAME_OBJECT_ARRAY;
}

/// @note JSON 内の型付き GO token だけを抽出する。普通の文字列中の GUID は参照として扱わない。
struct StructuredValue final : IScriptSerializable {
    std::string json;
    std::vector<std::string> objectGuids;
    std::vector<std::string> componentOwnerGuids;
    /// @note 表示だけに使う寿命検証済み参照。保存時は既存 objectGuids の GUID 列のみ出力する。
    std::vector<ScriptObjectReference> objectReferences;

    explicit StructuredValue(std::string value) : json(std::move(value))
    {
        m_valid = Extract(json, objectGuids) && ExtractOwners(json, componentOwnerGuids);
        if (m_valid) for (const auto& guid : objectGuids) objectReferences.push_back({guid, EntityID::INVALID});
    }
    bool Valid() const { return m_valid; }
    void Reflect(IReflector& reflector) override
    {
        const auto previous = json;
        reflector.BeginField("json", "json");
        reflector.Field("json", json);
        reflector.EndField();
        if (previous != json) {
            objectGuids.clear(); objectReferences.clear(); componentOwnerGuids.clear();
            m_valid = Extract(json, objectGuids) && ExtractOwners(json, componentOwnerGuids);
            if (m_valid) for (const auto& guid : objectGuids) objectReferences.push_back({guid, EntityID::INVALID});
        }
        if (m_valid) {
            reflector.BeginField("objectGuids", "objectGuids");
            reflector.ObjectReferenceListField("objectGuids", objectReferences);
            reflector.EndField();
            objectGuids.clear();
            for (const auto& reference : objectReferences) objectGuids.push_back(reference.guid);
            reflector.BeginField("componentOwnerGuids", "componentOwnerGuids");
            reflector.ListField("componentOwnerGuids", componentOwnerGuids);
            reflector.EndField();
        }
    }
    /// @note Prefab の既存 TOML GUID remap が一覧を更新した後で、同じ token 順に JSON を再接続する。
    bool ApplyReferences()
    {
        std::vector<std::string> previous;
        if (!m_valid || !Extract(json, previous) || previous.size() != objectGuids.size()) return false;
        std::string output;
        size_t offset = 0;
        constexpr std::string_view prefix = "{\"$go\":\"";
        for (const auto& guid : objectGuids) {
            if (!ValidGuid(guid)) return false;
            const size_t start = json.find(prefix, offset);
            if (start == std::string::npos) return false;
            const size_t end = json.find("\"}", start + prefix.size());
            output.append(json, offset, start + prefix.size() - offset);
            output.append(guid);
            output.append("\"}");
            offset = end + 2;
        }
        output.append(json, offset, std::string::npos);
        json = std::move(output);
        std::vector<std::string> owners;
        std::vector<std::pair<size_t, size_t>> slots;
        if (!ExtractOwners(json, owners, &slots) || owners.size() != componentOwnerGuids.size()) return false;
        /// @note 後方から差し替え、可変長 GUID でも先行 slot の位置を壊さない。
        for (size_t index = slots.size(); index > 0; --index) {
            if (!ValidGuid(componentOwnerGuids[index - 1])) return false;
            const auto [start, end] = slots[index - 1];
            json.replace(start, end - start, componentOwnerGuids[index - 1]);
        }
        return true;
    }
private:
    static bool ValidGuid(std::string_view guid)
    {
        if (guid.empty() || guid.size() > 4096) return false;
        return std::all_of(guid.begin(), guid.end(), [](char value) {
            return value >= 32 && value <= 126 && value != '"' && value != '\\';
        });
    }
    static bool Extract(std::string_view json, std::vector<std::string>& guids)
    {
        constexpr std::string_view prefix = "{\"$go\":\"";
        size_t offset = 0;
        while ((offset = json.find(prefix, offset)) != std::string_view::npos) {
            offset += prefix.size();
            const size_t end = json.find("\"}", offset);
            if (end == std::string_view::npos || !ValidGuid(json.substr(offset, end - offset))) return false;
            guids.emplace_back(json.substr(offset, end - offset));
            offset = end + 2;
        }
        return true;
    }
    /// @note Host の canonical $component token だけを扱い、同名の普通の文字列を remap しない。
    static bool ExtractOwners(std::string_view json, std::vector<std::string>& guids, std::vector<std::pair<size_t, size_t>>* slots = nullptr)
    {
        constexpr std::string_view prefix = "{\"$component\":{\"kind\":\"";
        constexpr std::string_view owner = "\",\"ownerGuid\":\"";
        size_t offset = 0;
        while ((offset = json.find(prefix, offset)) != std::string_view::npos) {
            const size_t kindStart = offset + prefix.size(), kindEnd = json.find(owner, kindStart);
            if (kindEnd == std::string_view::npos) return false;
            const auto kind = json.substr(kindStart, kindEnd - kindStart);
            if (kind != "native" && kind != "script") return false;
            const size_t start = kindEnd + owner.size(), end = json.find('"', start);
            if (end == std::string_view::npos || !ValidGuid(json.substr(start, end - start))) return false;
            guids.emplace_back(json.substr(start, end - start));
            if (slots) slots->emplace_back(start, end);
            offset = end + 1;
        }
        return true;
    }
    bool m_valid = false;
};

template<typename T>
bool DecodeArray(std::string_view bytes, std::vector<T>& values, uint32_t maximumElements = kMaximumArrayElements)
{
    size_t offset = 0;
    uint32_t count = 0;
    if (!Read(bytes, offset, count) || count > maximumElements) return false;
    constexpr size_t minimumBytes = std::is_same_v<T, bool> ? 1 : std::is_same_v<T, math::Vector3> ? 12 : sizeof(uint32_t);
    if (static_cast<uint64_t>(count) * minimumBytes > bytes.size() - offset) return false;
    std::vector<T> decoded;
    decoded.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        T value{};
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, ScriptObjectReference>) {
            uint32_t length = 0;
            if (!Read(bytes, offset, length)) return false;
            if (length == std::numeric_limits<uint32_t>::max()) {
                if constexpr (std::is_same_v<T, std::string>) return false;
            } else {
                if (offset > bytes.size() || length > bytes.size() - offset) return false;
                if constexpr (std::is_same_v<T, std::string>) value.assign(bytes.substr(offset, length));
                else value.guid.assign(bytes.substr(offset, length));
                offset += length;
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            uint8_t number = 0;
            if (!Read(bytes, offset, number) || number > 1) return false;
            value = number != 0;
        } else if constexpr (std::is_same_v<T, math::Vector3>) {
            if (!Read(bytes, offset, value.x) || !Read(bytes, offset, value.y) || !Read(bytes, offset, value.z)
                || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
        } else {
            if (!Read(bytes, offset, value)) return false;
            if constexpr (std::is_same_v<T, float>) if (!std::isfinite(value)) return false;
        }
        decoded.push_back(std::move(value));
    }
    if (offset != bytes.size()) return false;
    values = std::move(decoded);
    return true;
}

template<typename T>
std::string EncodeArray(const std::vector<T>& values)
{
    std::string bytes;
    Append(bytes, static_cast<uint32_t>(values.size()));
    for (size_t index = 0; index < values.size(); ++index) {
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, ScriptObjectReference>) {
            const std::string& text = [&]() -> const std::string& {
                if constexpr (std::is_same_v<T, std::string>) return values[index];
                else return values[index].guid;
            }();
            if constexpr (std::is_same_v<T, ScriptObjectReference>) {
                if (text.empty()) { Append(bytes, std::numeric_limits<uint32_t>::max()); continue; }
            }
            Append(bytes, static_cast<uint32_t>(text.size()));
            bytes.append(text);
        } else if constexpr (std::is_same_v<T, bool>) {
            Append(bytes, static_cast<uint8_t>(values[index] ? 1 : 0));
        } else if constexpr (std::is_same_v<T, math::Vector3>) {
            Append(bytes, values[index].x);
            Append(bytes, values[index].y);
            Append(bytes, values[index].z);
        } else Append(bytes, values[index]);
    }
    return bytes;
}

inline bool FloatFromToml(const toml::node& node, float& result)
{
    const auto value = node.value<double>();
    if (!value || !std::isfinite(*value) || std::abs(*value) > std::numeric_limits<float>::max()) return false;
    result = static_cast<float>(*value);
    return true;
}

inline bool VectorFromToml(const toml::node& node, math::Vector3& result)
{
    const auto* array = node.as_array();
    return array && array->size() == 3 && FloatFromToml((*array)[0], result.x)
        && FloatFromToml((*array)[1], result.y) && FloatFromToml((*array)[2], result.z);
}

template<typename T>
bool ArrayFromToml(const toml::node& node, std::string& bytes)
{
    const auto* array = node.as_array();
    if (!array || array->size() > kMaximumArrayElements) return false;
    std::vector<T> values;
    values.reserve(array->size());
    for (const auto& element : *array) {
        T value{};
        if constexpr (std::is_same_v<T, float>) {
            if (!FloatFromToml(element, value)) return false;
        } else if constexpr (std::is_same_v<T, int>) {
            const auto number = element.value<int64_t>();
            if (!number || *number < std::numeric_limits<int32_t>::min() || *number > std::numeric_limits<int32_t>::max()) return false;
            value = static_cast<int>(*number);
        } else if constexpr (std::is_same_v<T, bool>) {
            const auto number = element.value<bool>();
            if (!number) return false;
            value = *number;
        } else if constexpr (std::is_same_v<T, math::Vector3>) {
            if (!VectorFromToml(element, value)) return false;
        } else {
            const auto text = element.value<std::string>();
            if (!text) return false;
            if constexpr (std::is_same_v<T, std::string>) value = *text;
            else value.guid = *text;
        }
        values.push_back(std::move(value));
    }
    bytes = EncodeArray(values);
    return true;
}

inline bool ReadSavedField(const toml::node& node, ManagedFieldValue& field)
{
    switch (field.value.valueKind) {
    case ManagedValueKind::FLOAT: return FloatFromToml(node, field.value.x);
    case ManagedValueKind::INT: {
        const auto value = node.value<int64_t>();
        if (!value || *value < std::numeric_limits<int32_t>::min() || *value > std::numeric_limits<int32_t>::max()) return false;
        field.value.integer = *value;
        return true;
    }
    case ManagedValueKind::BOOL: {
        const auto value = node.value<bool>();
        if (!value) return false;
        field.value.integer = *value ? 1 : 0;
        return true;
    }
    case ManagedValueKind::STRING:
    case ManagedValueKind::ASSET_REFERENCE:
    case ManagedValueKind::GAME_OBJECT: {
        const auto value = node.value<std::string>();
        if (!value) return false;
        field.text = *value;
        if (field.value.valueKind == ManagedValueKind::GAME_OBJECT && value->empty()) field.value.flags |= kManagedNullValue;
        return true;
    }
    case ManagedValueKind::VECTOR3: {
        math::Vector3 value;
        if (!VectorFromToml(node, value)) return false;
        field.value.x = value.x;
        field.value.y = value.y;
        field.value.z = value.z;
        return true;
    }
    case ManagedValueKind::QUATERNION: {
        const auto* values = node.as_array();
        return values && values->size() == 4 && FloatFromToml((*values)[0], field.value.x)
            && FloatFromToml((*values)[1], field.value.y) && FloatFromToml((*values)[2], field.value.z)
            && FloatFromToml((*values)[3], field.value.w);
    }
    case ManagedValueKind::STRUCTURED: {
        const auto* table = node.as_table();
        if (!table) return false;
        const auto json = (*table)["json"].value<std::string>();
        const auto* references = (*table)["objectGuids"].as_array();
            if (!json || !references) return false;
        StructuredValue value{*json};
        if (!value.Valid()) return false;
        value.objectGuids.clear();
        if (const auto* owners = (*table)["componentOwnerGuids"].as_array()) {
            value.componentOwnerGuids.clear();
            for (const auto& owner : *owners) {
                const auto guid = owner.value<std::string>(); if (!guid) return false;
                value.componentOwnerGuids.push_back(*guid);
            }
        }
        for (const auto& reference : *references) {
            const auto guid = reference.value<std::string>();
            if (!guid) return false;
            value.objectGuids.push_back(*guid);
        }
        if (!value.ApplyReferences()) return false;
        field.text = std::move(value.json);
        return true;
    }
    case ManagedValueKind::FLOAT_ARRAY: return ArrayFromToml<float>(node, field.text);
    case ManagedValueKind::INT_ARRAY: return ArrayFromToml<int>(node, field.text);
    case ManagedValueKind::BOOL_ARRAY: return ArrayFromToml<bool>(node, field.text);
    case ManagedValueKind::STRING_ARRAY: return ArrayFromToml<std::string>(node, field.text);
    case ManagedValueKind::VECTOR3_ARRAY: return ArrayFromToml<math::Vector3>(node, field.text);
    case ManagedValueKind::GAME_OBJECT_ARRAY: return ArrayFromToml<ScriptObjectReference>(node, field.text);
    default: return false;
    }
}

} /// @note namespace fbzz::scene::managed_detail
