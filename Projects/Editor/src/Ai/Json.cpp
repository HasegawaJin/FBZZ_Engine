/// @file    Json.cpp
/// @brief   最小 JSON パーサ / シリアライザ / base64 の実装。再帰下降で RFC 8259 のサブセットを扱う。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/Json.hpp>

#include <array>
#include <charconv>
#include <cstdio>
#include <limits>

namespace fbzz::editor::ai {

namespace {

/// 再帰下降パーサ。std::string_view 上を index で走査し、throw せず失敗を bool で伝播する。
class Parser {
public:
    Parser(std::string_view text, std::string* error) : m_text(text), m_error(error) {}

    bool Parse(JsonValue& out)
    {
        SkipWhitespace();
        if (!ParseValue(out)) return false;
        SkipWhitespace();
        /// @note 末尾に余分なトークンがあれば不正な JSON とみなす。
        if (m_pos != m_text.size()) return Fail("末尾に余分な文字があります");
        return true;
    }

private:
    std::string_view m_text;
    std::size_t      m_pos = 0;
    std::string*     m_error = nullptr;

    bool Fail(const char* message)
    {
        if (m_error && m_error->empty()) {
            *m_error = std::string(message) + " (offset " + std::to_string(m_pos) + ")";
        }
        return false;
    }

    bool AtEnd() const { return m_pos >= m_text.size(); }
    char Peek() const { return m_text[m_pos]; }

    void SkipWhitespace()
    {
        while (!AtEnd()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_pos;
            else break;
        }
    }

    bool ParseValue(JsonValue& out)
    {
        if (AtEnd()) return Fail("値がありません");
        switch (Peek()) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string s;
                if (!ParseString(s)) return false;
                out = JsonValue(std::move(s));
                return true;
            }
            case 't': case 'f': return ParseBool(out);
            case 'n': return ParseNull(out);
            default:  return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out)
    {
        out = JsonValue::MakeObject();
        /// @note '{'
        ++m_pos;
        SkipWhitespace();
        if (!AtEnd() && Peek() == '}') { ++m_pos; return true; }
        for (;;) {
            SkipWhitespace();
            if (AtEnd() || Peek() != '"') return Fail("オブジェクトキーが必要です");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (AtEnd() || Peek() != ':') return Fail("':' が必要です");
            ++m_pos;
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value)) return false;
            out.Set(std::move(key), std::move(value));
            SkipWhitespace();
            if (AtEnd()) return Fail("オブジェクトが閉じていません");
            if (Peek() == ',') { ++m_pos; continue; }
            if (Peek() == '}') { ++m_pos; return true; }
            return Fail("',' か '}' が必要です");
        }
    }

    bool ParseArray(JsonValue& out)
    {
        out = JsonValue::MakeArray();
        /// @note '['
        ++m_pos;
        SkipWhitespace();
        if (!AtEnd() && Peek() == ']') { ++m_pos; return true; }
        for (;;) {
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value)) return false;
            out.Push(std::move(value));
            SkipWhitespace();
            if (AtEnd()) return Fail("配列が閉じていません");
            if (Peek() == ',') { ++m_pos; continue; }
            if (Peek() == ']') { ++m_pos; return true; }
            return Fail("',' か ']' が必要です");
        }
    }

    bool ParseString(std::string& out)
    {
        /// @note 開き '"'
        ++m_pos;
        out.clear();
        while (!AtEnd()) {
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (AtEnd()) return Fail("エスケープが途中で終わっています");
                const char esc = m_text[m_pos++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u':  if (!ParseUnicodeEscape(out)) return false; break;
                    default:   return Fail("不正なエスケープです");
                }
            } else {
                out.push_back(c);
            }
        }
        return Fail("文字列が閉じていません");
    }

    /// \uXXXX (必要ならサロゲートペア) を読み UTF-8 へ変換して out へ追記する。
    bool ParseUnicodeEscape(std::string& out)
    {
        std::uint32_t code = 0;
        if (!ReadHex4(code)) return false;
        /// @note 上位サロゲートなら続く \uXXXX と結合する。
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (m_pos + 1 >= m_text.size() || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u') {
                return Fail("サロゲートペアが不完全です");
            }
            m_pos += 2;
            std::uint32_t low = 0;
            if (!ReadHex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("下位サロゲートが不正です");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        AppendUtf8(out, code);
        return true;
    }

    bool ReadHex4(std::uint32_t& out)
    {
        if (m_pos + 4 > m_text.size()) return Fail("\\u の桁が足りません");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return Fail("16進数ではありません");
        }
        out = value;
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t code)
    {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseBool(JsonValue& out)
    {
        if (m_text.compare(m_pos, 4, "true") == 0)  { m_pos += 4; out = JsonValue(true);  return true; }
        if (m_text.compare(m_pos, 5, "false") == 0) { m_pos += 5; out = JsonValue(false); return true; }
        return Fail("true / false ではありません");
    }

    bool ParseNull(JsonValue& out)
    {
        if (m_text.compare(m_pos, 4, "null") == 0) { m_pos += 4; out = JsonValue(nullptr); return true; }
        return Fail("null ではありません");
    }

    bool ParseNumber(JsonValue& out)
    {
        const std::size_t start = m_pos;
        if (!AtEnd() && Peek() == '-') ++m_pos;
        while (!AtEnd()) {
            const char c = Peek();
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++m_pos;
            else break;
        }
        if (m_pos == start) return Fail("数値ではありません");
        double value = 0.0;
        const char* first = m_text.data() + start;
        const char* last  = m_text.data() + m_pos;
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last) return Fail("数値の解析に失敗しました");
        out = JsonValue(value);
        return true;
    }
};

void SerializeString(const std::string& s, std::string& out)
{
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    /// @note 制御文字は \u00XX でエスケープ (UTF-8 マルチバイトはそのまま通す)。
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer.data();
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void SerializeNumber(double value, std::string& out)
{
    /// @note std::to_chars は最短往復表現を出す (1.0 → "1", 1.5 → "1.5")。JSON は非有限を許さないため 0 に丸める。
    if (!(value == value) || value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity()) {
        out += "0";
        return;
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

void SerializeValue(const JsonValue& value, std::string& out)
{
    switch (value.GetType()) {
        case JsonValue::Type::Null:   out += "null"; break;
        case JsonValue::Type::Bool:   out += value.AsBool() ? "true" : "false"; break;
        case JsonValue::Type::Number: SerializeNumber(value.AsNumber(), out); break;
        case JsonValue::Type::String: SerializeString(value.AsString(), out); break;
        case JsonValue::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& element : value.AsArray()) {
                if (!first) out.push_back(',');
                first = false;
                SerializeValue(element, out);
            }
            out.push_back(']');
            break;
        }
        case JsonValue::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& member : value.AsObject()) {
                if (!first) out.push_back(',');
                first = false;
                SerializeString(member.first, out);
                out.push_back(':');
                SerializeValue(member.second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::optional<JsonValue> ParseJson(std::string_view text, std::string* error)
{
    JsonValue out;
    Parser parser(text, error);
    if (!parser.Parse(out)) return std::nullopt;
    return out;
}

std::string SerializeJson(const JsonValue& value)
{
    std::string out;
    SerializeValue(value, out);
    return out;
}

std::string Base64Encode(const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16)
                                   | (static_cast<std::uint32_t>(data[i + 1]) << 8)
                                   | static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[triple & 0x3F]);
    }
    const std::size_t remaining = size - i;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out += "==";
    } else if (remaining == 2) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16)
                                   | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string Base64Encode(const std::vector<std::uint8_t>& data)
{
    return Base64Encode(data.data(), data.size());
}

} // namespace fbzz::editor::ai
