/// @file    PlaytestJsonTests.cpp
/// @brief   Playtest の表明に使う JSON パス解決と比較演算を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>

#include <Editor/Ai/Serialization/Json.hpp>
#include <Editor/Playtest/PlaytestJson.hpp>

#include <string>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;
using editor::ai::ParseJson;
using editor::playtest::EvaluateJsonCondition;
using editor::playtest::ResolveJsonPath;

class PlaytestJsonPointerTest : public testkit::Fixture {};

JsonValue Parse(const std::string& text)
{
    auto parsed = ParseJson(text, nullptr);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(JsonValue{});
}

bool Holds(const JsonValue& root, const std::string& path, const std::string& op, const std::string& expectedJson)
{
    const JsonValue expected = Parse(expectedJson);
    std::string detail;
    return EvaluateJsonCondition(ResolveJsonPath(root, path), op, &expected, detail);
}

} /// @note namespace

/// @brief Resolves qualified observation keys for numeric conditions.
TEST_F(PlaytestJsonPointerTest, ResolvesQualifiedObservationKeysForNumericConditions)
{
    const JsonValue root = Parse(R"({"fields":{"ImportedSamples.OceanEcosystem.MarineEcosystemComponent.observedSeconds":30.5}})");
    const std::string path = "/fields/ImportedSamples.OceanEcosystem.MarineEcosystemComponent.observedSeconds";

    ASSERT_NE(ResolveJsonPath(root, path), nullptr);
    EXPECT_TRUE(Holds(root, path, ">=", "30"));
    EXPECT_FALSE(Holds(root, path, ">=", "60"));
    EXPECT_EQ(ResolveJsonPath(root, "fields.observedSeconds"), nullptr);
}

/// @brief Decodes escaped tokens once and resolves array elements.
TEST_F(PlaytestJsonPointerTest, DecodesEscapedTokensOnceAndResolvesArrayElements)
{
    const JsonValue root = Parse(R"({"a/b":{"m~n":["first",{"~1":42}]}})");

    const JsonValue* first = ResolveJsonPath(root, "/a~1b/m~0n/0");
    const JsonValue* escaped = ResolveJsonPath(root, "/a~1b/m~0n/1/~01");

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->AsString(), "first");
    ASSERT_NE(escaped, nullptr);
    EXPECT_EQ(escaped->AsInt(), 42);
}

/// @brief Resolves empty tokens and preserves empty root path.
TEST_F(PlaytestJsonPointerTest, ResolvesEmptyTokensAndPreservesEmptyRootPath)
{
    const JsonValue root = Parse(R"({"":{"":7},"nodes":[{"":9}]})");

    EXPECT_EQ(ResolveJsonPath(root, ""), &root);
    ASSERT_NE(ResolveJsonPath(root, "/"), nullptr);
    EXPECT_TRUE(ResolveJsonPath(root, "/")->IsObject());
    ASSERT_NE(ResolveJsonPath(root, "//"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "//")->AsInt(), 7);
    ASSERT_NE(ResolveJsonPath(root, "/nodes/0/"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "/nodes/0/")->AsInt(), 9);
}

/// @brief Rejects malformed escapes and invalid array indices.
TEST_F(PlaytestJsonPointerTest, RejectsMalformedEscapesAndInvalidArrayIndices)
{
    const JsonValue root = Parse(R"({"nodes":[{"name":"Player"}],"a~":1,"a~2":2})");

    for (const char* path : {"/a~", "/a~2", "/nodes/0/name~", "/nodes/0/name~x", "/nodes/01", "/nodes/-1", "/nodes/+0",
                             "/nodes/-", "/nodes/", "/nodes/x", "/nodes/1", "/nodes/184467440737095516160", "/nodes/0/name/deeper", "/missing"}) {
        EXPECT_EQ(ResolveJsonPath(root, path), nullptr) << path;
    }
}

/// @brief Preserves dotted paths without pointer decoding.
TEST_F(PlaytestJsonPointerTest, PreservesDottedPathsWithoutPointerDecoding)
{
    const JsonValue root = Parse(R"({"nodes":[{"name":"Player"},{"name":"Boss"}],"a~1b":{"count":3},"a/b":{"count":4}})");

    const JsonValue* dotted = ResolveJsonPath(root, "nodes.1.name");
    ASSERT_NE(dotted, nullptr);
    EXPECT_EQ(dotted, ResolveJsonPath(root, "/nodes/1/name"));
    ASSERT_NE(ResolveJsonPath(root, "a~1b.count"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "a~1b.count")->AsInt(), 3);
    ASSERT_NE(ResolveJsonPath(root, "a/b.count"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "a/b.count")->AsInt(), 4);
}

/// @brief Resolves object keys and array indices.
TEST(PlaytestJsonPath, ResolvesObjectKeysAndArrayIndices)
{
    const JsonValue root = Parse(R"({"nodes":[{"name":"Player"},{"name":"Boss"}],"count":2})");

    ASSERT_NE(ResolveJsonPath(root, "nodes.1.name"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "nodes.1.name")->AsString(), "Boss");
    EXPECT_EQ(ResolveJsonPath(root, "count")->AsInt(), 2);
    EXPECT_EQ(ResolveJsonPath(root, ""), &root);
}

/// @brief Returns null for missing or out-of-range segments.
TEST(PlaytestJsonPath, ReturnsNullForMissingOrOutOfRangeSegments)
{
    const JsonValue root = Parse(R"({"nodes":[{"name":"Player"}]})");

    EXPECT_EQ(ResolveJsonPath(root, "nodes.5.name"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "nodes.x"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "missing.name"), nullptr);
    EXPECT_EQ(ResolveJsonPath(root, "nodes.0.name.deeper"), nullptr);
}

/// @brief Exists and missing do not need a value.
TEST(PlaytestJsonCondition, ExistsAndMissingDoNotNeedAValue)
{
    const JsonValue root = Parse(R"({"a":1})");
    std::string detail;

    EXPECT_TRUE(EvaluateJsonCondition(ResolveJsonPath(root, "a"), "exists", nullptr, detail));
    EXPECT_FALSE(EvaluateJsonCondition(ResolveJsonPath(root, "b"), "exists", nullptr, detail));
    EXPECT_TRUE(EvaluateJsonCondition(ResolveJsonPath(root, "b"), "missing", nullptr, detail));
}

/// @brief Compares numbers with tolerance for round-trips.
TEST(PlaytestJsonCondition, ComparesNumbersWithToleranceForRoundTrips)
{
    const JsonValue root = Parse(R"({"hp":0.30000000000000004,"score":120})");

    EXPECT_TRUE(Holds(root, "hp", "==", "0.3"));
    EXPECT_TRUE(Holds(root, "score", ">=", "120"));
    EXPECT_FALSE(Holds(root, "score", "<", "100"));
    EXPECT_TRUE(Holds(root, "score", "!=", "121"));
}

/// @brief Contains works on strings and arrays.
TEST(PlaytestJsonCondition, ContainsWorksOnStringsAndArrays)
{
    const JsonValue root = Parse(R"({"message":"Stage cleared","tags":["Player","Enemy"]})");

    EXPECT_TRUE(Holds(root, "message", "contains", R"("clear")"));
    EXPECT_TRUE(Holds(root, "tags", "contains", R"("Enemy")"));
    EXPECT_FALSE(Holds(root, "tags", "contains", R"("Boss")"));
}

/// @brief Length operators count arrays and strings.
TEST(PlaytestJsonCondition, LengthOperatorsCountArraysAndStrings)
{
    const JsonValue root = Parse(R"({"entries":[1,2,3],"empty":[]})");

    EXPECT_TRUE(Holds(root, "entries", "length==", "3"));
    EXPECT_TRUE(Holds(root, "empty", "length==", "0"));
    EXPECT_TRUE(Holds(root, "entries", "length>=", "2"));
    EXPECT_FALSE(Holds(root, "entries", "length<=", "2"));
}

/// @brief Explains why a condition failed.
TEST(PlaytestJsonCondition, ExplainsWhyAConditionFailed)
{
    const JsonValue root = Parse(R"({"state":"playing"})");
    const JsonValue expected = Parse(R"("editor")");
    std::string detail;

    EXPECT_FALSE(EvaluateJsonCondition(ResolveJsonPath(root, "state"), "==", &expected, detail));
    EXPECT_NE(detail.find("playing"), std::string::npos) << detail;
}

/// @brief Rejects unknown operators.
TEST(PlaytestJsonCondition, RejectsUnknownOperators)
{
    const JsonValue root = Parse(R"({"a":1})");
    EXPECT_FALSE(Holds(root, "a", "~=", "1"));
}

} /// @note namespace fbzz::tests
