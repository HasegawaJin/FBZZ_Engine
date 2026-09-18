/// @file    Json.hpp
/// @brief   AI 連携 (MCP) の wire フォーマット用に、依存を増やさない最小 JSON 値・パーサ・シリアライザを自作する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note シリアライズは toml++ 一本の方針だが、Editor Command Bus は Claude/MCP と NDJSON でやり取りするため
///       RFC 8259 サブセットの自作 JSON コーデックを持つ。エラーは throw せず optional / bool で返す。
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

    /// 挿入順を保つため Object は map ではなく pair の vector で持つ (出力の安定性・小規模前提)。
    using Array  = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool value) : m_type(Type::Bool), m_bool(value) {}
    JsonValue(int value) : m_type(Type::Number), m_number(static_cast<double>(value)) {}
    JsonValue(std::int64_t value) : m_type(Type::Number), m_number(static_cast<double>(value)) {}
    JsonValue(double value) : m_type(Type::Number), m_number(value) {}
    JsonValue(const char* value) : m_type(Type::String), m_string(value) {}
    JsonValue(std::string value) : m_type(Type::String), m_string(std::move(value)) {}

    static JsonValue MakeArray()  { JsonValue v; v.m_type = Type::Array;  return v; }
    static JsonValue MakeObject() { JsonValue v; v.m_type = Type::Object; return v; }

    Type GetType() const { return m_type; }
    bool IsNull()   const { return m_type == Type::Null; }
    bool IsBool()   const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray()  const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    /// 型が一致しない場合は既定値を返す (throw しない)。呼び出し側は Is*() で確認してから使う想定。
    bool               AsBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    double             AsNumber(double fallback = 0.0) const { return m_type == Type::Number ? m_number : fallback; }
    int                AsInt(int fallback = 0) const { return m_type == Type::Number ? static_cast<int>(m_number) : fallback; }
    const std::string& AsString() const { return m_string; }

    Array&        AsArray()  { return m_array; }
    const Array&  AsArray()  const { return m_array; }
    Object&       AsObject() { return m_object; }
    const Object& AsObject() const { return m_object; }

    /// Array 構築ヘルパ。
    void Push(JsonValue value) { m_type = Type::Array; m_array.push_back(std::move(value)); }

    /// Object 構築ヘルパ (同名キーは上書きせず追記しない — 既存を更新する)。
    void Set(std::string key, JsonValue value)
    {
        m_type = Type::Object;
        for (auto& member : m_object) {
            if (member.first == key) { member.second = std::move(value); return; }
        }
        m_object.emplace_back(std::move(key), std::move(value));
    }

    /// Object の値を検索する。存在しなければ nullptr。
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
    std::string m_string;
    Array       m_array;
    Object      m_object;
};

/// text を JSON としてパースする。失敗時は nullopt を返し、error != nullptr なら理由を書き込む。
std::optional<JsonValue> ParseJson(std::string_view text, std::string* error = nullptr);

/// JsonValue をコンパクト (改行なし) にシリアライズする。NDJSON の1行としてそのまま送れる。
std::string SerializeJson(const JsonValue& value);

/// バイト列を標準 base64 (パディングあり) へ変換する。PNG を JSON 文字列へ埋め込むのに使う。
std::string Base64Encode(const std::uint8_t* data, std::size_t size);
std::string Base64Encode(const std::vector<std::uint8_t>& data);

} // namespace fbzz::editor::ai
