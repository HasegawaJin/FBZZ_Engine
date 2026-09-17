/// @file    Reflection.hpp
/// @brief   スクリプトの「自己登録リフレクション」基盤。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// @note 従来は外部ツール (FBZZ Header Tool / FHT) が FBZZ_FIELD を走査し .generated.hpp に Reflect() を
///       吐き出していた (生成漏れフットガン・1 スクリプト = 3 ファイル・リポジトリ外ツール依存)。
/// @note 本ヘッダは __COUNTER__ ベースのタグディスパッチで、フィールド宣言と同じ場所から Reflect() を生成する。
///       外部ツールも .generated.hpp も不要になり、フィールドを 1 行足すだけで Inspector とシリアライズが
///       自動追従する。
#pragma once
#include <string>

namespace fbzz::scene::detail {

/// @cond INTERNAL
/// @brief 宣言順を保つためのタグ型。`ReflectTag<N>` は `ReflectTag<N-1>` を継承する。
/// @note `ReflectTag<最大値>` を渡すと最後に宣言したフィールドの関数が選ばれ、1 つ前のタグへ再帰して先頭まで宣言順に連鎖する。
/// @note Doxygen は再帰継承を循環と誤検出するため、条件セクションでリファレンスから外している。
template<int N> struct ReflectTag : ReflectTag<N - 1> {};
template<>      struct ReflectTag<0> {};
/// @endcond

/// @brief camelCase / snake_case のメンバー名を人間可読な Inspector 表示名へ整形する (例: `swingStartTime` → `Swing Start Time`)。
/// @note 空文字を渡すと自動生成し、FBZZ_FIELD の表示名の重複記述を省ける。
inline std::string MakeDisplayName(const char* member)
{
    std::string out;
    bool prevLower = false;
    bool prevDigit = false;
    for (const char* p = member; p && *p; ++p) {
        char c = *p;
        if (c == '_') {
            /// @note スネークケースの区切りは空白へ変換する。
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            prevLower = false;
            prevDigit = false;
            continue;
        }
        const bool isUpper = (c >= 'A' && c <= 'Z');
        const bool isDigit = (c >= '0' && c <= '9');
        /// @note 小文字→大文字 / 文字⇔数字 の境界で単語を区切る。
        if (!out.empty() && out.back() != ' ' &&
            ((isUpper && prevLower) || (isDigit && !prevDigit) || (!isDigit && prevDigit)))
            out.push_back(' ');
        /// @note 各単語の先頭は大文字化する。
        if (out.empty() || out.back() == ' ') {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        }
        out.push_back(c);
        prevLower = (c >= 'a' && c <= 'z');
        prevDigit = isDigit;
    }
    return out;
}

/// @brief 明示表示名が空 (`""`) なら member 名から自動生成する。非空ならそのまま使う。
inline std::string DisplayOr(const char* explicitName, const char* member)
{
    return (explicitName && explicitName[0] != '\0')
        ? std::string(explicitName)
        : MakeDisplayName(member);
}

} // namespace fbzz::scene::detail
