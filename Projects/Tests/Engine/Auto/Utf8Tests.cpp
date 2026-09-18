/// @file    Utf8Tests.cpp
/// @brief   UTF-8 の復号・符号化と、壊れたバイト列で «必ず前へ進む» ことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// UI テキストは std::string (UTF-8) で持ち、フォントは 1 コードポイント単位で引く。
/// 復号が 1 バイトも進めない経路が 1 つでもあると、そのテキストを表示した瞬間に
/// 描画スレッドが無限ループで固まる ── «特定の文字を含むときだけ固まる» という形になる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/Utf8.hpp>

#include <cstdint>
#include <string>

namespace fbzz::tests {
namespace {

using util::Utf8;

/// 1 文字だけ復号して返す。offset の進み方も一緒に確かめたいときは Decode を直接使う。
char32_t DecodeFirst(std::string_view text)
{
    std::size_t offset = 0;
    return Utf8::Decode(text, offset);
}

} // namespace

class Utf8Test : public testkit::EngineFixture {};

/// @name 復号

TEST_F(Utf8Test, DecodesAsciiAsItself)
{
    EXPECT_EQ(DecodeFirst("A"), U'A');
    EXPECT_EQ(DecodeFirst("~"), U'~');
}

TEST_F(Utf8Test, DecodesTwoThreeAndFourByteSequences)
{
    /// @note é
    EXPECT_EQ(DecodeFirst("\xC3\xA9"), 0x00E9u);
    /// @note あ
    EXPECT_EQ(DecodeFirst("\xE3\x81\x82"), 0x3042u);
    /// @note 🎮
    EXPECT_EQ(DecodeFirst("\xF0\x9F\x8E\xAE"), 0x1F3AEu);
}

TEST_F(Utf8Test, AdvancesTheOffsetByTheSequenceLength)
{
    const std::string text = "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x8E\xAE";
    std::size_t offset = 0;

    static_cast<void>(Utf8::Decode(text, offset));
    EXPECT_EQ(offset, 1u);
    static_cast<void>(Utf8::Decode(text, offset));
    EXPECT_EQ(offset, 3u);
    static_cast<void>(Utf8::Decode(text, offset));
    EXPECT_EQ(offset, 6u);
    static_cast<void>(Utf8::Decode(text, offset));
    EXPECT_EQ(offset, 10u);
}

TEST_F(Utf8Test, DecodesZeroPastTheEnd)
{
    std::size_t offset = 3;
    EXPECT_EQ(Utf8::Decode("abc", offset), 0u);
}

/// @name 壊れた入力

TEST_F(Utf8Test, ReplacesAContinuationByteThatStartsASequence)
{
    /// @note 文字の途中から読み始めた場合。ここで戻らずに «読めなかった» と伝える。
    EXPECT_EQ(DecodeFirst("\xA0"), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, ReplacesATruncatedSequence)
{
    EXPECT_EQ(DecodeFirst("\xE3\x81"), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, ReplacesAnOverlongEncoding)
{
    /// @note "/" を 2 バイトで書いたもの。通すとパス比較をすり抜ける古典的な穴になる。
    EXPECT_EQ(DecodeFirst("\xC0\xAF"), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, ReplacesASurrogateHalf)
{
    EXPECT_EQ(DecodeFirst("\xED\xA0\x80"), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, ReplacesACodePointBeyondUnicode)
{
    EXPECT_EQ(DecodeFirst("\xF5\x80\x80\x80"), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, AlwaysAdvancesAtLeastOneByteOnBrokenInput)
{
    /// @note これが最も大事な契約。1 バイトも進めない経路があると、その文字列の描画で固まる。
    const std::string broken = "\xA0\xC0\xAF\xE3\x81\xED\xA0\x80\xF5\x80";

    std::size_t offset = 0;
    std::size_t guard  = 0;
    while (offset < broken.size()) {
        const std::size_t before = offset;
        static_cast<void>(Utf8::Decode(broken, offset));

        ASSERT_GT(offset, before) << "offset " << before << " で止まった";
        ASSERT_LT(++guard, broken.size() + 1) << "1 文字あたり 1 バイト未満しか進んでいない";
    }
}

/// @name 文字数

TEST_F(Utf8Test, CountsCodePointsNotBytes)
{
    /// @note "あA🎮" は 3 文字 8 バイト。バイト数で折り返すとカーソルが文字の途中へ入る。
    EXPECT_EQ(Utf8::Length("\xE3\x81\x82" "A" "\xF0\x9F\x8E\xAE"), 3u);
    EXPECT_EQ(Utf8::Length(""), 0u);
    EXPECT_EQ(Utf8::Length("abc"), 3u);
}

TEST_F(Utf8Test, CountsBrokenBytesAsOneCharacterEach)
{
    EXPECT_EQ(Utf8::Length("\xA0\xA0"), 2u);
}

/// @name 符号化

TEST_F(Utf8Test, AppendsEachSequenceLength)
{
    std::string out;
    Utf8::Append(out, U'A');
    EXPECT_EQ(out, "A");

    out.clear();
    Utf8::Append(out, 0x3042u);
    EXPECT_EQ(out, "\xE3\x81\x82");

    out.clear();
    Utf8::Append(out, 0x1F3AEu);
    EXPECT_EQ(out, "\xF0\x9F\x8E\xAE");
}

TEST_F(Utf8Test, AppendWritesTheReplacementCharForInvalidCodePoints)
{
    std::string surrogate;
    std::string tooLarge;
    Utf8::Append(surrogate, 0xD800u);
    Utf8::Append(tooLarge, 0x110000u);

    EXPECT_EQ(DecodeFirst(surrogate), util::UTF8_REPLACEMENT_CHAR);
    EXPECT_EQ(DecodeFirst(tooLarge), util::UTF8_REPLACEMENT_CHAR);
}

TEST_F(Utf8Test, EncodingRoundTripsThroughDecoding)
{
    const char32_t samples[] = { U'A', 0x00E9u, 0x3042u, 0xFFFDu, 0x1F3AEu, 0x10FFFFu };

    for (const char32_t code : samples) {
        std::string encoded;
        Utf8::Append(encoded, code);

        EXPECT_EQ(DecodeFirst(encoded), code) << "U+" << std::hex << static_cast<uint32_t>(code);
        EXPECT_EQ(Utf8::Length(encoded), 1u);
    }
}

} // namespace fbzz::tests
