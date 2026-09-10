/// @file    EditorBusDispatcherTests.cpp
/// @brief   AI 連携ディスパッチャが «壊れた文脈» で落ちないことを網羅で確かめる。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// Play 中はアクティブシーンが差し替わる。SceneManager::CurrentScene() は
/// m_externalScene と m_active の «どちらも無い» 瞬間に nullptr を返し、EditorApp は
/// その値をそのまま EditorContext::activeScene へ入れる (Play 中の LoadScene がまさにこれ)。
/// 一方 DrainRequests は毎フレーム走るので、その瞬間に届いた AI コマンドは
/// nullptr のシーンを掴む。ここで «落ちずにエラーを返す» ことを 1 コマンドずつ固定する。
///
/// WHY 網羅するか: ハンドラは 125 個あり、シーンを触るものと触らないものが混在している。
///     どれか 1 つでもガードを忘れると «Play 中に AI から操作するとエディターが落ちる» に
///     なるが、落ちるまでどれが穴かは分からない。1 つずつ投げて確かめるしかない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/EditorContext.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::ai::EditorBusDispatcher;
using editor::ai::JsonValue;
using editor::ai::ParseJson;
using editor::ai::SerializeJson;

// 実運用と同じ NDJSON エンベロープを 1 行組み立てる。
std::string MakeRequest(const std::string& type, const std::string& kind = "query",
                        bool dryRun = true)
{
    JsonValue payload = JsonValue::MakeObject();
    payload.Set("t", JsonValue(type));

    JsonValue root = JsonValue::MakeObject();
    root.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
    root.Set("id", JsonValue("test-1"));
    root.Set("kind", JsonValue(kind));
    root.Set("dryRun", JsonValue(dryRun));
    root.Set("payload", std::move(payload));
    return SerializeJson(root);
}

// 応答が «プロトコルとして読める» ことまで見る。落ちないだけでは足りない
// (空文字や壊れた JSON を返すと、MCP 側が固まる)。
void ExpectWellFormedResponse(const std::string& line, const std::string& what)
{
    ASSERT_FALSE(line.empty()) << what << ": 応答が空";

    std::string error;
    const auto parsed = ParseJson(line, &error);
    ASSERT_TRUE(parsed.has_value()) << what << ": 応答が JSON ではない (" << error << ") "
                                    << line.substr(0, 200);

    const JsonValue* ok = parsed->Find("ok");
    ASSERT_NE(ok, nullptr) << what << ": ok が無い";
    EXPECT_TRUE(ok->IsBool()) << what << ": ok が bool ではない";

    const JsonValue* id = parsed->Find("id");
    ASSERT_NE(id, nullptr) << what << ": id が無い";
    EXPECT_EQ(id->AsString(), "test-1") << what << ": 相関 id が返らない";
}

// Dispatcher が受け付ける payload.t の一覧。
// NOTE: 実装の文字列リテラルから拾った 125 種。増えたらここへ足す。
const std::vector<std::string>& AllCommandTypes()
{
    static const std::vector<std::string> types = {
        // @@COMMANDS_BEGIN
        "animation.addLayer", "animation.addMotion", "animation.addParameter",
        "animation.addState", "animation.addTransition", "animation.blendTree",
        "animation.control", "animation.graph", "animation.playSlot", "animation.pose",
        "animation.removeLayer", "animation.removeMotion", "animation.removeParameter",
        "animation.removeState", "animation.removeTransition", "animation.setCondition",
        "animation.setLayer", "animation.setMotion", "animation.setParameter",
        "animation.setState", "animation.state", "animation.stopSlot", "asset.findUnused",
        "asset.import", "asset.inspect", "asset.list", "asset.thumbnail", "audio.control",
        "audio.inspect", "avatarMask.get", "avatarMask.write", "bt.autoLayout",
        "bt.blackboard.add", "bt.blackboard.remove", "bt.diff", "bt.guide", "bt.lint",
        "bt.node.add", "bt.node.duplicate", "bt.node.remove", "bt.node.setField",
        "bt.node.setOrder", "bt.node.setParent", "bt.nodeField", "bt.repair", "bt.runtime",
        "bt.schema", "bt.template.apply", "bt.templateCatalog", "bt.tree", "build.run",
        "build.status", "component.add", "component.remove", "component.set", "console.logs",
        "edit.redo", "edit.undo", "editor.catalog", "editor.catalog.search",
        "editor.op.invoke", "editor.op.list", "editor.op.query", "editor.redo", "editor.state",
        "editor.transaction", "editor.undo", "editor.undoHistory", "environment.inspect",
        "input.inject", "material.asset.setShader", "material.assign", "material.inspect",
        "material.override", "navmesh.bake", "navmesh.path", "navmesh.sample", "navmesh.state",
        "node.components", "node.create", "node.delete", "node.duplicate", "node.rename",
        "node.reparent", "node.setActive", "node.setLayer", "node.setTag", "physics.events",
        "physics.overlapSphere", "physics.raycast", "play.control", "prefab.apply",
        "prefab.create", "prefab.instantiate", "prefab.revert", "preset.catalog",
        "preset.create", "profiler.snapshot", "scene.find", "scene.list", "scene.open",
        "scene.save", "scene.selection", "scene.snapshot", "scene.tree", "scene.validate",
        "script.reload", "selection.set", "shader.diagnostics", "shader.inspect",
        "sprite.list", "sprite.rename", "sprite.slice", "sprite.thumbnail", "terrain.inspect",
        "terrain.paint", "terrain.sample", "terrain.sculpt", "terrain.setLayerMaterial",
        "transform.set", "ui.inspect", "vfx.generateMotionVectors", "viewport.camera",
        "viewport.capture", "viewport.semantic",
        // @@COMMANDS_END
    };
    return types;
}

} // namespace

// 既定は «何も繋がっていない» 文脈。Play 中にシーンが差し替わる谷間そのもの。
class EditorBusDispatcherTest : public testkit::EditorFixture {};

TEST_F(EditorBusDispatcherTest, SurvivesEveryCommandWithoutAScene)
{
    // 本題。1 つでもガードが抜けていれば、ここでプロセスごと落ちる。
    EditorBusDispatcher dispatcher(Context());
    ASSERT_EQ(Context().activeScene, nullptr);

    for (const std::string& type : AllCommandTypes()) {
        const std::string response = dispatcher.Handle(MakeRequest(type, "query"));
        ExpectWellFormedResponse(response, type);
        if (::testing::Test::HasFatalFailure()) return;
    }
}

TEST_F(EditorBusDispatcherTest, SurvivesEveryCommandAsADryRunCommand)
{
    // query だけでなく command 側の分岐も通す。dryRun なので実変更は起きない。
    EditorBusDispatcher dispatcher(Context());

    for (const std::string& type : AllCommandTypes()) {
        const std::string response =
            dispatcher.Handle(MakeRequest(type, "command", /*dryRun=*/true));
        ExpectWellFormedResponse(response, type + " (command)");
        if (::testing::Test::HasFatalFailure()) return;
    }
}

TEST_F(EditorBusDispatcherTest, SurvivesEveryCommandWithASceneAttached)
{
    // シーンが «有る» 側も通す。無い側だけ守っても、シーンはあるが中身が空という
    // 状態 (開いた直後・遷移直後) で落ちれば同じこと。
    scene::Scene& scene = AttachScene();
    scene.CreateGameObject("Player");

    EditorBusDispatcher dispatcher(Context());
    for (const std::string& type : AllCommandTypes()) {
        const std::string response = dispatcher.Handle(MakeRequest(type, "query"));
        ExpectWellFormedResponse(response, type + " (with scene)");
        if (::testing::Test::HasFatalFailure()) return;
    }
}

TEST_F(EditorBusDispatcherTest, SurvivesEveryCommandOnAnEmptyScene)
{
    // GameObject が 1 つも無いシーン。«先頭を取る» 類の実装がここで落ちる。
    AttachScene();

    EditorBusDispatcher dispatcher(Context());
    for (const std::string& type : AllCommandTypes()) {
        const std::string response = dispatcher.Handle(MakeRequest(type, "query"));
        ExpectWellFormedResponse(response, type + " (empty scene)");
        if (::testing::Test::HasFatalFailure()) return;
    }
}

TEST_F(EditorBusDispatcherTest, SurvivesWhenTheSceneIsDetachedMidSession)
{
    // Play 中の LoadScene が起こす順序。繋がった状態で一度使ってから切れる。
    scene::Scene& scene = AttachScene();
    scene.CreateGameObject("Player");

    EditorBusDispatcher dispatcher(Context());
    ASSERT_FALSE(dispatcher.Handle(MakeRequest("scene.tree")).empty());

    DetachScene();

    for (const std::string& type : AllCommandTypes()) {
        const std::string response = dispatcher.Handle(MakeRequest(type, "query"));
        ExpectWellFormedResponse(response, type + " (detached)");
        if (::testing::Test::HasFatalFailure()) return;
    }
}

TEST_F(EditorBusDispatcherTest, ReportsAnErrorForAnUnknownCommand)
{
    EditorBusDispatcher dispatcher(Context());

    const std::string response = dispatcher.Handle(MakeRequest("no.such.command"));

    ExpectWellFormedResponse(response, "unknown");
    const auto parsed = ParseJson(response, nullptr);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->Find("ok")->AsBool());
}

TEST_F(EditorBusDispatcherTest, AnswersEvenWhenTheLineIsNotJson)
{
    // 相関 id を取り出せなくても «壊れている» ことは返す。黙ると送信側は
    // timeout まで固まり、しかも «届いていない» のか «壊れていた» のか分からない。
    EditorBusDispatcher dispatcher(Context());

    const std::string response = dispatcher.Handle("this is not json {{{");

    ASSERT_FALSE(response.empty()) << "壊れた行にも応答を返すこと";
    const auto parsed = ParseJson(response, nullptr);
    ASSERT_TRUE(parsed.has_value()) << response;
    EXPECT_FALSE(parsed->Find("ok")->AsBool());
    ASSERT_NE(parsed->Find("error"), nullptr);
    EXPECT_EQ(parsed->Find("error")->Find("code")->AsString(), "BAD_JSON");
}

TEST_F(EditorBusDispatcherTest, StaysSilentOnABlankLine)
{
    // 空行は NDJSON の区切りとして正常。要求ではないのでエラーを返さない。
    EditorBusDispatcher dispatcher(Context());

    EXPECT_TRUE(dispatcher.Handle("").empty());
    EXPECT_TRUE(dispatcher.Handle("   ").empty());
    EXPECT_TRUE(dispatcher.Handle("\r\n").empty());
}

TEST_F(EditorBusDispatcherTest, AnswersWithAnErrorWhenTheIdIsReadable)
{
    // エンベロープが壊れていても、id が読めればそれを載せて «誰への失敗か» を言う。
    // 読めない場合 (上のテスト) は id 空で返す。相関できるかの差だけで、
    // どちらも «応答は返す» 側に倒す。
    EditorBusDispatcher dispatcher(Context());

    JsonValue root = JsonValue::MakeObject();
    root.Set("id", JsonValue("test-1"));
    root.Set("kind", JsonValue("query"));   // protocol と payload が無い

    const std::string response = dispatcher.Handle(SerializeJson(root));

    ASSERT_FALSE(response.empty()) << "id が読めるなら応答を返すこと";
    const auto parsed = ParseJson(response, nullptr);
    ASSERT_TRUE(parsed.has_value()) << response;
    EXPECT_FALSE(parsed->Find("ok")->AsBool());
    EXPECT_EQ(parsed->Find("id")->AsString(), "test-1");
}

TEST_F(EditorBusDispatcherTest, KeepsWorkingAfterAFailedRequest)
{
    // 1 回の失敗で内部状態が壊れると、以降 «何を送っても落ちる» になる。
    EditorBusDispatcher dispatcher(Context());

    dispatcher.Handle("garbage");
    dispatcher.Handle(MakeRequest("no.such.command"));

    ExpectWellFormedResponse(dispatcher.Handle(MakeRequest("editor.catalog")), "after failures");
}

TEST_F(EditorBusDispatcherTest, HandlesTheSameCommandRepeatedly)
{
    // MCP は同じ問い合わせを繰り返す。呼ぶたびに状態が積み上がらないこと。
    EditorBusDispatcher dispatcher(Context());

    for (int i = 0; i < 5; ++i)
        ExpectWellFormedResponse(dispatcher.Handle(MakeRequest("editor.catalog")),
                                 "repeat " + std::to_string(i));
}

TEST_F(EditorBusDispatcherTest, SurvivesACommandCarryingUnexpectedFields)
{
    // 送信側が新しい項目を足しても、古いエディターが落ちてはいけない。
    EditorBusDispatcher dispatcher(Context());

    JsonValue payload = JsonValue::MakeObject();
    payload.Set("t", JsonValue("editor.catalog"));
    payload.Set("id", JsonValue(12345));            // 文字列を期待している所へ数値
    payload.Set("futureField", JsonValue(true));
    payload.Set("nested", JsonValue::MakeArray());

    JsonValue root = JsonValue::MakeObject();
    root.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
    root.Set("id", JsonValue("test-1"));
    root.Set("kind", JsonValue("query"));
    root.Set("payload", std::move(payload));

    ExpectWellFormedResponse(dispatcher.Handle(SerializeJson(root)), "extra fields");
}

} // namespace fbzz::tests
