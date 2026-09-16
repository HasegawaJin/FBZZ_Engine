/// @file    Utf8.hpp
/// @brief   UTF-8 バイト列とコードポイント (char32_t) の相互変換。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY: UI テキストは std::string (UTF-8) で保持するが、フォントのグリフ検索は
/// コードポイント単位で行う必要がある。日本語 1 文字は UTF-8 で 3 バイトなので、
/// std::string を char 単位で走査すると 1 文字が 3 グリフに化けてしまう。
/// 文字コード境界の処理をこのヘッダに集約し、呼び出し側から分岐を消す。
///
/// WHY (ヘッダオンリー): 数命令で終わる小さな関数の集合であり、
/// テキストレイアウトのホットループから 1 文字ごとに呼ばれるためインライン化したい。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::util {

// 不正なバイト列に遭遇したときに返す文字 (U+FFFD REPLACEMENT CHARACTER)。
// WHY: 不正入力で例外を投げる (AGENTS.md で禁止) ことも、無言で打ち切ることもせず、
//      「読めない文字がここにあった」と目に見える形で描画へ伝える。
inline constexpr char32_t UTF8_REPLACEMENT_CHAR = 0xFFFDu;

// Unicode の最大コードポイント。これを超える値は不正。
inline constexpr char32_t UTF8_MAX_CODEPOINT = 0x10FFFFu;

class Utf8 {
public:
    // s の offset 位置から 1 コードポイントをデコードし、offset を次の文字の先頭へ進める。
    //
    // 不正なバイト列 (不正な先頭バイト / 継続バイト不足 / 冗長符号化 / サロゲート /
    // 範囲外) の場合は UTF8_REPLACEMENT_CHAR を返し、offset は 1 バイトだけ進める。
    // WHY: どの失敗経路でも必ず 1 以上進めることで、呼び出し側の while ループが
    //      無限ループに陥らないことを保証する。
    [[nodiscard]] static char32_t Decode(std::string_view s, std::size_t& offset)
    {
        if (offset >= s.size()) return 0;

        const auto  bytes = reinterpret_cast<const unsigned char*>(s.data());
        const unsigned char lead = bytes[offset];

        // 1 バイト (ASCII): 0xxxxxxx
        if (lead < 0x80u) {
            ++offset;
            return static_cast<char32_t>(lead);
        }

        // 継続バイト (10xxxxxx) が先頭に来るのは不正 (文字の途中から読み始めた場合)
        if (lead < 0xC0u) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 先頭バイトから系列長と初期ビットを決める。
        std::size_t length  = 0;
        char32_t    result  = 0;
        char32_t    minimum = 0;   // 冗長符号化の検出に使う、この長さで表せる最小値
        if (lead < 0xE0u) {        // 110xxxxx → 2 バイト
            length = 2; result = lead & 0x1Fu; minimum = 0x80u;
        } else if (lead < 0xF0u) { // 1110xxxx → 3 バイト
            length = 3; result = lead & 0x0Fu; minimum = 0x800u;
        } else if (lead < 0xF8u) { // 11110xxx → 4 バイト
            length = 4; result = lead & 0x07u; minimum = 0x10000u;
        } else {                   // 0xF8 以降は UTF-8 に存在しない
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 系列が文字列末尾で途切れている
        if (offset + length > s.size()) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 継続バイトを畳み込む。1 つでも 10xxxxxx でなければ不正。
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = bytes[offset + i];
            if ((continuation & 0xC0u) != 0x80u) {
                ++offset;
                return UTF8_REPLACEMENT_CHAR;
            }
            result = (result << 6) | (continuation & 0x3Fu);
        }

        // 冗長符号化 (より短い系列で表せる値) / サロゲート領域 / 範囲外を弾く。
        // WHY: 冗長符号化を通すと、同じ文字が複数のバイト列で表現できてしまい、
        //      文字列比較やフィルタリングの前提が崩れる (セキュリティ上の定石)。
        if (result < minimum || result > UTF8_MAX_CODEPOINT
            || (result >= 0xD800u && result <= 0xDFFFu)) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        offset += length;
        return result;
    }

    // 文字列に含まれるコードポイント数を数える (バイト数ではない)。
    [[nodiscard]] static std::size_t Length(std::string_view s)
    {
        std::size_t offset = 0;
        std::size_t count  = 0;
        while (offset < s.size()) {
            // 戻り値は使わず offset を進めるためだけに呼ぶ。
            // [[nodiscard]] を明示的に捨てる (/W4 の C4834 対策)。
            static_cast<void>(Decode(s, offset));
            ++count;
        }
        return count;
    }

    // 1 コードポイントを UTF-8 バイト列として out の末尾へ追記する。
    // 不正なコードポイントは U+FFFD として書き出す。
    static void Append(std::string& out, char32_t codePoint)
    {
        if (codePoint > UTF8_MAX_CODEPOINT
            || (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
            codePoint = UTF8_REPLACEMENT_CHAR;
        }

        if (codePoint < 0x80u) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
    }
};

} // namespace fbzz::util
