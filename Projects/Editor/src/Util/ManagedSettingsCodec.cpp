/// @file    ManagedSettingsCodec.cpp
/// @brief   AI 層に依存しない bounded JSON と複合設定の読取。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Engine/Scene/Script.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace fbzz::editor {
namespace {
bool SameSettingsValue(const OpData& first, const OpData& second)
{
    if (first.GetType() != second.GetType()) return false;
    switch (first.GetType()) {
    case OpData::Type::Null: return true;
    case OpData::Type::Bool: return first.AsBool() == second.AsBool();
    case OpData::Type::Int: return first.AsInt64() == second.AsInt64();
    case OpData::Type::Float: return first.AsDouble() == second.AsDouble();
    case OpData::Type::String: return first.AsString() == second.AsString();
    case OpData::Type::Vec3: {
        const auto a = first.AsVec3(), b = second.AsVec3(); return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    case OpData::Type::Array:
        if (first.AsArray().size() != second.AsArray().size()) return false;
        for (size_t index = 0; index < first.AsArray().size(); ++index)
            if (!SameSettingsValue(first.AsArray()[index], second.AsArray()[index])) return false;
        return true;
    case OpData::Type::Object:
        if (first.AsObject().size() != second.AsObject().size()) return false;
        for (const auto& [key, value] : first.AsObject()) {
            const OpData* other = second.Find(key); if (!other || !SameSettingsValue(value, *other)) return false;
        }
        return true;
    }
    return false;
}

class SettingsParser {
public:
    explicit SettingsParser(std::string_view input) : m_input(input) {}
    bool Parse(OpData& result)
    {
        Skip();
        if (!Value(result, 0)) return false;
        Skip(); return m_at == m_input.size();
    }
private:
    std::string_view m_input;
    std::size_t m_at = 0;
    void Skip() { while (m_at < m_input.size() && (m_input[m_at] == ' ' || m_input[m_at] == '\t' || m_input[m_at] == '\r' || m_input[m_at] == '\n')) ++m_at; }
    bool Take(char value) { if (m_at == m_input.size() || m_input[m_at] != value) return false; ++m_at; return true; }
    bool Literal(std::string_view text) { if (m_input.substr(m_at, text.size()) != text) return false; m_at += text.size(); return true; }
    bool Value(OpData& value, std::size_t depth)
    {
        if (depth > 64 || m_at == m_input.size()) return false;
        if (m_input[m_at] == '{') {
            ++m_at; Skip(); value = OpData::MakeObject();
            if (Take('}')) return true;
            for (;;) {
                std::string key; if (!String(key) || value.Find(key)) return false;
                Skip(); if (!Take(':')) return false; Skip();
                OpData item; if (!Value(item, depth + 1)) return false;
                value.Set(std::move(key), std::move(item)); Skip();
                if (Take('}')) return true;
                if (!Take(',')) return false;
                Skip();
            }
        }
        if (m_input[m_at] == '[') {
            ++m_at; Skip(); value = OpData::MakeArray();
            if (Take(']')) return true;
            for (;;) {
                OpData item; if (!Value(item, depth + 1)) return false;
                value.Push(std::move(item)); Skip();
                if (Take(']')) return true;
                if (!Take(',')) return false;
                Skip();
            }
        }
        if (m_input[m_at] == '"') { std::string text; if (!String(text)) return false; value = OpData(std::move(text)); return true; }
        if (m_input[m_at] == 't') { if (!Literal("true")) return false; value = OpData(true); return true; }
        if (m_input[m_at] == 'f') { if (!Literal("false")) return false; value = OpData(false); return true; }
        if (m_input[m_at] == 'n') { if (!Literal("null")) return false; value = {}; return true; }
        return Number(value);
    }
    bool Hex(std::uint32_t& value)
    {
        value = 0;
        for (int index = 0; index < 4; ++index) {
            if (m_at == m_input.size()) return false;
            const char digit = m_input[m_at++]; value <<= 4;
            if (digit >= '0' && digit <= '9') value |= static_cast<std::uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            else return false;
        }
        return true;
    }
    static void Utf8(std::string& text, std::uint32_t code)
    {
        if (code < 0x80) text.push_back(static_cast<char>(code));
        else {
            const int tail = code < 0x800 ? 1 : code < 0x10000 ? 2 : 3;
            text.push_back(static_cast<char>((tail == 1 ? 0xC0u : tail == 2 ? 0xE0u : 0xF0u) | (code >> (tail * 6))));
            for (int shift = (tail - 1) * 6; shift >= 0; shift -= 6) text.push_back(static_cast<char>(0x80u | ((code >> shift) & 0x3Fu)));
        }
    }
    bool String(std::string& text)
    {
        if (!Take('"')) return false;
        while (m_at < m_input.size()) {
            const unsigned char byte = static_cast<unsigned char>(m_input[m_at++]);
            if (byte == '"') return true;
            if (byte < 0x20) return false;
            if (byte == '\\') {
                if (m_at == m_input.size()) return false;
                const char escape = m_input[m_at++];
                switch (escape) {
                case '"': case '\\': case '/': text.push_back(escape); break;
                case 'b': text.push_back('\b'); break;
                case 'f': text.push_back('\f'); break;
                case 'n': text.push_back('\n'); break;
                case 'r': text.push_back('\r'); break;
                case 't': text.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code; if (!Hex(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (!Take('\\') || !Take('u')) return false;
                        std::uint32_t low; if (!Hex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                    } else if (code >= 0xDC00 && code <= 0xDFFF) return false;
                    Utf8(text, code); break;
                }
                default: return false;
                }
            } else if (byte < 0x80) text.push_back(static_cast<char>(byte));
            else {
                const int tail = byte >= 0xC2 && byte <= 0xDF ? 1 : byte >= 0xE0 && byte <= 0xEF ? 2 : byte >= 0xF0 && byte <= 0xF4 ? 3 : 0;
                if (tail == 0 || m_at + tail > m_input.size()) return false;
                std::uint32_t code = byte & (tail == 1 ? 0x1Fu : tail == 2 ? 0x0Fu : 0x07u);
                for (int index = 0; index < tail; ++index) {
                    const unsigned char next = static_cast<unsigned char>(m_input[m_at++]);
                    if ((next & 0xC0u) != 0x80u) return false;
                    code = (code << 6) | (next & 0x3Fu);
                }
                if (code < (tail == 1 ? 0x80u : tail == 2 ? 0x800u : 0x10000u) || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
                Utf8(text, code);
            }
        }
        return false;
    }
    bool Number(OpData& value)
    {
        const std::size_t first = m_at; Take('-');
        if (!Take('0')) {
            if (m_at == m_input.size() || m_input[m_at] < '1' || m_input[m_at] > '9') return false;
            while (m_at < m_input.size() && m_input[m_at] >= '0' && m_input[m_at] <= '9') ++m_at;
        }
        bool real = false;
        if (Take('.')) {
            real = true; const std::size_t digits = m_at;
            while (m_at < m_input.size() && m_input[m_at] >= '0' && m_input[m_at] <= '9') ++m_at;
            if (m_at == digits) return false;
        }
        if (Take('e') || Take('E')) {
            real = true; if (!Take('+')) Take('-'); const std::size_t digits = m_at;
            while (m_at < m_input.size() && m_input[m_at] >= '0' && m_input[m_at] <= '9') ++m_at;
            if (m_at == digits) return false;
        }
        const char* begin = m_input.data() + first; const char* end = m_input.data() + m_at;
        if (!real) {
            std::int64_t integer = 0;
            const auto parsed = std::from_chars(begin, end, integer);
            if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
            value = OpData(integer);
            return true;
        }
        double number = 0.0; const auto parsed = std::from_chars(begin, end, number);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(number)) return false;
        value = OpData(number); return true;
    }
};
struct StructuredReader final : scene::IReflector {
    std::string json;
    std::vector<scene::ScriptObjectReference> references;
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char* name, std::string& value) override { if (std::string_view(name) == "json") json = value; }
    void ObjectReferenceListField(const char* name, std::vector<scene::ScriptObjectReference>& value) override
    { if (std::string_view(name) == "objectGuids") references = value; }
};
bool IndexReferences(const OpData& value, std::string_view path,
    const std::vector<scene::ScriptObjectReference>& references, std::size_t& index,
    ManagedStructuredReferencePaths& output, std::size_t depth, std::size_t& nodes)
{
    if (depth > 64) return false;
    if (const OpData* guid = value.Find("$go")) {
        if (!guid->IsString() || value.AsObject().size() != 1 || index >= references.size()
            || references[index].guid != guid->AsString()) return false;
        output.emplace(std::string(path), references[index++]);
    } else if (value.IsObject()) {
        for (const auto& [key, child] : value.AsObject())
            if (!IndexReferences(child, ManagedSettingChildPath(path, key), references, index, output, depth + 1, nodes)) return false;
    } else if (value.IsArray()) {
        for (std::size_t child = 0; child < value.AsArray().size(); ++child)
            if (!IndexReferences(value.AsArray()[child], ManagedSettingChildPath(path, std::to_string(child)),
                references, index, output, depth + 1, nodes)) return false;
    }
    return true;
}
} /// @note namespace

bool ManagedSettingsPagesEqual(const OpData& first, const OpData& second)
{
    if (!first.IsObject() || !second.IsObject()) return false;
    for (std::string_view key : {"value", "kind", "total", "start", "returned", "items", "text", "unit", "references", "diagnostic"}) {
        const OpData* a = first.Find(key), *b = second.Find(key);
        if ((a == nullptr) != (b == nullptr) || (a && !SameSettingsValue(*a, *b))) return false;
    }
    return true;
}
std::uint64_t ManagedSettingsPageCursor::Start() const
{
    return m_starts[m_current];
}

bool ManagedSettingsPageCursor::CanAdvance(const OpData& page) const
{
    const OpData* start = page.Find("start");
    const OpData* returned = page.Find("returned");
    const OpData* next = page.Find("nextStart");
    if (!start || !start->IsInt() || !returned || !returned->IsInt() || !next || !next->IsInt())
        return false;
    if (start->AsInt64() < 0 || returned->AsInt64() <= 0 ||
        static_cast<std::uint64_t>(start->AsInt64()) != Start()) return false;
    const std::uint64_t nextStart = static_cast<std::uint64_t>(start->AsInt64()) +
        static_cast<std::uint64_t>(returned->AsInt64());
    if (next->AsInt64() < 0 || static_cast<std::uint64_t>(next->AsInt64()) != nextStart || nextStart <= Start())
        return false;
    return true;
}

bool ManagedSettingsPageCursor::Advance(const OpData& page)
{
    if (!CanAdvance(page)) return false;
    const std::uint64_t nextStart = static_cast<std::uint64_t>(page.Find("nextStart")->AsInt64());
    if (HasVisitedNext() && m_starts[m_current + 1] != nextStart)
        m_starts.erase(m_starts.begin() + static_cast<std::ptrdiff_t>(m_current + 1), m_starts.end());
    if (!HasVisitedNext()) m_starts.push_back(nextStart);
    ++m_current;
    return true;
}

bool ManagedSettingsPageCursor::Back()
{
    if (m_current == 0) return false;
    --m_current;
    return true;
}

void ManagedSettingsPageCursor::InvalidateFuture()
{
    if (m_current + 1 < m_starts.size())
        m_starts.erase(m_starts.begin() + static_cast<std::ptrdiff_t>(m_current + 1), m_starts.end());
}

void ManagedSettingsPageCursor::Reset()
{
    m_starts.assign(1, 0);
    m_current = 0;
}

bool ReadManagedSettingsJson(std::string_view json, OpData& output)
{
    OpData candidate; if (!SettingsParser(json).Parse(candidate)) return false;
    output = std::move(candidate); return true;
}
bool ReadManagedStructuredSetting(scene::IScriptSerializable& value, OpData& output, std::vector<scene::ScriptObjectReference>* references)
{
    StructuredReader reader; value.Reflect(reader);
    OpData envelope; if (!ReadManagedSettingsJson(reader.json, envelope) || !envelope.IsObject() || envelope.AsObject().size() != 2) return false;
    const OpData* version = envelope.Find("$version"); const OpData* content = envelope.Find("value");
    if (!version || !version->IsInt() || version->AsInt64() != 1 || !content) return false;
    output = *content;
    if (references) *references = std::move(reader.references);
    return true;
}
std::string ManagedSettingChildPath(std::string_view parent, std::string_view key)
{
    std::string result(parent); result.push_back('/');
    for (char character : key) {
        if (character == '~') result.append("~0");
        else if (character == '/') result.append("~1");
        else result.push_back(character);
    }
    return result;
}
bool IndexManagedStructuredReferences(const OpData& value,
    const std::vector<scene::ScriptObjectReference>& references, ManagedStructuredReferencePaths& output)
{
    ManagedStructuredReferencePaths candidate;
    std::size_t index = 0, nodes = 0;
    if (!IndexReferences(value, "", references, index, candidate, 0, nodes) || index != references.size()) return false;
    output = std::move(candidate);
    return true;
}
} /// @note namespace fbzz::editor
