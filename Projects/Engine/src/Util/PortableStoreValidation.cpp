/// @file    PortableStoreValidation.cpp
/// @brief   DOM を作らず portable envelope 全体を検査し、旧値の上書きを防ぐ。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "PortableStoreValidation.hpp"
#include <charconv>
#include <cmath>
#include <string>
#include <unordered_set>
namespace fbzz::util {
namespace {
class Reader {
public:
    explicit Reader(std::string_view value) : m_value(value) {}
    bool Envelope(std::string_view type, std::string_view schema)
    {
        if (!Take('{')) return false;
        unsigned fields = 0; std::unordered_set<std::string> keys;
        do {
            std::string key; if (!String(&key) || !keys.insert(key).second || !Take(':')) return false;
            if (key == "formatVersion") { if (!Literal("1") || (Peek() != ',' && Peek() != '}')) return false; fields |= 1; }
            else if (key == "type" || key == "schema") {
                std::string value; if (!String(&value) || value != (key == "type" ? type : schema)) return false; fields |= key == "type" ? 2U : 4U;
            } else if (key == "value") { if (!Value(0)) return false; fields |= 8; }
            else return false;
            if (Take('}')) { Space(); return fields == 15 && m_position == m_value.size(); }
        } while (Take(','));
        return false;
    }
private:
    void Space() { while (m_position < m_value.size() && (m_value[m_position] == ' ' || m_value[m_position] == '\n' || m_value[m_position] == '\r' || m_value[m_position] == '\t')) ++m_position; }
    char Peek() { Space(); return m_position < m_value.size() ? m_value[m_position] : '\0'; }
    bool Take(char value) { if (Peek() != value) return false; ++m_position; return true; }
    bool Literal(std::string_view value) { Space(); if (m_value.substr(m_position, value.size()) != value) return false; m_position += value.size(); return true; }
    bool Hex(uint32_t& value)
    {
        value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            if (m_position == m_value.size()) return false; const char c = m_value[m_position++];
            unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 16U;
            if (digit > 15) return false; value = (value << 4) | digit;
        }
        return true;
    }
    bool String(std::string* output = nullptr)
    {
        if (!Take('"')) return false;
        while (m_position < m_value.size()) {
            const unsigned char c = static_cast<unsigned char>(m_value[m_position++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                if (c < 0x80) { if (output) output->push_back(static_cast<char>(c)); continue; }
                const size_t start = m_position - 1; uint32_t point = 0, remaining = 0, minimum = 0;
                if (c >= 0xC2 && c <= 0xDF) { point = c & 31; remaining = 1; minimum = 0x80; }
                else if (c >= 0xE0 && c <= 0xEF) { point = c & 15; remaining = 2; minimum = 0x800; }
                else if (c >= 0xF0 && c <= 0xF4) { point = c & 7; remaining = 3; minimum = 0x10000; }
                else return false;
                if (remaining > m_value.size() - m_position) return false;
                for (uint32_t index = 0; index < remaining; ++index) {
                    const auto next = static_cast<unsigned char>(m_value[m_position++]);
                    if ((next & 0xC0) != 0x80) return false; point = (point << 6) | (next & 63);
                }
                if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
                if (output) output->append(m_value.substr(start, m_position - start));
                continue;
            }
            if (m_position == m_value.size()) return false; const char escape = m_value[m_position++];
            char decoded = 0;
            switch (escape) { case '"': decoded = '"'; break; case '\\': decoded = '\\'; break; case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break; case 'f': decoded = '\f'; break; case 'n': decoded = '\n'; break; case 'r': decoded = '\r'; break; case 't': decoded = '\t'; break;
            case 'u': {
                uint32_t point = 0; if (!Hex(point)) return false;
                if (point >= 0xD800 && point <= 0xDBFF) {
                    uint32_t low = 0; if (m_value.substr(m_position, 2) != "\\u") return false; m_position += 2;
                    if (!Hex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    point = 0x10000 + ((point - 0xD800) << 10) + (low - 0xDC00);
                } else if (point >= 0xDC00 && point <= 0xDFFF) return false;
                if (output) {
                    if (point < 0x80) output->push_back(static_cast<char>(point));
                    else if (point < 0x800) { output->push_back(static_cast<char>(0xC0 | (point >> 6))); output->push_back(static_cast<char>(0x80 | (point & 63))); }
                    else if (point < 0x10000) { output->push_back(static_cast<char>(0xE0 | (point >> 12))); output->push_back(static_cast<char>(0x80 | ((point >> 6) & 63))); output->push_back(static_cast<char>(0x80 | (point & 63))); }
                    else { output->push_back(static_cast<char>(0xF0 | (point >> 18))); output->push_back(static_cast<char>(0x80 | ((point >> 12) & 63))); output->push_back(static_cast<char>(0x80 | ((point >> 6) & 63))); output->push_back(static_cast<char>(0x80 | (point & 63))); }
                }
                continue;
            }
            default: return false; }
            if (output) output->push_back(decoded);
        }
        return false;
    }
    bool Number()
    {
        Space(); const size_t start = m_position; if (Peek() == '-') ++m_position;
        if (m_position == m_value.size()) return false;
        if (m_value[m_position] == '0') ++m_position;
        else {
            if (m_value[m_position] < '1' || m_value[m_position] > '9') return false;
            while (m_position < m_value.size() && m_value[m_position] >= '0' && m_value[m_position] <= '9') ++m_position;
        }
        const auto digits = [&] { const auto before = m_position; while (m_position < m_value.size() && m_value[m_position] >= '0' && m_value[m_position] <= '9') ++m_position; return m_position != before; };
        if (m_position < m_value.size() && m_value[m_position] == '.') { ++m_position; if (!digits()) return false; }
        if (m_position < m_value.size() && (m_value[m_position] == 'e' || m_value[m_position] == 'E')) {
            ++m_position; if (m_position < m_value.size() && (m_value[m_position] == '+' || m_value[m_position] == '-')) ++m_position;
            if (!digits()) return false;
        }
        double value = 0; const auto parsed = std::from_chars(m_value.data() + start, m_value.data() + m_position, value);
        return parsed.ec == std::errc{} && parsed.ptr == m_value.data() + m_position && std::isfinite(value);
    }
    bool Value(unsigned depth)
    {
        if (depth > 32) return false;
        const char first = Peek();
        if (first == '"') return String();
        if (first == 't') return Literal("true"); if (first == 'f') return Literal("false"); if (first == 'n') return Literal("null");
        if (first == '[') { ++m_position; if (Take(']')) return true; do { if (!Value(depth + 1)) return false; if (Take(']')) return true; } while (Take(',')); return false; }
        if (first == '{') {
            ++m_position; std::unordered_set<std::string> keys; if (Take('}')) return true;
            do { std::string key; if (!String(&key) || !keys.insert(key).second || !Take(':') || !Value(depth + 1)) return false; if (Take('}')) return true; } while (Take(',')); return false;
        }
        return Number();
    }
    std::string_view m_value; size_t m_position = 0;
};
} /// @note namespace
bool ValidatePortableStoreDocument(std::string_view json, std::string_view type, std::string_view schema)
{ return Reader(json).Envelope(type, schema); }
} /// @note namespace fbzz::util
