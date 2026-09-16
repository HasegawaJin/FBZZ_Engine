/// @file    EditorIcons.hpp
/// @brief   UI で使う記号グリフ (Segoe Fluent Icons / MDL2 Assets) の名前付き定義。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY 画像アイコンを持たないか:
///   .png を並べると DPI ごとに用意が要り、色も焼き込みになる。フォントなら
///   文字と同じ経路で拡大・着色でき、UI スケール (EditorTheme::SetUiScale) にも
///   そのまま追従する。既に日本語フォントを merge しているので、増えるのは 1 本だけ。
///
/// WHY Windows のシステムフォントか:
///   エディターは Windows 専用で、再配布もしない (配布するのはゲーム側)。
///   ThirdParty へ持ち込まずに済み、ライセンスの取り回しも起きない。
///   見つからない環境では Available() が false になり、呼び出し側は
///   Or() の文字ラベルへ落ちる — 豆腐 (□) は出さない。
#pragma once

namespace fbzz::editor::icons {

namespace detail {

/// PUA のコードポイント 1 つを UTF-8 の文字列にしたもの。
/// WHY 型にするか: "\xEE\x9D\xA8" を手で書くと、間違えてもコンパイルは通り、
///     画面に別の絵が出るまで気付けない。コードポイントのまま書けるようにする。
struct Glyph {
    char text[4];
};

[[nodiscard]] constexpr Glyph ToUtf8(unsigned int codepoint)
{
    return Glyph{ { static_cast<char>(0xE0u | (codepoint >> 12)),
                    static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)),
                    static_cast<char>(0x80u | (codepoint & 0x3Fu)),
                    '\0' } };
}

/// 記号フォントを merge できたか。書くのは EditorTheme::Apply() だけ。
/// WHY 公開するか: フォントを積む場所とアイコンを使う場所は別ファイルで、
///     成否を伝える経路がここ以外に無い。読むのは Available() / Or() を通す。
extern bool g_iconFontLoaded;

} // namespace detail

// ── 再生 ──────────────────────────────────────────────────────────────────
inline constexpr detail::Glyph kPlay     = detail::ToUtf8(0xE768);
inline constexpr detail::Glyph kPause    = detail::ToUtf8(0xE769);
inline constexpr detail::Glyph kStop     = detail::ToUtf8(0xE71A);
inline constexpr detail::Glyph kStep     = detail::ToUtf8(0xE893);

// ── 状態 ──────────────────────────────────────────────────────────────────
inline constexpr detail::Glyph kInfo     = detail::ToUtf8(0xE946);
inline constexpr detail::Glyph kWarning  = detail::ToUtf8(0xE7BA);
inline constexpr detail::Glyph kError    = detail::ToUtf8(0xE783);
inline constexpr detail::Glyph kDetail   = detail::ToUtf8(0xE712); // 詳細ログ (…)
inline constexpr detail::Glyph kAccept   = detail::ToUtf8(0xE8FB);
inline constexpr detail::Glyph kCancel   = detail::ToUtf8(0xE711);

// ── 操作 ──────────────────────────────────────────────────────────────────
inline constexpr detail::Glyph kSearch   = detail::ToUtf8(0xE721);
inline constexpr detail::Glyph kRefresh  = detail::ToUtf8(0xE72C);
inline constexpr detail::Glyph kSettings = detail::ToUtf8(0xE713);
inline constexpr detail::Glyph kAdd      = detail::ToUtf8(0xE710);
inline constexpr detail::Glyph kDelete   = detail::ToUtf8(0xE74D);
inline constexpr detail::Glyph kSave     = detail::ToUtf8(0xE74E);
inline constexpr detail::Glyph kEdit     = detail::ToUtf8(0xE70F);
inline constexpr detail::Glyph kFilter   = detail::ToUtf8(0xE71C);
inline constexpr detail::Glyph kLink     = detail::ToUtf8(0xE71B);
inline constexpr detail::Glyph kLock     = detail::ToUtf8(0xE72E);
inline constexpr detail::Glyph kView     = detail::ToUtf8(0xE890);

// ── ギズモ ────────────────────────────────────────────────────────────────
inline constexpr detail::Glyph kMove     = detail::ToUtf8(0xE7C2);
inline constexpr detail::Glyph kRotate   = detail::ToUtf8(0xE7AD);
inline constexpr detail::Glyph kScale    = detail::ToUtf8(0xE740);
inline constexpr detail::Glyph kWorld    = detail::ToUtf8(0xE909);
inline constexpr detail::Glyph kPivot    = detail::ToUtf8(0xE81D);
inline constexpr detail::Glyph kSnapGrid = detail::ToUtf8(0xE80A);

// ── もの ──────────────────────────────────────────────────────────────────
inline constexpr detail::Glyph kFolder   = detail::ToUtf8(0xE8B7);
inline constexpr detail::Glyph kDocument = detail::ToUtf8(0xE8A5);
inline constexpr detail::Glyph kCamera   = detail::ToUtf8(0xE722);
inline constexpr detail::Glyph kLight    = detail::ToUtf8(0xE706);
inline constexpr detail::Glyph kGrid     = detail::ToUtf8(0xE80A);
inline constexpr detail::Glyph kTimeline = detail::ToUtf8(0xE916);

/// 記号フォントが読めているか。読めていなければ描画側は文字へ落とす。
[[nodiscard]] bool Available();

/// 記号が使えればその字を、使えなければ fallback を返す。
/// @note 戻り値は静的記憶域を指す。書き換えない。
[[nodiscard]] const char* Or(const detail::Glyph& glyph, const char* fallback);

} // namespace fbzz::editor::icons
