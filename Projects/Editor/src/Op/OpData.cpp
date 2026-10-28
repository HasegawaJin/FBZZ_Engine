/// @file    OpData.cpp
/// @brief   Operator の構造化設定を AI 層へ依存せず JSON に変換する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/Op/OpData.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>

namespace fbzz::editor {
namespace {

/// @see https://www.rfc-editor.org/rfc/rfc8259#section-7 JSON strings の制御文字と引用符。
void AppendString(std::string_view value, std::string& output)
{
    constexpr char HEX[] = "0123456789abcdef";
    output += '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output += '\\';
            output += static_cast<char>(character);
        } else if (character < 0x20) {
            output += "\\u00";
            output += HEX[character >> 4];
            output += HEX[character & 15];
        } else {
            output += static_cast<char>(character);
        }
    }
    output += '"';
}

bool AppendNumber(double value, std::string& output)
{
    if (!std::isfinite(value)) return false;
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) return false;
    output.append(buffer.data(), result.ptr);
    return true;
}

bool AppendValue(const OpData& value, std::string& output, std::size_t depth)
{
    if (depth > 32) return false;
    switch (value.GetType()) {
    case OpData::Type::Null: output += "null"; return true;
    case OpData::Type::Bool: output += value.AsBool() ? "true" : "false"; return true;
    case OpData::Type::Int: output += std::to_string(value.AsInt64()); return true;
    case OpData::Type::Float: return AppendNumber(value.AsDouble(), output);
    case OpData::Type::String: AppendString(value.AsString(), output); return true;
    case OpData::Type::Vec3: {
        const math::Vector3 vector = value.AsVec3();
        output += '[';
        if (!AppendNumber(vector.x, output)) return false;
        output += ',';
        if (!AppendNumber(vector.y, output)) return false;
        output += ',';
        if (!AppendNumber(vector.z, output)) return false;
        output += ']';
        return true;
    }
    case OpData::Type::Array: {
        output += '[';
        bool first = true;
        for (const OpData& item : value.AsArray()) {
            if (!first) output += ',';
            first = false;
            if (!AppendValue(item, output, depth + 1)) return false;
        }
        output += ']';
        return true;
    }
    case OpData::Type::Object: {
        /// @note 同一の要求はキーの入力順に依存しない文字列表現へ揃える。
        std::vector<const OpData::Member*> members;
        for (const OpData::Member& member : value.AsObject()) members.push_back(&member);
        std::sort(members.begin(), members.end(), [](const auto* left, const auto* right) {
            return left->first < right->first;
        });
        output += '{';
        bool first = true;
        for (const OpData::Member* member : members) {
            if (!first) output += ',';
            first = false;
            AppendString(member->first, output);
            output += ':';
            if (!AppendValue(member->second, output, depth + 1)) return false;
        }
        output += '}';
        return true;
    }
    }
    return false;
}

} /// @note namespace

bool WriteOpDataJson(const OpData& value, std::string& output)
{
    std::string result;
    if (!AppendValue(value, result, 0)) return false;
    output = std::move(result);
    return true;
}

} /// @note namespace fbzz::editor
