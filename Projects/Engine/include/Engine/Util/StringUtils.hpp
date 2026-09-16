/// @file    StringUtils.hpp
/// @brief   文字列操作ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 検索・分割・trim・大文字小文字変換と Win32 向け wide/narrow 変換をまとめる。
/// 文字コード境界をここに寄せ、呼び出し側の変換処理を減らす。
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::util {

class StringUtils {
public:
    static bool        Contains(const std::string& s, const std::string& sub);
    static bool        ContainsCI(const std::string& s, const std::string& sub);  // 大文字小文字無視
    // 大文字小文字を無視した完全一致。
    // WHY 部分一致と別に要るか: 名前で対象を 1 つ選ぶ用途 (パネル名・プリセット名) では
    //     部分一致だと "Console" が "Build Output Console" にも当たり、
    //     指したつもりの無い対象を操作してしまう。
    static bool        EqualsCI(const std::string& a, const std::string& b);
    static std::string ToLower(const std::string& s);
    static std::string ToUpper(const std::string& s);
    static bool        StartsWith(const std::string& s, const std::string& prefix);
    static bool        EndsWith(const std::string& s, const std::string& suffix);
    static std::vector<std::string> Split(const std::string& s, char delim);
    static std::string Trim(const std::string& s);

    /// UTF-8 の文字境界で maxBytes 以下へ切り詰める。切ったときだけ ellipsis を足す。
    ///
    /// WHY 要るか: `s.substr(0, n)` はバイトで切るので、日本語の途中で切ると
    ///     壊れた列が残り、UI では «□» になる。「64 文字まで」のつもりで書いた
    ///     切り詰めが、日本語のときだけ表示を壊す ── 英語で試している限り出ない。
    ///
    /// @param maxBytes ellipsis を含めた上限。これより短い結果しか返さない。
    static std::string TruncateUtf8(const std::string& s, std::size_t maxBytes,
                                    const std::string& ellipsis = "...");

    // Win32 API 用ワイド文字列変換
    static std::wstring ToWide(const std::string& s);
    static std::string  ToNarrow(const std::wstring& s);
    // 終端 NUL を持たない Win32 API の wchar_t バッファを UTF-8 化する。
    // WHY: ReadDirectoryChangesW などは文字数つきで名前を返すため、Editor 側に変換処理を重複させない。
    static std::string  ToNarrow(const wchar_t* s, int charCount);

    /// filesystem::path を UTF-8 文字列へ変換する。
    /// WHY: Windows の filesystem::path は wchar_t ベースなので wstring() 経由が最も安全。
    static std::string  PathToUtf8(const std::filesystem::path& path);
};

} // namespace fbzz::util
