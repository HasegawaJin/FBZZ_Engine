/// @file    StringUtils.cpp
/// @brief   文字列操作ユーティリティ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 検索・分割・trim・大文字小文字変換と wide / narrow 変換を扱う。
/// @note Win32 API 境界で必要な文字列変換をここに集約する。
#include <Core/Util/StringUtils.hpp>
#include <algorithm>
#include <cctype>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace fbzz::util {

bool StringUtils::Contains(const std::string& s, const std::string& sub)
{
    return s.find(sub) != std::string::npos;
}

bool StringUtils::ContainsCI(const std::string& s, const std::string& sub)
{
    return ToLower(s).find(ToLower(sub)) != std::string::npos;
}

bool StringUtils::EqualsCI(const std::string& a, const std::string& b)
{
    return EqualsCIView(a, b);
}

bool StringUtils::EqualsCIView(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string StringUtils::ToLower(const std::string& s)
{
    return ToLowerCopy(s);
}

std::string StringUtils::ToLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string StringUtils::ToUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

bool StringUtils::StartsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool StringUtils::EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::string> StringUtils::Split(const std::string& text, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    for (char character : text) {
        if (character == delimiter) {
            tokens.push_back(token);
            token.clear();
        } else {
            token += character;
        }
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string StringUtils::Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string StringUtils::TruncateUtf8(const std::string& s, std::size_t maxBytes,
                                      const std::string& ellipsis)
{
    if (s.size() <= maxBytes) {
        return s;
    }
    // ellipsis すら入らない指定は «切れるだけ切る» に倒す。
    const std::size_t budget = maxBytes > ellipsis.size() ? maxBytes - ellipsis.size() : 0;

    // 継続バイト (0b10xxxxxx) は文字の途中。境界まで戻してから切る。
    std::size_t cut = budget;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + ellipsis;
}

/// @note Win32 に入力長 -1 を渡す変換は、終端 NUL を含めて確保してから結果を縮める。
std::wstring StringUtils::ToWide(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (requiredChars <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
    const int writtenChars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), requiredChars);
    if (writtenChars <= 0) {
        return {};
    }
    wide.resize(static_cast<size_t>(writtenChars - 1));
    return wide;
}

std::string StringUtils::ToNarrow(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(requiredBytes), '\0');
    const int writtenBytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), requiredBytes, nullptr, nullptr);
    if (writtenBytes <= 0) {
        return {};
    }
    utf8.resize(static_cast<size_t>(writtenBytes - 1));
    return utf8;
}

std::string StringUtils::ToNarrow(const wchar_t* wide, int charCount)
{
    if (!wide || charCount <= 0) {
        return {};
    }
    const int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, wide, charCount, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(requiredBytes), '\0');
    const int writtenBytes = WideCharToMultiByte(CP_UTF8, 0, wide, charCount, utf8.data(), requiredBytes, nullptr, nullptr);
    if (writtenBytes <= 0) {
        return {};
    }
    utf8.resize(static_cast<size_t>(writtenBytes));
    return utf8;
}

std::string StringUtils::PathToUtf8(const std::filesystem::path& path)
{
    // Windows の filesystem::path は '\' 区切りを返すため '/' に正規化する。
    // FileSystem::PathToUtf8 と挙動を統一し、アセット管理の文字列比較を一致させる。
    std::string s = ToNarrow(path.wstring());
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // namespace fbzz::util
