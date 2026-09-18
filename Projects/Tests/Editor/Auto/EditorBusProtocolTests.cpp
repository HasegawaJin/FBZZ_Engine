/// @file    EditorBusProtocolTests.cpp
/// @brief   AI 連携の wire エンベロープ検証と応答生成。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// ここは «外から来た文字列» が最初に通る関門。緩めると、壊れた要求が
/// 既定値のまま Editor の操作として実行される。特に dryRun の既定は
/// «試算» 側でなければならない (欠落しただけで実変更が走ってはいけない)。
#include <TestKit/TestKit.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>

#include <string>

namespace fbzz::tests {
namespace {

using editor::ai::BusRequest;
using editor::ai::JsonValue;
using editor::ai::kEditorProtocol;
using editor::ai::ParseBusRequest;

/// 既定で «通る» 要求。テストごとに 1 か所だけ壊して、そこが弾かれることを見る。
JsonValue MakeEnvelope(const char* kind = "query")
{
    JsonValue payload = JsonValue::MakeObject();
    payload.Set("t", JsonValue("scene.list"));

    JsonValue root = JsonValue::MakeObject();
    root.Set("protocol", JsonValue(kEditorProtocol));
    root.Set("id", JsonValue("req-1"));
    root.Set("kind", JsonValue(kind));
    root.Set("payload", std::move(payload));
    return root;
}

} // namespace

TEST(EditorBusProtocol, AcceptsAWellFormedQuery)
{
    std::string error;
    const auto request = ParseBusRequest(MakeEnvelope("query"), &error);

    ASSERT_TRUE(request.has_value()) << error;
    EXPECT_EQ(request->id, "req-1");
    EXPECT_TRUE(request->IsQuery());
    EXPECT_FALSE(request->IsCommand());
    EXPECT_EQ(request->PayloadType(), "scene.list");
}

TEST(EditorBusProtocol, AcceptsAWellFormedCommand)
{
    const auto request = ParseBusRequest(MakeEnvelope("command"), nullptr);

    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->IsCommand());
    EXPECT_FALSE(request->IsQuery());
}

TEST(EditorBusProtocol, DefaultsDryRunToTheSafeSide)
{
    /// @note dryRun の «欠落» を実変更として扱ってはいけない。壊れた送信側や古い
    ///       クライアントが、黙ってシーンを書き換えることになる。
    const auto request = ParseBusRequest(MakeEnvelope("command"), nullptr);
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->dryRun);
}

TEST(EditorBusProtocol, DefaultsDryRunToTrueWhenItIsNotABool)
{
    JsonValue root = MakeEnvelope("command");
    /// @note 文字列は bool ではない
    root.Set("dryRun", JsonValue("yes"));

    const auto request = ParseBusRequest(root, nullptr);
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->dryRun);
}

TEST(EditorBusProtocol, HonoursAnExplicitDryRunFlag)
{
    JsonValue root = MakeEnvelope("command");
    root.Set("dryRun", JsonValue(false));

    const auto request = ParseBusRequest(root, nullptr);
    ASSERT_TRUE(request.has_value());
    EXPECT_FALSE(request->dryRun);
}

TEST(EditorBusProtocol, RejectsAMismatchedProtocol)
{
    JsonValue root = MakeEnvelope();
    root.Set("protocol", JsonValue("fbzz.editor.v999"));

    std::string error;
    EXPECT_FALSE(ParseBusRequest(root, &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(EditorBusProtocol, RejectsAMissingOrEmptyId)
{
    JsonValue missing = MakeEnvelope();
    /// @note null
    missing.Set("id", JsonValue());
    EXPECT_FALSE(ParseBusRequest(missing, nullptr).has_value());

    JsonValue empty = MakeEnvelope();
    empty.Set("id", JsonValue(""));
    EXPECT_FALSE(ParseBusRequest(empty, nullptr).has_value());
}

TEST(EditorBusProtocol, RejectsAnUnknownKind)
{
    JsonValue root = MakeEnvelope("notify");
    EXPECT_FALSE(ParseBusRequest(root, nullptr).has_value());
}

TEST(EditorBusProtocol, RejectsAPayloadWithoutAType)
{
    JsonValue root = MakeEnvelope();
    /// @note "t" が無い
    root.Set("payload", JsonValue::MakeObject());

    EXPECT_FALSE(ParseBusRequest(root, nullptr).has_value());
}

TEST(EditorBusProtocol, RejectsAPayloadThatIsNotAnObject)
{
    JsonValue root = MakeEnvelope();
    root.Set("payload", JsonValue("scene.list"));

    EXPECT_FALSE(ParseBusRequest(root, nullptr).has_value());
}

TEST(EditorBusProtocol, RejectsAnEnvelopeThatIsNotAnObject)
{
    EXPECT_FALSE(ParseBusRequest(JsonValue("just a string"), nullptr).has_value());
    EXPECT_FALSE(ParseBusRequest(JsonValue::MakeArray(), nullptr).has_value());
}

TEST(EditorBusProtocol, OkResponseCarriesTheCorrelationId)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(3));

    const JsonValue response = editor::ai::MakeOkResponse("req-1", std::move(result));

    EXPECT_EQ(response.Find("protocol")->AsString(), kEditorProtocol);
    EXPECT_EQ(response.Find("id")->AsString(), "req-1");
    EXPECT_TRUE(response.Find("ok")->AsBool());
    ASSERT_NE(response.Find("result"), nullptr);
    EXPECT_EQ(response.Find("result")->Find("count")->AsInt(), 3);
}

TEST(EditorBusProtocol, ErrorResponseCarriesCodeAndMessage)
{
    const JsonValue response =
        editor::ai::MakeErrorResponse("req-1", "not_found", "scene がありません");

    EXPECT_EQ(response.Find("id")->AsString(), "req-1");
    EXPECT_FALSE(response.Find("ok")->AsBool());
    ASSERT_NE(response.Find("error"), nullptr);
    EXPECT_EQ(response.Find("error")->Find("code")->AsString(), "not_found");
    EXPECT_EQ(response.Find("error")->Find("message")->AsString(), "scene がありません");
    /// @note 失敗応答に result を混ぜない (受け手が «部分的に成功» と読む余地を残さない)。
    EXPECT_EQ(response.Find("result"), nullptr);
}

} // namespace fbzz::tests
