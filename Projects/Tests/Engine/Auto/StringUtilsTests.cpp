/// @file    StringUtilsTests.cpp
/// @brief   文字列ユーティリティ (検索・大文字小文字無視の比較・分割・トリム・ワイド変換) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// アセットパス・タグ・スクリプト名の比較がここを通る。大文字小文字の扱いと
/// 分割の境界 (空要素・末尾の区切り) は、壊れても «たまに一致しない» としか見えない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/StringUtils.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {

using util::StringUtils;

class StringUtilsTest : public testkit::EngineFixture {};

/// @name 検索

TEST_F(StringUtilsTest, ContainsIsCaseSensitiveAndContainsCIIsNot)
{
    EXPECT_TRUE(StringUtils::Contains("PlayerComponent", "Component"));
    EXPECT_FALSE(StringUtils::Contains("PlayerComponent", "component"));
    EXPECT_TRUE(StringUtils::ContainsCI("PlayerComponent", "component"));
}

TEST_F(StringUtilsTest, EveryStringContainsTheEmptyString)
{
    EXPECT_TRUE(StringUtils::Contains("abc", ""));
    EXPECT_TRUE(StringUtils::ContainsCI("abc", ""));
}

TEST_F(StringUtilsTest, PrefixAndSuffixTestsMatchTheWholeEdge)
{
    EXPECT_TRUE(StringUtils::StartsWith("Assets/Scenes/Main.scene", "Assets/"));
    EXPECT_FALSE(StringUtils::StartsWith("Assets/Scenes/Main.scene", "scenes"));
    EXPECT_TRUE(StringUtils::EndsWith("Assets/Scenes/Main.scene", ".scene"));
    EXPECT_FALSE(StringUtils::EndsWith("Assets/Scenes/Main.scene", ".mat"));
}

TEST_F(StringUtilsTest, PrefixAndSuffixRejectAffixesLongerThanTheString)
{
    EXPECT_FALSE(StringUtils::StartsWith("ab", "abc"));
    EXPECT_FALSE(StringUtils::EndsWith("ab", "abc"));
}

/// @name 比較と変換

TEST_F(StringUtilsTest, EqualsCIIgnoresCaseOnBothSides)
{
    EXPECT_TRUE(StringUtils::EqualsCI("Main.Scene", "MAIN.scene"));
    EXPECT_FALSE(StringUtils::EqualsCI("Main.scene", "Main.mat"));
    EXPECT_FALSE(StringUtils::EqualsCI("Main", "Main2"));
}

TEST_F(StringUtilsTest, CaseConversionLeavesNonLettersAlone)
{
    EXPECT_EQ(StringUtils::ToLower("Assets/UI_01.PNG"), "assets/ui_01.png");
    EXPECT_EQ(StringUtils::ToUpper("Assets/UI_01.png"), "ASSETS/UI_01.PNG");
}

TEST_F(StringUtilsTest, WideConversionRoundTrips)
{
    /// @note パスは Windows API へ渡すたびにこの往復を通る。落とすとファイルが開けない。
    const std::string original = "Assets/Scenes/Main.scene";

    EXPECT_EQ(StringUtils::ToNarrow(StringUtils::ToWide(original)), original);
}

TEST_F(StringUtilsTest, WideConversionSurvivesAnEmptyString)
{
    EXPECT_TRUE(StringUtils::ToWide("").empty());
    EXPECT_TRUE(StringUtils::ToNarrow(std::wstring{}).empty());
}

/// @name 分割

TEST_F(StringUtilsTest, SplitBreaksOnEveryDelimiter)
{
    const std::vector<std::string> parts = StringUtils::Split("a,b,c", ',');

    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST_F(StringUtilsTest, SplitKeepsEmptyFieldsInTheMiddle)
{
    /// @note "a,,b" は 3 列。空の列を落とすと、CSV 的な並びの列がずれる。
    const std::vector<std::string> parts = StringUtils::Split("a,,b", ',');

    ASSERT_EQ(parts.size(), 3u);
    EXPECT_TRUE(parts[1].empty());
}

TEST_F(StringUtilsTest, SplitDropsTheEmptyTailAfterATrailingDelimiter)
{
    /// @note 末尾の区切りは «空の列» を作らない契約。
    const std::vector<std::string> parts = StringUtils::Split("a,b,", ',');

    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[1], "b");
}

TEST_F(StringUtilsTest, SplitOfATextWithoutTheDelimiterIsTheTextItself)
{
    const std::vector<std::string> parts = StringUtils::Split("abc", ',');

    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], "abc");
}

TEST_F(StringUtilsTest, SplitOfAnEmptyStringIsEmpty)
{
    EXPECT_TRUE(StringUtils::Split("", ',').empty());
}

/// @name トリム

TEST_F(StringUtilsTest, TrimRemovesSurroundingWhitespace)
{
    EXPECT_EQ(StringUtils::Trim("  hello  "), "hello");
    EXPECT_EQ(StringUtils::Trim("\t\r\nhello\n"), "hello");
}

TEST_F(StringUtilsTest, TrimKeepsInnerWhitespace)
{
    EXPECT_EQ(StringUtils::Trim("  a b  "), "a b");
}

TEST_F(StringUtilsTest, TrimOfBlankTextIsEmpty)
{
    EXPECT_EQ(StringUtils::Trim("   \t\n"), "");
    EXPECT_EQ(StringUtils::Trim(""), "");
}

} // namespace fbzz::tests
