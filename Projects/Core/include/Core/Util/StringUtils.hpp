/// @file    StringUtils.hpp
/// @brief   文字列操作ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 検索・分割・前後の空白除去・大文字小文字の変換と、Win32 用ワイド文字列との変換をまとめる。
#pragma once
#include <Core/Api.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::util {

class StringUtils {
public:
    FBZZ_CORE_API static bool        Contains(const std::string& s, const std::string& sub);
    /// @brief 大文字小文字を区別せずに部分一致を調べる。
    FBZZ_CORE_API static bool        ContainsCI(const std::string& s, const std::string& sub);
    /// @brief 大文字小文字を無視した完全一致。
    /// @note 名前で対象を一つ選ぶ場合、部分一致だと「Console」が「Build Output Console」にも一致するため、完全一致を使う。
    FBZZ_CORE_API static bool        EqualsCI(const std::string& a, const std::string& b);
    /// @brief 所有しない文字列を大文字小文字を無視して比較する。
    /// @note std::tolower に unsigned char を渡し、EqualsCI と同じ C ロケールで比較する。
    FBZZ_CORE_API static bool        EqualsCIView(std::string_view a, std::string_view b);
    FBZZ_CORE_API static std::string ToLower(const std::string& s);
    /// @note 値で受けたバッファを再利用し、既存 ToLower のコピー契約を保ったまま一時文字列の追加コピーを避ける。
    FBZZ_CORE_API static std::string ToLowerCopy(std::string s);
    FBZZ_CORE_API static std::string ToUpper(const std::string& s);
    FBZZ_CORE_API static bool        StartsWith(const std::string& s, const std::string& prefix);
    FBZZ_CORE_API static bool        EndsWith(const std::string& s, const std::string& suffix);
    FBZZ_CORE_API static std::vector<std::string> Split(const std::string& s, char delim);
    FBZZ_CORE_API static std::string Trim(const std::string& s);

    /// @brief UTF-8 の文字境界で maxBytes 以下へ切り詰める。切ったときだけ ellipsis を足す。
    /// @param[in] maxBytes ellipsis を含む結果の最大バイト数。
    /// @note 文字の途中で切ると不正な UTF-8 列になるため、コードポイントの境界まで戻す。
    FBZZ_CORE_API static std::string TruncateUtf8(const std::string& s, std::size_t maxBytes,
                                    const std::string& ellipsis = "...");

    /// @name Win32 API 用ワイド文字列変換
    /// @{
    FBZZ_CORE_API static std::wstring ToWide(const std::string& s);
    FBZZ_CORE_API static std::string  ToNarrow(const std::wstring& s);
    /// @brief 終端 NUL を持たない Win32 API の wchar_t バッファを UTF-8 化する。
    /// @note ReadDirectoryChangesW などは文字数つきで名前を返すため、Editor 側に変換処理を重複させない。
    FBZZ_CORE_API static std::string  ToNarrow(const wchar_t* s, int charCount);
    /// @}

    /// @brief filesystem::path を UTF-8 文字列へ変換する。
    /// @note Windows の filesystem::path は wchar_t ベースなので wstring() 経由が最も安全。
    FBZZ_CORE_API static std::string  PathToUtf8(const std::filesystem::path& path);
};

} // namespace fbzz::util
