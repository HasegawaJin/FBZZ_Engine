// FBZZ Engine
// StringUtils.hpp | fbzz::util
// 文字列操作ユーティリティ
// 検索・分割・trim・大文字小文字変換と Win32 向け wide/narrow 変換をまとめる。
// 文字コード境界をここに寄せ、呼び出し側の変換処理を減らす。
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
