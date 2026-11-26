/// @file    Utf8.hpp
/// @brief   UTF-8 バイト列とコードポイント (char32_t) の相互変換。
/// @author  Hasegawa Jin
/// @date    2026-08-19
/// @note UI テキストは std::string (UTF-8) で保持するが、フォントのグリフ検索はコードポイント
/// @note       単位で行う必要がある。日本語 1 文字は UTF-8 で 3 バイトなので、std::string を char
/// @note       単位で走査すると 1 文字が 3 グリフに化けてしまう。文字コード境界の処理をこのヘッダに
/// @note       集約し、呼び出し側から分岐を消す。ヘッダオンリーなのは、テキストレイアウトの
/// @note       ホットループから 1 文字ごとに呼ばれる小さな関数群をインライン化したいため。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::util {

/// @brief 不正なバイト列に遭遇したときに返す文字 (U+FFFD REPLACEMENT CHARACTER)。
/// @note 不正入力で例外を投げる (AGENTS.md で禁止) ことも無言で打ち切ることもせず、
/// @note       「読めない文字がここにあった」と目に見える形で描画へ伝える。
inline constexpr char32_t UTF8_REPLACEMENT_CHAR = 0xFFFDu;

/// @brief Unicode の最大コードポイント。これを超える値は不正。
inline constexpr char32_t UTF8_MAX_CODEPOINT = 0x10FFFFu;

class Utf8 {
public:
    /// @brief 指定位置から UTF-8 のコードポイントを1つ読み取る。
    /// @param[in] s 読み取る UTF-8 バイト列。
    /// @param[in,out] offset 読み取り開始位置をバイト単位で渡す。正常なら文字の長さだけ、不正な列なら1だけ進む。末尾以降では変わらない。
    /// @return 読み取ったコードポイント。不正な列では UTF8_REPLACEMENT_CHAR、末尾以降では 0。
    [[nodiscard]] static char32_t Decode(std::string_view s, std::size_t& offset)
    {
        if (offset >= s.size()) {
            return 0;
        }

        const auto bytes = s;
        const unsigned char lead = static_cast<unsigned char>(bytes[offset]);

        // 1 バイト (ASCII): 0xxxxxxx
        if (lead < 0x80u) {
            ++offset;
            return static_cast<char32_t>(lead);
        }

        // 継続バイト (10xxxxxx) が先頭に来るのは不正 (文字の途中から読み始めた場合)。
        if (lead < 0xC0u) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 先頭バイトから系列長と初期ビットを決める。
        // 110xxxxx → 2 バイト、1110xxxx → 3 バイト、11110xxx → 4 バイト、
        // 0xF8 以降は UTF-8 に存在しない。
        std::size_t length  = 0;
        char32_t    result  = 0;
        // この長さで表せる最小値を下回るコードポイントは、冗長符号化として扱う。
        char32_t    minimum = 0;
        if (lead < 0xE0u) {
            length = 2; result = lead & 0x1Fu; minimum = 0x80u;
        } else if (lead < 0xF0u) {
            length = 3; result = lead & 0x0Fu; minimum = 0x800u;
        } else if (lead < 0xF8u) {
            length = 4; result = lead & 0x07u; minimum = 0x10000u;
        } else {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 系列が文字列末尾で途切れている。
        if (offset + length > s.size()) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        // 継続バイトを畳み込む。1 つでも 10xxxxxx でなければ不正。
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(bytes[offset + i]);
            if ((continuation & 0xC0u) != 0x80u) {
                ++offset;
                return UTF8_REPLACEMENT_CHAR;
            }
            result = (result << 6) | (continuation & 0x3Fu);
        }

        // 冗長符号化 (より短い系列で表せる値) / サロゲート領域 / 範囲外を弾く。
        // 冗長符号化を通すと同じ文字が複数のバイト列で表現できてしまい、
        // 文字列比較やフィルタリングの前提が崩れる (セキュリティ上の定石)。
        if (result < minimum || result > UTF8_MAX_CODEPOINT
            || (result >= 0xD800u && result <= 0xDFFFu)) {
            ++offset;
            return UTF8_REPLACEMENT_CHAR;
        }

        offset += length;
        return result;
    }

    /// @brief 文字列に含まれるコードポイント数を数える (バイト数ではない)。
    /// @param[in] s 数える UTF-8 バイト列。不正なバイトはそれぞれ1文字として数える。
    [[nodiscard]] static std::size_t Length(std::string_view s)
    {
        std::size_t offset = 0;
        std::size_t count  = 0;
        while (offset < s.size()) {
            // 戻り値は使わず offset を進めるためだけに呼ぶ。[[nodiscard]] を明示的に
            // 捨てる (/W4 の C4834 対策)。
            static_cast<void>(Decode(s, offset));
            ++count;
        }
        return count;
    }

    /// @brief 1 コードポイントを UTF-8 バイト列として out の末尾へ追記する。
    /// @param[in,out] out 既存のバイト列に続けて UTF-8 を書き込む文字列。
    /// @param[in] codePoint 追記するコードポイント。サロゲート値や Unicode の上限を超えた値は U+FFFD として書き出す。
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
