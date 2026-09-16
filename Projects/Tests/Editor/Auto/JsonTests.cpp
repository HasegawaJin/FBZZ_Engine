/// @file    JsonTests.cpp
/// @brief   AI 連携が使う自前 JSON の解析・直列化・Base64 を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// このパーサは Editor と MCP の間の唯一の通信形式。壊れても «AI から操作できない» という
/// 形でしか出ず、原因がプロトコルなのか操作側なのか切り分けられない。
/// 往復して同じ値に戻ること、壊れた入力を «成功» と言わないことをここで固定する。
#include <TestKit/TestKit.hpp>

#include <Editor/Ai/Json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;
using editor::ai::ParseJson;
using editor::ai::SerializeJson;

JsonValue MustParse(const std::string& text)
{
    std::string error;
    auto parsed = ParseJson(text, &error);
    EXPECT_TRUE(parsed.has_value()) << text << " -> " << error;
    return parsed.value_or(JsonValue{});
}

} // namespace

// --- 解析 -------------------------------------------------------------------

TEST(JsonParse, ReadsEachScalarType)
{
    EXPECT_TRUE(MustParse("null").IsNull());
    EXPECT_TRUE(MustParse("true").AsBool());
    EXPECT_FALSE(MustParse("false").AsBool());
    EXPECT_DOUBLE_EQ(MustParse("42").AsNumber(), 42.0);
    EXPECT_EQ(MustParse("\"hello\"").AsString(), "hello");
}

TEST(JsonParse, ReadsNegativeAndFractionalNumbers)
{
    EXPECT_DOUBLE_EQ(MustParse("-17").AsNumber(), -17.0);
    EXPECT_DOUBLE_EQ(MustParse("0.5").AsNumber(), 0.5);
    EXPECT_DOUBLE_EQ(MustParse("-0.25").AsNumber(), -0.25);
    EXPECT_DOUBLE_EQ(MustParse("1e3").AsNumber(), 1000.0);
}

TEST(JsonParse, ReadsAnObjectAndFindsMembersByKey)
{
    const JsonValue root = MustParse(R"({"t":"ping","n":3,"ok":true})");
    ASSERT_TRUE(root.IsObject());

    ASSERT_NE(root.Find("t"), nullptr);
    EXPECT_EQ(root.Find("t")->AsString(), "ping");
    EXPECT_EQ(root.Find("n")->AsInt(), 3);
    EXPECT_TRUE(root.Find("ok")->AsBool());
    EXPECT_EQ(root.Find("missing"), nullptr);
}

TEST(JsonParse, ReadsNestedArraysAndObjects)
{
    const JsonValue root = MustParse(R"({"items":[{"id":1},{"id":2}]})");
    const JsonValue* items = root.Find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_TRUE(items->IsArray());
    ASSERT_EQ(items->AsArray().size(), 2u);
    EXPECT_EQ(items->AsArray()[1].Find("id")->AsInt(), 2);
}

TEST(JsonParse, IgnoresWhitespaceBetweenTokens)
{
    const JsonValue root = MustParse("  {\n \"a\" : [ 1 , 2 ]\t}\r\n");
    ASSERT_NE(root.Find("a"), nullptr);
    EXPECT_EQ(root.Find("a")->AsArray().size(), 2u);
}

TEST(JsonParse, ReadsEscapeSequencesInStrings)
{
    const JsonValue root = MustParse(R"({"s":"a\"b\\c\nd\tE"})");
    EXPECT_EQ(root.Find("s")->AsString(), "a\"b\\c\nd\tE");
}

TEST(JsonParse, ReadsRawUtf8InStrings)
{
    // MCP はパス・アセット名に日本語を載せる。ここが壊れると名前が化ける。
    const JsonValue root = MustParse(R"({"s":"あ"})");
    EXPECT_EQ(root.Find("s")->AsString(), "\xE3\x81\x82");  // U+3042 'あ'
}

TEST(JsonParse, DecodesUnicodeEscapesToUtf8)
{
    // 送信側が \uXXXX で寄こす場合。生の UTF-8 と同じ結果にならなければならない。
    const JsonValue root = MustParse(R"({"s":"\u3042"})");
    EXPECT_EQ(root.Find("s")->AsString(), "\xE3\x81\x82");
}

TEST(JsonParse, DecodesAsciiAndTwoByteUnicodeEscapes)
{
    EXPECT_EQ(MustParse(R"("\u0041")").AsString(), "A");            // 1 バイト
    EXPECT_EQ(MustParse(R"("\u00E9")").AsString(), "\xC3\xA9");     // 2 バイト (é)
}

TEST(JsonParse, CombinesSurrogatePairs)
{
    // 絵文字は上位 + 下位の 2 つ組で来る。片方だけを文字にすると化ける。
    const JsonValue root = MustParse(R"("\uD83D\uDE00")");          // U+1F600 😀
    EXPECT_EQ(root.AsString(), "\xF0\x9F\x98\x80");
}

TEST(JsonParse, RejectsABrokenSurrogatePair)
{
    // 上位だけ、あるいは下位が下位でない組み合わせ。黙って化けさせない。
    EXPECT_FALSE(ParseJson(R"("\uD83D")", nullptr).has_value());
    EXPECT_FALSE(ParseJson(R"("\uD83DA")", nullptr).has_value());
}

TEST(JsonParse, RejectsAShortUnicodeEscape)
{
    EXPECT_FALSE(ParseJson(R"("\u30")", nullptr).has_value());
    EXPECT_FALSE(ParseJson(R"("\uZZZZ")", nullptr).has_value());
}

TEST(JsonParse, ReadsAnEmptyContainer)
{
    EXPECT_TRUE(MustParse("{}").IsObject());
    EXPECT_TRUE(MustParse("[]").IsArray());
    EXPECT_EQ(MustParse("[]").AsArray().size(), 0u);
}

TEST(JsonParse, RejectsMalformedInputInsteadOfGuessing)
{
    // 中途半端に読めた «つもり» で返すと、欠けた値が既定値として通ってしまう。
    const char* broken[] = {
        "",  "{",  "}",  "[",  "[1,",  "{\"a\"}",  "{\"a\":}",
        "{a:1}",  "tru",  "\"unterminated",  "--1",
    };
    for (const char* text : broken) {
        std::string error;
        EXPECT_FALSE(ParseJson(text, &error).has_value()) << "accepted: " << text;
    }
}

TEST(JsonParse, ReportsAReasonWhenItFails)
{
    std::string error;
    EXPECT_FALSE(ParseJson("{", &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(JsonParse, WorksWithoutAnErrorSink)
{
    // error は省略可能。null 渡しで落ちないこと。
    EXPECT_FALSE(ParseJson("{", nullptr).has_value());
    EXPECT_TRUE(ParseJson("{}", nullptr).has_value());
}

// --- 直列化 -----------------------------------------------------------------

TEST(JsonSerialize, RoundTripsAnObject)
{
    const std::string text = R"({"t":"cmd","n":3,"ok":true,"arr":[1,2],"nil":null})";
    const JsonValue once  = MustParse(text);
    const JsonValue twice = MustParse(SerializeJson(once));

    EXPECT_EQ(twice.Find("t")->AsString(), "cmd");
    EXPECT_EQ(twice.Find("n")->AsInt(), 3);
    EXPECT_TRUE(twice.Find("ok")->AsBool());
    EXPECT_EQ(twice.Find("arr")->AsArray().size(), 2u);
    EXPECT_TRUE(twice.Find("nil")->IsNull());
}

TEST(JsonSerialize, EscapesCharactersThatWouldBreakTheDocument)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("s", JsonValue(std::string("quote\" back\\ newline\n tab\t")));

    // 往復して元に戻れば、エスケープと解除が噛み合っている。
    const JsonValue back = MustParse(SerializeJson(root));
    EXPECT_EQ(back.Find("s")->AsString(), "quote\" back\\ newline\n tab\t");
}

TEST(JsonSerialize, KeepsUtf8Intact)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("s", JsonValue(std::string("日本語のアセット名")));

    const JsonValue back = MustParse(SerializeJson(root));
    EXPECT_EQ(back.Find("s")->AsString(), "日本語のアセット名");
}

TEST(JsonSerialize, WritesEmptyContainers)
{
    EXPECT_EQ(SerializeJson(JsonValue::MakeObject()), "{}");
    EXPECT_EQ(SerializeJson(JsonValue::MakeArray()), "[]");
}

// --- 値の組み立て -----------------------------------------------------------

TEST(JsonValueBuild, SetReplacesAnExistingKeyInPlace)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("a", JsonValue(1));
    root.Set("a", JsonValue(2));

    EXPECT_EQ(root.AsObject().size(), 1u);
    EXPECT_EQ(root.Find("a")->AsInt(), 2);
}

TEST(JsonValueBuild, PushTurnsTheValueIntoAnArray)
{
    JsonValue value;
    ASSERT_TRUE(value.IsNull());
    value.Push(JsonValue(1));

    EXPECT_TRUE(value.IsArray());
    EXPECT_EQ(value.AsArray().size(), 1u);
}

TEST(JsonValueBuild, AccessorsFallBackWhenTheTypeDiffers)
{
    // 相手の型を取り違えても «それらしい値» を返さず、指定した既定値に落ちること。
    const JsonValue text{"not a number"};
    EXPECT_DOUBLE_EQ(text.AsNumber(-1.0), -1.0);
    EXPECT_EQ(text.AsInt(-1), -1);
    EXPECT_TRUE(text.AsBool(true));

    const JsonValue number{7};
    EXPECT_EQ(number.AsString(), "");
}

// --- Base64 -----------------------------------------------------------------

TEST(JsonBase64, MatchesTheKnownVectors)
{
    // RFC 4648 の例。スクリーンショット転送が化けたときに «どちら側か» を切り分けられる。
    const auto encode = [](const std::string& text) {
        return editor::ai::Base64Encode(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    };
    EXPECT_EQ(encode(""),       "");
    EXPECT_EQ(encode("f"),      "Zg==");
    EXPECT_EQ(encode("fo"),     "Zm8=");
    EXPECT_EQ(encode("foo"),    "Zm9v");
    EXPECT_EQ(encode("foob"),   "Zm9vYg==");
    EXPECT_EQ(encode("fooba"),  "Zm9vYmE=");
    EXPECT_EQ(encode("foobar"), "Zm9vYmFy");
}

TEST(JsonBase64, EncodesBytesAboveAscii)
{
    const std::vector<std::uint8_t> bytes{ 0x00, 0xFF, 0x80, 0x7F };
    EXPECT_EQ(editor::ai::Base64Encode(bytes), "AP+Afw==");
}

} // namespace fbzz::tests
