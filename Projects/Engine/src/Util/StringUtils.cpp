// FBZZ Engine
// StringUtils.cpp | fbzz::util
// 文字列操作ユーティリティ実装
// 検索・分割・trim・大文字小文字変換と wide / narrow 変換を扱う。
// Win32 API 境界で必要な文字列変換をここに集約する。
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cctype>
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

std::string StringUtils::ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
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

std::vector<std::string> StringUtils::Split(const std::string& s, char delim)
{
    std::vector<std::string> result;
    std::string token;
    for (char c : s) {
        if (c == delim) { result.push_back(token); token.clear(); }
        else            { token += c; }
    }
    if (!token.empty()) result.push_back(token);
    return result;
}

std::string StringUtils::Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring StringUtils::ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written - 1));
    return out;
}

std::string StringUtils::ToNarrow(const std::wstring& s)
{
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written - 1));
    return out;
}

std::string StringUtils::ToNarrow(const wchar_t* s, int charCount)
{
    if (!s || charCount <= 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, charCount, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, s, charCount, out.data(), len, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string StringUtils::PathToUtf8(const std::filesystem::path& path)
{
    return ToNarrow(path.wstring());
}

} // namespace fbzz::util
