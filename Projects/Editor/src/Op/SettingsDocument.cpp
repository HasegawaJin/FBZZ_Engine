/// @file    SettingsDocument.cpp
/// @brief   設定ドキュメントの型を保持した部分変更。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include "SettingsDocument.hpp"
#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace fbzz::editor::settingsdocument {
namespace {
OpData Read(const toml::node& node)
{
    if (const auto* value = node.as_boolean()) return OpData(value->get());
    if (const auto* value = node.as_integer()) return OpData(value->get());
    if (const auto* value = node.as_floating_point()) return OpData(value->get());
    if (const auto* value = node.as_string()) return OpData(value->get());
    if (const auto* table = node.as_table()) {
        OpData data = OpData::MakeObject();
        for (const auto& [key, value] : *table) data.Set(std::string(key.str()), Read(value));
        return data;
    }
    if (const auto* array = node.as_array()) {
        OpData data = OpData::MakeArray();
        for (const auto& value : *array) data.Push(Read(value));
        return data;
    }
    return {};
}

bool Set(toml::table& table, const std::string& key, const OpData& data, bool existing);

bool Append(toml::array& array, const OpData& data)
{
    if (data.IsBool()) array.push_back(data.AsBool());
    else if (data.IsInt()) array.push_back(data.AsInt64());
    else if (data.IsFloat() && std::isfinite(data.AsDouble())) array.push_back(data.AsDouble());
    else if (data.IsString()) array.push_back(data.AsString());
    else if (data.IsArray()) {
        toml::array nested;
        for (const auto& value : data.AsArray()) if (!Append(nested, value)) return false;
        array.push_back(std::move(nested));
    } else if (data.IsObject()) {
        toml::table nested;
        for (const auto& [key, value] : data.AsObject()) if (!Set(nested, key, value, false)) return false;
        array.push_back(std::move(nested));
    } else return false;
    return true;
}

bool Set(toml::table& table, const std::string& key, const OpData& data, bool existing)
{
    const auto* original = table.get(key);
    /// @note 空配列など保存時に省略される既知項目も受け、型付きローダー後のPreservesで未知キーを拒否する。
    if (data.IsObject()) {
        if (original && !original->is_table()) return false;
        toml::table nested = original ? *original->as_table() : toml::table{};
        for (const auto& [name, value] : data.AsObject()) if (!Set(nested, name, value, existing)) return false;
        table.insert_or_assign(key, std::move(nested));
    } else if (data.IsArray()) {
        if (original && !original->is_array()) return false;
        toml::array array;
        for (const auto& value : data.AsArray()) if (!Append(array, value)) return false;
        table.insert_or_assign(key, std::move(array));
    } else if (data.IsBool()) {
        if (original && !original->is_boolean()) return false;
        table.insert_or_assign(key, data.AsBool());
    } else if (data.IsString()) {
        if (original && !original->is_string()) return false;
        table.insert_or_assign(key, data.AsString());
    } else if (data.IsInt()) {
        if (original && !original->is_number()) return false;
        table.insert_or_assign(key, data.AsInt64());
    } else if (data.IsFloat() && std::isfinite(data.AsDouble())) {
        if (original && !original->is_number()) return false;
        table.insert_or_assign(key, data.AsDouble());
    } else return false;
    return true;
}

bool Contains(const OpData& actual, const OpData& expected)
{
    if (expected.IsObject()) {
        if (!actual.IsObject()) return false;
        for (const auto& [key, value] : expected.AsObject()) {
            const auto* child = actual.Find(key);
            if (!child || !Contains(*child, value)) return false;
        }
        return true;
    }
    if (expected.IsArray()) {
        if (!actual.IsArray() || actual.AsArray().size() != expected.AsArray().size()) return false;
        for (std::size_t i = 0; i < expected.AsArray().size(); ++i)
            if (!Contains(actual.AsArray()[i], expected.AsArray()[i])) return false;
        return true;
    }
    if ((expected.IsFloat() || expected.IsInt()) && (actual.IsFloat() || actual.IsInt())) {
        if (expected.IsInt() && actual.IsInt()) return expected.AsInt64() == actual.AsInt64();
        const double a = actual.IsInt() ? static_cast<double>(actual.AsInt64()) : actual.AsDouble();
        const double b = expected.IsInt() ? static_cast<double>(expected.AsInt64()) : expected.AsDouble();
        return std::isfinite(a) && std::isfinite(b)
            && std::abs(a - b) <= std::numeric_limits<float>::epsilon() * std::max(1.0, std::abs(b));
    }
    if (expected.GetType() != actual.GetType()) return false;
    if (expected.IsBool()) return actual.AsBool() == expected.AsBool();
    if (expected.IsString()) return actual.AsString() == expected.AsString();
    return false;
}
} /// @note namespace

OpData Inspect(const std::string& text)
{
    const auto parsed = toml::parse(text);
    return parsed ? Read(parsed.table()) : OpData{};
}

bool Patch(const std::string& text, const OpData& patch, std::string& output)
{
    if (!patch.IsObject()) return false;
    auto parsed = toml::parse(text);
    if (!parsed) return false;
    for (const auto& [key, value] : patch.AsObject())
        if (!Set(parsed.table(), key, value, true)) return false;
    std::ostringstream stream;
    stream << parsed.table();
    output = stream.str();
    return true;
}

bool Preserves(const std::string& text, const OpData& patch)
{
    return Contains(Inspect(text), patch);
}
} /// @note namespace fbzz::editor::settingsdocument
