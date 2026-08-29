/// @file    Reflection.hpp
/// @brief   スクリプトの「自己登録リフレクション」基盤。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// 設計意図 (WHY):
/// 従来は外部ツール (FBZZ Header Tool / FHT) が FBZZ_FIELD を走査して
/// .generated.hpp に Reflect() を吐き出していた。これは
/// - 生成漏れフットガン (フィールド追加後に再生成しないと Inspector が古いまま)
/// - 1 スクリプト = 3 ファイル (.hpp / .cpp / .generated.hpp) の煩雑さ
/// - リポジトリ外ツールへの暗黙依存
/// を生んでいた。
///
/// 本ヘッダは __COUNTER__ ベースのタグディスパッチで、フィールド宣言と
/// 同じ場所から Reflect() を生成する土台を提供する。これにより外部ツールも
/// .generated.hpp も不要になり、「フィールドを 1 行足すだけ」で Inspector と
/// シリアライズが自動追従する (単一の真実)。
#pragma once
#include <string>

namespace fbzz::scene::detail {

// ── ReflectTag ────────────────────────────────────────────────────────────────
// 宣言順を保つためのタグ型。ReflectTag<N> は ReflectTag<N-1> を継承するため、
// _fbzz_reflect(ReflectTag<K>, ...) のオーバーロード集合に対して
// ReflectTag<最大値> を渡すと「最後に宣言されたフィールドの関数」が選ばれ、
// その関数が 1 つ前のタグへ再帰することで先頭まで宣言順に処理が連鎖する。
template<int N> struct ReflectTag : ReflectTag<N - 1> {};
template<>      struct ReflectTag<0> {};

// ── MakeDisplayName ──────────────────────────────────────────────────────────
// "swingStartTime" → "Swing Start Time"、"max_points" → "Max Points" のように
// camelCase / snake_case のメンバー名を人間可読な Inspector 表示名へ整形する。
// WHY: 表示名はほぼ常に変数名の機械的な再掲なので、空文字を渡せば自動生成して
//      FBZZ_FIELD の冗長さ (表示名の重複) を解消する。
inline std::string MakeDisplayName(const char* member)
{
    std::string out;
    bool prevLower = false;
    bool prevDigit = false;
    for (const char* p = member; p && *p; ++p) {
        char c = *p;
        if (c == '_') {
            // スネークケースの区切りは空白へ
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            prevLower = false;
            prevDigit = false;
            continue;
        }
        const bool isUpper = (c >= 'A' && c <= 'Z');
        const bool isDigit = (c >= '0' && c <= '9');
        // 小文字→大文字 / 文字⇔数字 の境界で単語を区切る
        if (!out.empty() && out.back() != ' ' &&
            ((isUpper && prevLower) || (isDigit && !prevDigit) || (!isDigit && prevDigit)))
            out.push_back(' ');
        // 各単語の先頭は大文字化する
        if (out.empty() || out.back() == ' ') {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        }
        out.push_back(c);
        prevLower = (c >= 'a' && c <= 'z');
        prevDigit = isDigit;
    }
    return out;
}

// 明示表示名が空 ("") なら member 名から自動生成する。非空ならそのまま使う。
inline std::string DisplayOr(const char* explicitName, const char* member)
{
    return (explicitName && explicitName[0] != '\0')
        ? std::string(explicitName)
        : MakeDisplayName(member);
}

} // namespace fbzz::scene::detail
