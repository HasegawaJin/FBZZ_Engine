/// @file    SceneFindPaginationTests.cpp
/// @brief   Shared scene.find pages reach every matching object without omitted later results or invalid offsets.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>

namespace fbzz::tests {
class SceneFindPaginationTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); AttachScene(); }
    editor::ai::JsonValue Find(const editor::ai::JsonValue& offset) {
        using editor::ai::JsonValue;
        JsonValue payload = JsonValue::MakeObject(); payload.Set("t", JsonValue("scene.find")); payload.Set("name", JsonValue("Reachable")); payload.Set("limit", JsonValue(500)); payload.Set("offset", offset);
        JsonValue request = JsonValue::MakeObject(); request.Set("protocol", JsonValue(editor::ai::kEditorProtocol)); request.Set("id", JsonValue("paged-find"));
        request.Set("kind", JsonValue("query")); request.Set("dryRun", JsonValue(false)); request.Set("payload", payload);
        editor::ai::EditorBusDispatcher dispatcher(Context()); std::string error;
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)), &error);
        EXPECT_TRUE(response.has_value()) << error; return response.value_or(JsonValue{});
    }
};
TEST_F(SceneFindPaginationTest, EveryObjectBeyondFiveHundredIsReachableInStableRequestOrder)
{
    using editor::ai::JsonValue; std::vector<std::string> expected, observed;
    for (int index = 0; index < 1001; index++) expected.push_back(AttachedScene()->CreateGameObject("Reachable" + std::to_string(index)).instanceId);
    int64_t offset = 0;
    for (int page = 0; page < 3; page++) {
        const auto response = Find(JsonValue(offset)); ASSERT_TRUE(response.Find("ok")->AsBool());
        const auto* result = response.Find("result"); ASSERT_NE(result, nullptr); EXPECT_EQ(result->Find("count")->AsInt64(), 1001);
        EXPECT_EQ(result->Find("offset")->AsInt64(), offset); EXPECT_EQ(result->Find("sample")->AsString(), "request");
        const auto* matches = result->Find("matches"); ASSERT_NE(matches, nullptr);
        for (const auto& object : matches->AsArray()) observed.push_back(object.Find("id")->AsString());
        const auto* next = result->Find("nextOffset"); ASSERT_NE(next, nullptr);
        if (page < 2) { ASSERT_TRUE(next->IsInteger()); offset = next->AsInt64(); EXPECT_TRUE(result->Find("truncated")->AsBool()); }
        else { EXPECT_TRUE(next->IsNull()); EXPECT_FALSE(result->Find("truncated")->AsBool()); EXPECT_EQ(matches->AsArray().size(), 1U); }
    }
    EXPECT_EQ(observed, expected);
    const auto beyond = Find(JsonValue(int64_t{1001})); ASSERT_TRUE(beyond.Find("ok")->AsBool());
    EXPECT_TRUE(beyond.Find("result")->Find("matches")->AsArray().empty()); EXPECT_TRUE(beyond.Find("result")->Find("nextOffset")->IsNull());
}
TEST_F(SceneFindPaginationTest, InvalidOffsetsCannotAliasTheFirstPage)
{
    AttachedScene()->CreateGameObject("Reachable");
    for (const auto& value : std::vector<editor::ai::JsonValue>{editor::ai::JsonValue(-1), editor::ai::JsonValue(0.5), editor::ai::JsonValue(true), editor::ai::JsonValue("500")}) {
        const auto result = Find(value); ASSERT_FALSE(result.Find("ok")->AsBool()); EXPECT_EQ(result.Find("error")->Find("code")->AsString(), "BAD_ARG");
    }
}
}
