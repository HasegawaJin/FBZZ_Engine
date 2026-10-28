/// @file    Json.hpp
/// @brief   AI 連携 (MCP) の wire フォーマット用に、依存を増やさない最小 JSON 値・パーサ・シリアライザを自作する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note シリアライズは toml++ 一本の方針だが、Editor Command Bus は Claude/MCP と NDJSON でやり取りするため RFC 8259 サブセットの自作 JSON コーデックを持つ。エラーは throw せず optional / bool で返す。
/// @note JsonValue はタグ + 全フィールド方式 (union / recursive-variant を避け、自己参照 Array/Object を素直に表現する)。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor::ai {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    /// @brief Object members retain insertion order for stable output.
    using Array  = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool value) : m_type(Type::Bool), m_bool(value) {}
    JsonValue(int value) : JsonValue(static_cast<std::int64_t>(value)) {}
    JsonValue(std::uint32_t value) : JsonValue(static_cast<std::int64_t>(value)) {}
    JsonValue(std::int64_t value) : m_type(Type::Number), m_integer(value), m_isInteger(true) {}
    JsonValue(double value) : m_type(Type::Number), m_number(value) {}
    JsonValue(const char* value) : m_type(Type::String), m_string(value) {}
    JsonValue(std::string value) : m_type(Type::String), m_string(std::move(value)) {}

    static JsonValue MakeArray()  { JsonValue v; v.m_type = Type::Array;  return v; }
    static JsonValue MakeObject() { JsonValue v; v.m_type = Type::Object; return v; }

    Type GetType() const { return m_type; }
    bool IsNull()   const { return m_type == Type::Null; }
    bool IsBool()   const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    /// @brief Parser または整数 constructor が保持した整数値を判別する。
    bool IsInteger() const { return m_type == Type::Number && m_isInteger; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray()  const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    /// @brief 型や範囲が合わない accessor は既定値を返す。
    bool               AsBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    double AsNumber(double fallback = 0.0) const
    {
        return m_type == Type::Number ? (m_isInteger ? static_cast<double>(m_integer) : m_number) : fallback;
    }
    /// @brief int64 値、または正確な整数 double を返す。
    std::int64_t AsInt64(std::int64_t fallback = 0) const
    {
        std::int64_t value = 0;
        return TryGetInt64(value) ? value : fallback;
    }
    int AsInt(int fallback = 0) const
    {
        int value = 0;
        return TryGetInt32(value) ? value : fallback;
    }
    /// @brief int64 範囲に入る整数だけを output へ書く。
    bool TryGetInt64(std::int64_t& output) const;
    /// @brief 32-bit int に収まる整数だけを output へ書く。
    bool TryGetInt32(int& output) const;
    const std::string& AsString() const { return m_string; }

    Array&        AsArray()  { return m_array; }
    const Array&  AsArray()  const { return m_array; }
    Object&       AsObject() { return m_object; }
    const Object& AsObject() const { return m_object; }

    /// @brief 配列へ値を追加する。
    void Push(JsonValue value) { m_type = Type::Array; m_array.push_back(std::move(value)); }

    /// @brief 同名キーを更新し、それ以外は object の末尾へ追加する。
    void Set(std::string key, JsonValue value)
    {
        m_type = Type::Object;
        for (auto& member : m_object) {
            if (member.first == key) { member.second = std::move(value); return; }
        }
        m_object.emplace_back(std::move(key), std::move(value));
    }

    /// @brief 指定キーを検索し、存在しない場合は nullptr を返す。
    const JsonValue* Find(std::string_view key) const
    {
        if (m_type != Type::Object) return nullptr;
        for (const auto& member : m_object) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }

private:
    Type        m_type = Type::Null;
    bool        m_bool = false;
    double      m_number = 0.0;
    std::int64_t m_integer = 0;
    bool        m_isInteger = false;
    std::string m_string;
    Array       m_array;
    Object      m_object;
};

/// @brief JSON text をパースし、失敗時は nullopt と診断を返す。
std::optional<JsonValue> ParseJson(std::string_view text, std::string* error = nullptr);

/// @brief JsonValue をコンパクトな JSON text へシリアライズする。
std::string SerializeJson(const JsonValue& value);

/// @brief バイト列を padding 付き標準 base64 へ変換する。
std::string Base64Encode(const std::uint8_t* data, std::size_t size);
std::string Base64Encode(const std::vector<std::uint8_t>& data);

} /// @note namespace fbzz::editor::ai
