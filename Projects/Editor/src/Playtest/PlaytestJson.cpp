/// @file    PlaytestJson.cpp
/// @brief   Playtest の JSON パス解決と比較演算。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Playtest/PlaytestJson.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>

namespace fbzz::editor::playtest {

namespace {

using ai::JsonValue;

bool ParseIndex(std::string_view text, std::size_t& out)
{
    if (text.empty()) return false;
    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        value = value * 10 + static_cast<std::size_t>(character - '0');
    }
    out = value;
    return true;
}

/// @note 復号は単回走査とし、~01 を / へ二重復号しない。
/// @see https://www.rfc-editor.org/rfc/rfc6901#section-4 RFC 6901 Section 4: Evaluation.
bool DecodePointerToken(std::string_view token, std::string& decoded)
{
    decoded.clear();
    decoded.reserve(token.size());
    for (std::size_t index = 0; index < token.size(); ++index) {
        const char character = token[index];
        if (character != '~') {
            decoded.push_back(character);
            continue;
        }
        if (++index == token.size() || (token[index] != '0' && token[index] != '1')) return false;
        decoded.push_back(token[index] == '0' ? '~' : '/');
    }
    return true;
}

/// @note '/' 開始だけを JSON Pointer とし、ドット付きの literal key と空 key を保持する。
/// @see https://www.rfc-editor.org/rfc/rfc6901#section-4 RFC 6901 Section 4: Evaluation.
const JsonValue* ResolveJsonPointer(const JsonValue& root, std::string_view path)
{
    const JsonValue* current = &root;
    std::string token;
    std::size_t start = 1;
    while (current != nullptr) {
        const std::size_t slash = path.find('/', start);
        if (!DecodePointerToken(path.substr(start, slash == std::string_view::npos ? slash : slash - start), token)) return nullptr;
        if (current->IsArray()) {
            if (token.empty() || (token.size() > 1 && token.front() == '0')) return nullptr;
            std::size_t index = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
            if (error != std::errc{} || end != token.data() + token.size() || index >= current->AsArray().size()) return nullptr;
            current = &current->AsArray()[index];
        } else if (current->IsObject()) {
            current = current->Find(token);
        } else {
            return nullptr;
        }
        if (slash == std::string_view::npos) return current;
        start = slash + 1;
    }
    return nullptr;
}

/// @note 数値は 1e-6 の相対誤差まで等しいとみなす。バスの応答は double を文字列経由で往復するため厳密一致では揺れる。
bool NumbersEqual(double a, double b)
{
    const double scale = std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-6 * scale;
}

bool ValuesEqual(const JsonValue& a, const JsonValue& b)
{
    if (a.IsNumber() && b.IsNumber()) return NumbersEqual(a.AsNumber(), b.AsNumber());
    if (a.IsString() && b.IsString()) return a.AsString() == b.AsString();
    if (a.IsBool() && b.IsBool()) return a.AsBool() == b.AsBool();
    if (a.IsNull() && b.IsNull()) return true;
    /// @note 配列・オブジェクトは正規化した文字列で比べる (キー順は Json 側で安定している)。
    if ((a.IsArray() && b.IsArray()) || (a.IsObject() && b.IsObject())) return ai::SerializeJson(a) == ai::SerializeJson(b);
    return false;
}

std::string Describe(const JsonValue* value)
{
    if (value == nullptr) return "(missing)";
    std::string text = ai::SerializeJson(*value);
    if (text.size() > 160) text = text.substr(0, 157) + "...";
    return text;
}

bool LengthOf(const JsonValue& value, double& out)
{
    if (value.IsArray()) { out = static_cast<double>(value.AsArray().size()); return true; }
    if (value.IsString()) { out = static_cast<double>(value.AsString().size()); return true; }
    if (value.IsObject()) { out = static_cast<double>(value.AsObject().size()); return true; }
    return false;
}

} /// @note namespace

const JsonValue* ResolveJsonPath(const JsonValue& root, std::string_view path)
{
    if (!path.empty() && path.front() == '/') return ResolveJsonPointer(root, path);
    const JsonValue* current = &root;
    std::size_t start = 0;
    while (current != nullptr && start <= path.size()) {
        if (path.empty()) return current;
        const std::size_t dot = path.find('.', start);
        const std::string_view segment = path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (current->IsArray()) {
            std::size_t index = 0;
            if (!ParseIndex(segment, index) || index >= current->AsArray().size()) return nullptr;
            current = &current->AsArray()[index];
        } else if (current->IsObject()) {
            current = current->Find(segment);
        } else {
            return nullptr;
        }
        if (dot == std::string_view::npos) return current;
        start = dot + 1;
    }
    return current;
}

bool EvaluateJsonCondition(const JsonValue* actual, std::string_view op, const JsonValue* expected, std::string& detail)
{
    const auto fail = [&](std::string message) { detail = std::move(message); return false; };

    if (op == "exists") return actual != nullptr ? true : fail("値が存在しない");
    if (op == "missing") return actual == nullptr ? true : fail("値が存在する: " + Describe(actual));
    if (actual == nullptr) return fail("値が存在しない (op=" + std::string(op) + ")");
    if (expected == nullptr) return fail("op=" + std::string(op) + " には value が必要");

    if (op == "==") return ValuesEqual(*actual, *expected) ? true : fail(Describe(actual) + " != " + Describe(expected));
    if (op == "!=") return !ValuesEqual(*actual, *expected) ? true : fail(Describe(actual) + " == " + Describe(expected));

    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        if (!actual->IsNumber() || !expected->IsNumber()) return fail("数値比較の片側が数値でない: " + Describe(actual));
        const double a = actual->AsNumber();
        const double b = expected->AsNumber();
        const bool ok = op == "<" ? a < b : op == "<=" ? a <= b : op == ">" ? a > b : a >= b;
        return ok ? true : fail(Describe(actual) + " " + std::string(op) + " " + Describe(expected) + " が不成立");
    }

    if (op == "contains") {
        if (actual->IsString() && expected->IsString()) {
            return actual->AsString().find(expected->AsString()) != std::string::npos
                ? true : fail(Describe(actual) + " は " + Describe(expected) + " を含まない");
        }
        if (actual->IsArray()) {
            for (const JsonValue& item : actual->AsArray()) {
                if (ValuesEqual(item, *expected)) return true;
            }
            return fail("配列に " + Describe(expected) + " が無い");
        }
        return fail("contains は文字列か配列にだけ使える");
    }

    if (op == "length==" || op == "length>=" || op == "length<=") {
        double length = 0.0;
        if (!LengthOf(*actual, length)) return fail("長さを持たない値: " + Describe(actual));
        if (!expected->IsNumber()) return fail("length 系の value は数値");
        const double b = expected->AsNumber();
        const bool ok = op == "length==" ? length == b : op == "length>=" ? length >= b : length <= b;
        return ok ? true : fail("length=" + std::to_string(static_cast<long long>(length)) + " が " + std::string(op) + Describe(expected) + " を満たさない");
    }

    return fail("未知の演算子: " + std::string(op));
}

} /// @note namespace fbzz::editor::playtest
