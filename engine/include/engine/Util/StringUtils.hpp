// FBZZ Engine
// StringUtils.hpp | fbzz::util
// 文字列操作ユーティリティ
#pragma once
#include <string>
#include <vector>

namespace fbzz::util {

class StringUtils {
public:
    static bool        Contains(const std::string& s, const std::string& sub);
    static bool        ContainsCI(const std::string& s, const std::string& sub);  // 大文字小文字無視
    static std::string ToLower(const std::string& s);
    static std::string ToUpper(const std::string& s);
    static bool        StartsWith(const std::string& s, const std::string& prefix);
    static bool        EndsWith(const std::string& s, const std::string& suffix);
    static std::vector<std::string> Split(const std::string& s, char delim);
    static std::string Trim(const std::string& s);

    // Win32 API 用ワイド文字列変換
    static std::wstring ToWide(const std::string& s);
    static std::string  ToNarrow(const std::wstring& s);
};

} // namespace fbzz::util
