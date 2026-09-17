/// @file    EditorBusDispatcherTests.cpp
/// @brief   AI 連携ディスパッチャが «壊れた文脈» で落ちないことを網羅で確かめる。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note Play 中はアクティブシーンが差し替わり、SceneManager::CurrentScene() は m_externalScene/m_active のどちらも無い瞬間に nullptr を返し EditorContext::activeScene へそのまま入る (Play 中の LoadScene)。
/// @note DrainRequests は毎フレーム走るため、その瞬間に届いた AI コマンドは nullptr のシーンを掴みうる。ここで落ちずにエラーを返すことを 1 コマンドずつ固定する。
/// @note ハンドラは 137 個あり、シーン操作の有無が混在する。1 つでもガードを忘れると Play 中の AI 操作で落ちるが、どれが穴かは事前に分からないため 1 つずつ投げて確かめる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::ai::EditorBusDispatcher;
using editor::ai::JsonValue;
using editor::ai::ParseJson;
using editor::ai::SerializeJson;

/// 実運用と同じ NDJSON エンベロープを 1 行組み立てる。
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

/// 応答が «プロトコルとして読める» ことまで見る。落ちないだけでは足りない
/// (空文字や壊れた JSON を返すと、MCP 側が固まる)。
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

/// @brief Dispatcher が受け付ける payload.t の一覧。
/// @note 実装の文字列リテラルから拾った 137 種。増えたらここへ足す。
const std::vector<std::string>& AllCommandTypes()
{
    static const std::vector<std::string> types = {
        /// @note @@COMMANDS_BEGIN
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
        "fluid.addOperator", "fluid.bake", "fluid.cancel", "fluid.create", "fluid.createEffect",
        "fluid.get", "fluid.jobStatus", "fluid.moveOperator", "fluid.preview",
        "fluid.removeOperator", "fluid.schema", "fluid.set",
        "input.inject", "material.asset.setShader", "material.assign", "material.inspect",
        "material.override", "navmesh.bake", "navmesh.path", "navmesh.sample", "navmesh.state",
        "node.components", "node.create", "node.delete", "node.duplicate", "node.rename",
        "node.reparent", "node.setActive", "node.setLayer", "node.setTag", "physics.events",
        "physics.overlapSphere", "physics.raycast", "play.control", "prefab.apply",
        "prefab.create", "prefab.instantiate", "prefab.revert", "preset.catalog",
        "preset.create", "profiler.snapshot", "scene.find", "scene.list", "scene.open",
        "scene.save", "scene.selection", "scene.snapshot", "scene.tree", "scene.validate",
        "script.reload", "selection.set", "shader.diagnostics", "shader.inspect",
        "sprite.list", "sprite.rename", "sprite.slice", "sprite.thumbnail", "terrain.hole",
        "terrain.inspect", "terrain.paint", "terrain.ramp", "terrain.sample", "terrain.sculpt",
        "terrain.setLayerMaterial",
        "transform.set", "ui.inspect", "vfx.generateMotionVectors", "viewport.camera",
        "viewport.capture", "viewport.semantic",
        "editor.bus.list", "input.record", "playtest.cancel", "playtest.list", "playtest.run",
        "playtest.status", "visual.compare",
        /// @note @@COMMANDS_END
    };
    return types;
}

} // namespace

/// 既定は «何も繋がっていない» 文脈。Play 中にシーンが差し替わる谷間そのもの。
class EditorBusDispatcherTest : public testkit::EditorFixture {};

TEST_F(EditorBusDispatcherTest, SurvivesEveryCommandWithoutAScene)
{
    /// @note 本題。1 つでもガードが抜けていれば、ここでプロセスごと落ちる。
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
    /// @note query だけでなく command 側の分岐も通す。dryRun なので実変更は起きない。
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
    /// @note シーンが «有る» 側も通す。無い側だけ守っても、シーンはあるが中身が空という
    ///       状態 (開いた直後・遷移直後) で落ちれば同じこと。
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
    /// @note GameObject が 1 つも無いシーン。«先頭を取る» 類の実装がここで落ちる。
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
    /// @note Play 中の LoadScene が起こす順序。繋がった状態で一度使ってから切れる。
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
    /// @note 相関 id を取り出せなくても «壊れている» ことは返す。黙ると送信側は
    ///       timeout まで固まり、しかも «届いていない» のか «壊れていた» のか分からない。
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
    /// @note 空行は NDJSON の区切りとして正常。要求ではないのでエラーを返さない。
    EditorBusDispatcher dispatcher(Context());

    EXPECT_TRUE(dispatcher.Handle("").empty());
    EXPECT_TRUE(dispatcher.Handle("   ").empty());
    EXPECT_TRUE(dispatcher.Handle("\r\n").empty());
}

TEST_F(EditorBusDispatcherTest, AnswersWithAnErrorWhenTheIdIsReadable)
{
    /// @note エンベロープが壊れていても、id が読めればそれを載せて «誰への失敗か» を言う。
    ///       読めない場合 (上のテスト) は id 空で返す。相関できるかの差だけで、
    ///       どちらも «応答は返す» 側に倒す。
    EditorBusDispatcher dispatcher(Context());

    JsonValue root = JsonValue::MakeObject();
    root.Set("id", JsonValue("test-1"));
    /// @note protocol と payload が無い
    root.Set("kind", JsonValue("query"));

    const std::string response = dispatcher.Handle(SerializeJson(root));

    ASSERT_FALSE(response.empty()) << "id が読めるなら応答を返すこと";
    const auto parsed = ParseJson(response, nullptr);
    ASSERT_TRUE(parsed.has_value()) << response;
    EXPECT_FALSE(parsed->Find("ok")->AsBool());
    EXPECT_EQ(parsed->Find("id")->AsString(), "test-1");
}

TEST_F(EditorBusDispatcherTest, KeepsWorkingAfterAFailedRequest)
{
    /// @note 1 回の失敗で内部状態が壊れると、以降 «何を送っても落ちる» になる。
    EditorBusDispatcher dispatcher(Context());

    dispatcher.Handle("garbage");
    dispatcher.Handle(MakeRequest("no.such.command"));

    ExpectWellFormedResponse(dispatcher.Handle(MakeRequest("editor.catalog")), "after failures");
}

TEST_F(EditorBusDispatcherTest, HandlesTheSameCommandRepeatedly)
{
    /// @note MCP は同じ問い合わせを繰り返す。呼ぶたびに状態が積み上がらないこと。
    EditorBusDispatcher dispatcher(Context());

    for (int i = 0; i < 5; ++i)
        ExpectWellFormedResponse(dispatcher.Handle(MakeRequest("editor.catalog")),
                                 "repeat " + std::to_string(i));
}

TEST_F(EditorBusDispatcherTest, SurvivesACommandCarryingUnexpectedFields)
{
    /// @note 送信側が新しい項目を足しても、古いエディターが落ちてはいけない。
    EditorBusDispatcher dispatcher(Context());

    JsonValue payload = JsonValue::MakeObject();
    payload.Set("t", JsonValue("editor.catalog"));
    /// @note 文字列を期待している所へ数値
    payload.Set("id", JsonValue(12345));
    payload.Set("futureField", JsonValue(true));
    payload.Set("nested", JsonValue::MakeArray());

    JsonValue root = JsonValue::MakeObject();
    root.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
    root.Set("id", JsonValue("test-1"));
    root.Set("kind", JsonValue("query"));
    root.Set("payload", std::move(payload));

    ExpectWellFormedResponse(dispatcher.Handle(SerializeJson(root)), "extra fields");
}

/// @name 流体 (.fluid)
/// AI はファイルだけで流体を作って焼く。«作る → 読む → 部分更新 → Undo» の往復と、
/// 焼きの窓口が無い文脈 (テスト・単体ツール) で黙って成功しないことを縛る。
namespace {

std::string MakePayloadRequest(const JsonValue& payload, const std::string& kind)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
    root.Set("id", JsonValue("test-1"));
    root.Set("kind", JsonValue(kind));
    root.Set("dryRun", JsonValue(false));
    root.Set("payload", payload);
    return SerializeJson(root);
}

JsonValue FluidPayload(const std::string& type, const std::string& path)
{
    JsonValue payload = JsonValue::MakeObject();
    payload.Set("t", JsonValue(type));
    if (!path.empty()) payload.Set("path", JsonValue(path));
    return payload;
}

bool IsOk(const JsonValue& response)
{
    const JsonValue* ok = response.Find("ok");
    return ok != nullptr && ok->AsBool();
}

std::string ErrorCode(const JsonValue& response)
{
    const JsonValue* error = response.Find("error");
    const JsonValue* code = error != nullptr ? error->Find("code") : nullptr;
    return code != nullptr ? code->AsString() : std::string{};
}

std::string ErrorMessage(const JsonValue& response)
{
    const JsonValue* error = response.Find("error");
    const JsonValue* message = error != nullptr ? error->Find("message") : nullptr;
    return message != nullptr ? message->AsString() : std::string{};
}

constexpr const char* kSmokePath = "Assets/VFX/Smoke.fluid";

} // namespace

/// projectRoot と UndoStack だけを繋ぐ。FluidBakeService は繋がない (焼きは GPU とフレームが要る)。
class EditorBusFluidTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().undoStack = &m_undo;
        m_dispatcher = std::make_unique<EditorBusDispatcher>(Context());
    }

    void TearDown() override
    {
        m_dispatcher.reset();
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }

    JsonValue Send(const JsonValue& payload, const std::string& kind = "command")
    {
        const std::string line = m_dispatcher->Handle(MakePayloadRequest(payload, kind));
        std::optional<JsonValue> parsed = ParseJson(line, nullptr);
        return parsed.has_value() ? *parsed : JsonValue::MakeObject();
    }

    fluid::FluidRecipe LoadRecipe(const std::string& relative)
    {
        fluid::FluidRecipe recipe;
        EXPECT_TRUE(asset::LoadFluidRecipe(File(relative).generic_string(), recipe)) << relative;
        return recipe;
    }

    editor::UndoStack                    m_undo;
    std::unique_ptr<EditorBusDispatcher> m_dispatcher;
};

TEST_F(EditorBusFluidTest, CreatesARecipeAndReadsItBack)
{
    const JsonValue created = Send(FluidPayload("fluid.create", kSmokePath));
    ASSERT_TRUE(IsOk(created)) << SerializeJson(created);
    ASSERT_NE(created.Find("result"), nullptr);
    EXPECT_EQ(created.Find("result")->Find("path")->AsString(), kSmokePath);
    EXPECT_TRUE(std::filesystem::is_regular_file(File(kSmokePath)));

    const JsonValue got = Send(FluidPayload("fluid.get", kSmokePath), "query");
    ASSERT_TRUE(IsOk(got)) << SerializeJson(got);
    const JsonValue* recipe = got.Find("result")->Find("recipe");
    ASSERT_NE(recipe, nullptr);
    const JsonValue* gas = recipe->Find("gas");
    ASSERT_NE(gas, nullptr) << SerializeJson(*recipe);
    ASSERT_NE(gas->Find("buoyancy"), nullptr);
    EXPECT_NEAR(gas->Find("buoyancy")->AsNumber(),
                asset::MakeFluidPreset(asset::FluidPreset::Smoke).gas.buoyancy, 1e-4);
    const bool volume = asset::MakeFluidPreset(asset::FluidPreset::Smoke).bake.mode == fluid::FluidBakeMode::Volume3D;
    EXPECT_EQ(got.Find("result")->Find("bakeMode")->AsString(), volume ? "3d" : "2d");
}

TEST_F(EditorBusFluidTest, SetChangesANestedKeyAndUndoRestoresIt)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    const float original = LoadRecipe(kSmokePath).gas.buoyancy;

    JsonValue gas = JsonValue::MakeObject();
    gas.Set("buoyancy", JsonValue(3.25));
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("gas", std::move(gas));
    /// @note ドット区切りでも同じ場所を指す。
    fields.Set("render.opacity", JsonValue(7.5));
    JsonValue payload = FluidPayload("fluid.set", kSmokePath);
    payload.Set("fields", std::move(fields));

    const JsonValue response = Send(payload);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const JsonValue* changed = response.Find("result")->Find("changed");
    ASSERT_NE(changed, nullptr);
    ASSERT_EQ(changed->AsArray().size(), 2u);
    EXPECT_EQ(changed->AsArray()[0].AsString(), "gas.buoyancy");
    EXPECT_EQ(changed->AsArray()[1].AsString(), "render.opacity");

    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, 3.25f);
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).render.opacity, 7.5f);

    m_undo.Undo();
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, original);

    m_undo.Redo();
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, 3.25f);
}

TEST_F(EditorBusFluidTest, SetReportsUnknownKeysAndWritesNothing)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    const float original = LoadRecipe(kSmokePath).gas.buoyancy;

    /// @note 正しいキーが混ざっていても書かない。一部だけ効くと AI は効かなかった方を見落とす。
    JsonValue gas = JsonValue::MakeObject();
    gas.Set("buoyancy", JsonValue(9.0));
    gas.Set("noSuchKey", JsonValue(1));
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("gas", std::move(gas));
    JsonValue payload = FluidPayload("fluid.set", kSmokePath);
    payload.Set("fields", std::move(fields));

    const JsonValue response = Send(payload);
    EXPECT_FALSE(IsOk(response));
    EXPECT_EQ(ErrorCode(response), "UNKNOWN_FIELD");
    EXPECT_NE(ErrorMessage(response).find("gas.noSuchKey"), std::string::npos) << ErrorMessage(response);
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, original);
}

TEST_F(EditorBusFluidTest, CreateRefusesToOverwriteUnlessAsked)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));

    const JsonValue again = Send(FluidPayload("fluid.create", kSmokePath));
    EXPECT_FALSE(IsOk(again));
    EXPECT_EQ(ErrorCode(again), "FLUID_EXISTS");

    JsonValue overwrite = FluidPayload("fluid.create", kSmokePath);
    overwrite.Set("preset", JsonValue("Fire"));
    overwrite.Set("overwrite", JsonValue(true));
    const JsonValue replaced = Send(overwrite);
    ASSERT_TRUE(IsOk(replaced)) << SerializeJson(replaced);
    EXPECT_TRUE(replaced.Find("result")->Find("overwrote")->AsBool());

    /// @note 上書きの Undo は消すのではなく前の中身へ戻す。
    const float fireBuoyancy = asset::MakeFluidPreset(asset::FluidPreset::Fire).gas.buoyancy;
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, fireBuoyancy);
    m_undo.Undo();
    EXPECT_TRUE(std::filesystem::is_regular_file(File(kSmokePath)));
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy,
                    asset::MakeFluidPreset(asset::FluidPreset::Smoke).gas.buoyancy);
}

TEST_F(EditorBusFluidTest, CreateUndoRemovesTheFileItCreated)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    ASSERT_TRUE(std::filesystem::is_regular_file(File(kSmokePath)));

    m_undo.Undo();
    EXPECT_FALSE(std::filesystem::exists(File(kSmokePath)));
}

TEST_F(EditorBusFluidTest, BakeAndPreviewNeedTheService)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));

    EXPECT_EQ(ErrorCode(Send(FluidPayload("fluid.bake", kSmokePath))), "SERVICE_UNAVAILABLE");
    EXPECT_EQ(ErrorCode(Send(FluidPayload("fluid.preview", kSmokePath))), "SERVICE_UNAVAILABLE");

    /// @note 焼けないと分かっているなら .fluid も書かない (成功に見えて何も出ないのを防ぐ)。
    JsonValue effect = FluidPayload("fluid.createEffect", "");
    effect.Set("name", JsonValue("Puff"));
    EXPECT_EQ(ErrorCode(Send(effect)), "SERVICE_UNAVAILABLE");
    EXPECT_FALSE(std::filesystem::exists(File("Assets/VFX/Fluid/Puff.fluid")));

    /// @note 焼かないなら窓口が無くても作れる。
    effect.Set("bake", JsonValue(false));
    const JsonValue created = Send(effect);
    ASSERT_TRUE(IsOk(created)) << SerializeJson(created);
    EXPECT_EQ(created.Find("result")->Find("fluidPath")->AsString(), "Assets/VFX/Fluid/Puff.fluid");
    EXPECT_EQ(created.Find("result")->Find("job"), nullptr);
    EXPECT_TRUE(std::filesystem::is_regular_file(File("Assets/VFX/Fluid/Puff.fluid")));
}

TEST_F(EditorBusFluidTest, JobStatusReportsAnUnknownJob)
{
    JsonValue payload = FluidPayload("fluid.jobStatus", "");
    payload.Set("job", JsonValue(42));

    const JsonValue response = Send(payload, "query");
    EXPECT_FALSE(IsOk(response));
    EXPECT_EQ(ErrorCode(response), "FLUID_JOB_NOT_FOUND");
}

TEST_F(EditorBusFluidTest, TransactionRefusesBakeButAcceptsEdits)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));

    /// @note 焼きは Undo できないので、まとめて戻す transaction には入れさせない。
    JsonValue bakeCmds = JsonValue::MakeArray();
    bakeCmds.Push(FluidPayload("fluid.bake", kSmokePath));
    JsonValue bakeTransaction = JsonValue::MakeObject();
    bakeTransaction.Set("t", JsonValue("editor.transaction"));
    bakeTransaction.Set("label", JsonValue("bake"));
    bakeTransaction.Set("cmds", std::move(bakeCmds));
    EXPECT_EQ(ErrorCode(Send(bakeTransaction)), "UNSUPPORTED");

    JsonValue fields = JsonValue::MakeObject();
    fields.Set("gas.buoyancy", JsonValue(2.5));
    JsonValue set = FluidPayload("fluid.set", kSmokePath);
    set.Set("fields", std::move(fields));
    JsonValue editCmds = JsonValue::MakeArray();
    editCmds.Push(std::move(set));
    JsonValue editTransaction = JsonValue::MakeObject();
    editTransaction.Set("t", JsonValue("editor.transaction"));
    editTransaction.Set("label", JsonValue("edit"));
    editTransaction.Set("cmds", std::move(editCmds));
    const JsonValue response = Send(editTransaction);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    EXPECT_FLOAT_EQ(LoadRecipe(kSmokePath).gas.buoyancy, 2.5f);

    /// @note 部品の追加も Undo できるファイル書き込みなので transaction に入れられる。
    JsonValue add = FluidPayload("fluid.addOperator", kSmokePath);
    add.Set("list", JsonValue("force"));
    JsonValue addCmds = JsonValue::MakeArray();
    addCmds.Push(std::move(add));
    JsonValue addTransaction = JsonValue::MakeObject();
    addTransaction.Set("t", JsonValue("editor.transaction"));
    addTransaction.Set("label", JsonValue("add"));
    addTransaction.Set("cmds", std::move(addCmds));
    const std::size_t forcesBefore = LoadRecipe(kSmokePath).forces.size();
    const JsonValue added = Send(addTransaction);
    ASSERT_TRUE(IsOk(added)) << SerializeJson(added);
    EXPECT_EQ(LoadRecipe(kSmokePath).forces.size(), forcesBefore + 1);
}

/// @name 部品 (発生源・力)
namespace {

JsonValue OperatorPayload(const std::string& type, const std::string& list)
{
    JsonValue payload = FluidPayload(type, kSmokePath);
    payload.Set("list", JsonValue(list));
    return payload;
}

bool ArrayHasString(const JsonValue* array, const std::string& wanted)
{
    if (array == nullptr || !array->IsArray()) return false;
    for (const JsonValue& item : array->AsArray())
        if (item.IsString() && item.AsString() == wanted) return true;
    return false;
}

} // namespace

TEST_F(EditorBusFluidTest, AddOperatorInsertsATypedPartAndUndoRemovesIt)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    const std::size_t before = LoadRecipe(kSmokePath).sources.size();

    /// @note ラベルは大文字小文字を問わない。fields は新しい部品 1 つぶんの部分指定。
    JsonValue payload = OperatorPayload("fluid.addOperator", "source");
    payload.Set("type", JsonValue("Cone"));
    payload.Set("index", JsonValue(0));
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("density", JsonValue(7.0));
    fields.Set("name", JsonValue("Jet"));
    payload.Set("fields", std::move(fields));

    const JsonValue response = Send(payload);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const JsonValue* result = response.Find("result");
    EXPECT_EQ(result->Find("list")->AsString(), "source");
    EXPECT_EQ(result->Find("index")->AsInt(), 0);
    EXPECT_EQ(result->Find("count")->AsInt(), static_cast<int>(before + 1));
    EXPECT_TRUE(ArrayHasString(result->Find("changed"), "source.0.density"));

    fluid::FluidRecipe recipe = LoadRecipe(kSmokePath);
    ASSERT_EQ(recipe.sources.size(), before + 1);
    EXPECT_EQ(recipe.sources[0].shape, fluid::FluidSourceShape::Cone);
    EXPECT_FLOAT_EQ(recipe.sources[0].density, 7.0f);
    EXPECT_EQ(recipe.sources[0].name, "Jet");

    m_undo.Undo();
    EXPECT_EQ(LoadRecipe(kSmokePath).sources.size(), before);
    m_undo.Redo();
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[0].shape, fluid::FluidSourceShape::Cone);

    /// @note 力の種類もラベルで選べる (TOML と同じ綴り)。
    JsonValue force = OperatorPayload("fluid.addOperator", "force");
    force.Set("type", JsonValue("VORTEX"));
    ASSERT_TRUE(IsOk(Send(force)));
    EXPECT_EQ(LoadRecipe(kSmokePath).forces.back().type, fluid::FluidForceType::Vortex);
}

TEST_F(EditorBusFluidTest, RemoveAndMoveOperatorsAreUndoable)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    for (const char* shape : { "box", "ring" }) {
        JsonValue add = OperatorPayload("fluid.addOperator", "source");
        add.Set("type", JsonValue(shape));
        ASSERT_TRUE(IsOk(Send(add)));
    }
    const fluid::FluidRecipe original = LoadRecipe(kSmokePath);
    const std::size_t count = original.sources.size();
    ASSERT_GE(count, 2u);

    /// @note 末尾 (ring) を先頭へ。
    JsonValue move = OperatorPayload("fluid.moveOperator", "source");
    move.Set("from", JsonValue(static_cast<int>(count - 1)));
    move.Set("to", JsonValue(0));
    const JsonValue moved = Send(move);
    ASSERT_TRUE(IsOk(moved)) << SerializeJson(moved);
    EXPECT_EQ(moved.Find("result")->Find("from")->AsInt(), static_cast<int>(count - 1));
    EXPECT_EQ(moved.Find("result")->Find("to")->AsInt(), 0);
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[0].shape, fluid::FluidSourceShape::Ring);
    m_undo.Undo();
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[count - 1].shape, fluid::FluidSourceShape::Ring);
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[0].shape, original.sources[0].shape);

    JsonValue remove = OperatorPayload("fluid.removeOperator", "source");
    remove.Set("index", JsonValue(static_cast<int>(count - 2)));
    const JsonValue removed = Send(remove);
    ASSERT_TRUE(IsOk(removed)) << SerializeJson(removed);
    EXPECT_EQ(removed.Find("result")->Find("removed")->AsInt(), static_cast<int>(count - 2));
    EXPECT_EQ(removed.Find("result")->Find("count")->AsInt(), static_cast<int>(count - 1));
    fluid::FluidRecipe afterRemove = LoadRecipe(kSmokePath);
    ASSERT_EQ(afterRemove.sources.size(), count - 1);
    EXPECT_EQ(afterRemove.sources.back().shape, fluid::FluidSourceShape::Ring);
    m_undo.Undo();
    fluid::FluidRecipe restored = LoadRecipe(kSmokePath);
    ASSERT_EQ(restored.sources.size(), count);
    EXPECT_EQ(restored.sources[count - 2].shape, fluid::FluidSourceShape::Box);

    /// @note 範囲外は何も書かずに BAD_ARG。
    remove.Set("index", JsonValue(static_cast<int>(count)));
    EXPECT_EQ(ErrorCode(Send(remove)), "BAD_ARG");
    move.Set("from", JsonValue(static_cast<int>(count)));
    EXPECT_EQ(ErrorCode(Send(move)), "BAD_ARG");
    EXPECT_EQ(LoadRecipe(kSmokePath).sources.size(), count);
}

TEST_F(EditorBusFluidTest, AddOperatorRejectsUnknownListsTypesAndAFullList)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));

    EXPECT_EQ(ErrorCode(Send(OperatorPayload("fluid.addOperator", "gas_source"))), "BAD_ARG");
    JsonValue badType = OperatorPayload("fluid.addOperator", "force");
    badType.Set("type", JsonValue("tornado"));
    EXPECT_EQ(ErrorCode(Send(badType)), "BAD_ARG");

    /// @note 配列ごと渡すと上限で切り詰め、何を落としたかを返す。
    JsonValue many = JsonValue::MakeArray();
    for (int i = 0; i < fluid::kMaxFluidForces + 2; ++i) many.Push(JsonValue::MakeObject());
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("force", std::move(many));
    JsonValue set = FluidPayload("fluid.set", kSmokePath);
    set.Set("fields", std::move(fields));
    const JsonValue filled = Send(set);
    ASSERT_TRUE(IsOk(filled)) << SerializeJson(filled);
    EXPECT_NE(filled.Find("result")->Find("clamped"), nullptr) << SerializeJson(filled);
    EXPECT_EQ(LoadRecipe(kSmokePath).forces.size(), static_cast<std::size_t>(fluid::kMaxFluidForces));

    const JsonValue full = Send(OperatorPayload("fluid.addOperator", "force"));
    EXPECT_FALSE(IsOk(full));
    EXPECT_EQ(ErrorCode(full), "OPERATOR_LIMIT");
    EXPECT_EQ(LoadRecipe(kSmokePath).forces.size(), static_cast<std::size_t>(fluid::kMaxFluidForces));
}

TEST_F(EditorBusFluidTest, SetAcceptsForceTypeLabelsAndMotionKeyArrays)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    ASSERT_TRUE(IsOk(Send(OperatorPayload("fluid.addOperator", "force"))));
    ASSERT_TRUE(IsOk(Send(OperatorPayload("fluid.addOperator", "source"))));
    const std::size_t forceIndex = LoadRecipe(kSmokePath).forces.size() - 1;

    JsonValue key0 = JsonValue::MakeObject();
    key0.Set("time", JsonValue(0.0));
    JsonValue offset0 = JsonValue::MakeArray();
    for (double v : { 0.0, 0.0, 0.0 }) offset0.Push(JsonValue(v));
    key0.Set("offset", std::move(offset0));
    JsonValue key1 = JsonValue::MakeObject();
    key1.Set("time", JsonValue(1.0));
    JsonValue offset1 = JsonValue::MakeArray();
    for (double v : { 0.5, 0.25, 0.0 }) offset1.Push(JsonValue(v));
    key1.Set("offset", std::move(offset1));
    JsonValue keys = JsonValue::MakeArray();
    keys.Push(std::move(key0));
    keys.Push(std::move(key1));

    JsonValue fields = JsonValue::MakeObject();
    fields.Set("force." + std::to_string(forceIndex) + ".type", JsonValue("vortex"));
    fields.Set("source.0.motion.key", std::move(keys));
    JsonValue payload = FluidPayload("fluid.set", kSmokePath);
    payload.Set("fields", std::move(fields));

    const JsonValue response = Send(payload);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const fluid::FluidRecipe recipe = LoadRecipe(kSmokePath);
    EXPECT_EQ(recipe.forces[forceIndex].type, fluid::FluidForceType::Vortex);
    ASSERT_EQ(recipe.sources[0].motion.keys.size(), 2u);
    EXPECT_FLOAT_EQ(recipe.sources[0].motion.keys[1].time, 1.0f);
    EXPECT_FLOAT_EQ(recipe.sources[0].motion.keys[1].offset.x, 0.5f);
    EXPECT_FLOAT_EQ(recipe.sources[0].motion.keys[1].offset.y, 0.25f);
}

TEST_F(EditorBusFluidTest, SchemaListsTheFieldsOfEachOperatorType)
{
    const JsonValue response = Send(FluidPayload("fluid.schema", ""), "query");
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const JsonValue* operators = response.Find("result")->Find("operators");
    ASSERT_NE(operators, nullptr);

    const JsonValue* source = operators->Find("source");
    ASSERT_NE(source, nullptr);
    const JsonValue* shapes = source->Find("types");
    ASSERT_NE(shapes, nullptr);
    EXPECT_EQ(shapes->AsArray().size(), 7u);
    EXPECT_TRUE(ArrayHasString(shapes, "cone"));
    EXPECT_TRUE(ArrayHasString(shapes, "texture"));
    EXPECT_TRUE(ArrayHasString(shapes, "capsule"));
    EXPECT_TRUE(ArrayHasString(shapes, "cylinder"));
    /// @note カプセルは芯の軸を direction で持つ形。種類ごとの一覧にその項目が見えていること。
    const JsonValue* capsule = source->Find("fields")->Find("capsule");
    ASSERT_NE(capsule, nullptr);
    EXPECT_TRUE(ArrayHasString(capsule, "direction")) << SerializeJson(*capsule);
    const JsonValue* textureShape = source->Find("fields")->Find("texture");
    ASSERT_NE(textureShape, nullptr);
    EXPECT_TRUE(ArrayHasString(textureShape, "texture")) << SerializeJson(*textureShape);
    /// @note cone は向きを持つ形。種類ごとの一覧にその項目が見えていること。
    const JsonValue* cone = source->Find("fields")->Find("cone");
    ASSERT_NE(cone, nullptr);
    EXPECT_FALSE(cone->AsArray().empty());
    EXPECT_TRUE(ArrayHasString(cone, "direction")) << SerializeJson(*cone);

    const JsonValue* force = operators->Find("force");
    ASSERT_NE(force, nullptr);
    const JsonValue* forceTypes = force->Find("types");
    ASSERT_NE(forceTypes, nullptr);
    EXPECT_EQ(forceTypes->AsArray().size(), 6u);
    const JsonValue* vortex = force->Find("fields")->Find("vortex");
    ASSERT_NE(vortex, nullptr);
    EXPECT_TRUE(ArrayHasString(vortex, "strength")) << SerializeJson(*vortex);
    EXPECT_TRUE(ArrayHasString(vortex, "direction")) << SerializeJson(*vortex);

    const JsonValue* collider = operators->Find("collider");
    ASSERT_NE(collider, nullptr);
    EXPECT_EQ(collider->Find("typeField")->AsString(), "shape");
    EXPECT_EQ(collider->Find("limit")->AsInt(), fluid::kMaxFluidColliders);
    const JsonValue* colliderShapes = collider->Find("types");
    ASSERT_NE(colliderShapes, nullptr);
    EXPECT_EQ(colliderShapes->AsArray().size(), 5u);
    for (const char* shape : { "sphere", "box", "plane", "capsule", "cylinder" })
        EXPECT_TRUE(ArrayHasString(colliderShapes, shape)) << shape;
    /// @note plane は法線 (direction) で向きが決まる形。
    const JsonValue* plane = collider->Find("fields")->Find("plane");
    ASSERT_NE(plane, nullptr);
    EXPECT_TRUE(ArrayHasString(plane, "direction")) << SerializeJson(*plane);
    /// @note カプセルは芯の軸 (direction) と大きさ (size) の両方を持つ。
    const JsonValue* capsuleCollider = collider->Find("fields")->Find("capsule");
    ASSERT_NE(capsuleCollider, nullptr);
    EXPECT_TRUE(ArrayHasString(capsuleCollider, "direction")) << SerializeJson(*capsuleCollider);
    EXPECT_TRUE(ArrayHasString(capsuleCollider, "size")) << SerializeJson(*capsuleCollider);

    const JsonValue* limits = response.Find("result")->Find("limits");
    ASSERT_NE(limits, nullptr);
    ASSERT_NE(limits->Find("collider"), nullptr);
    EXPECT_EQ(limits->Find("collider")->AsInt(), fluid::kMaxFluidColliders);
}

TEST_F(EditorBusFluidTest, CollidersCanBeAddedMovedAndRemovedWithUndo)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    const std::size_t before = LoadRecipe(kSmokePath).colliders.size();

    JsonValue plane = OperatorPayload("fluid.addOperator", "collider");
    plane.Set("type", JsonValue("Plane"));
    JsonValue fields = JsonValue::MakeObject();
    JsonValue direction = JsonValue::MakeArray();
    for (double v : { 0.0, 0.0, 1.0 }) direction.Push(JsonValue(v));
    fields.Set("direction", std::move(direction));
    fields.Set("friction", JsonValue(0.75));
    plane.Set("fields", std::move(fields));
    const JsonValue added = Send(plane);
    ASSERT_TRUE(IsOk(added)) << SerializeJson(added);
    EXPECT_EQ(added.Find("result")->Find("list")->AsString(), "collider");
    EXPECT_EQ(added.Find("result")->Find("index")->AsInt(), static_cast<int>(before));
    EXPECT_EQ(added.Find("result")->Find("count")->AsInt(), static_cast<int>(before + 1));

    fluid::FluidRecipe recipe = LoadRecipe(kSmokePath);
    ASSERT_EQ(recipe.colliders.size(), before + 1);
    EXPECT_EQ(recipe.colliders.back().shape, fluid::FluidColliderShape::Plane);
    EXPECT_FLOAT_EQ(recipe.colliders.back().direction.z, 1.0f);
    EXPECT_FLOAT_EQ(recipe.colliders.back().friction, 0.75f);
    m_undo.Undo();
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders.size(), before);
    m_undo.Redo();
    ASSERT_EQ(LoadRecipe(kSmokePath).colliders.size(), before + 1);

    /// @note 末尾に box を足して先頭へ回す。
    JsonValue box = OperatorPayload("fluid.addOperator", "collider");
    box.Set("type", JsonValue("box"));
    ASSERT_TRUE(IsOk(Send(box)));
    const std::size_t count = before + 2;
    JsonValue move = OperatorPayload("fluid.moveOperator", "collider");
    move.Set("from", JsonValue(static_cast<int>(count - 1)));
    move.Set("to", JsonValue(0));
    const JsonValue moved = Send(move);
    ASSERT_TRUE(IsOk(moved)) << SerializeJson(moved);
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders[0].shape, fluid::FluidColliderShape::Box);
    m_undo.Undo();
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders[count - 1].shape, fluid::FluidColliderShape::Box);

    JsonValue remove = OperatorPayload("fluid.removeOperator", "collider");
    remove.Set("index", JsonValue(static_cast<int>(count - 1)));
    const JsonValue removed = Send(remove);
    ASSERT_TRUE(IsOk(removed)) << SerializeJson(removed);
    ASSERT_EQ(LoadRecipe(kSmokePath).colliders.size(), count - 1);
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders.back().shape, fluid::FluidColliderShape::Plane);
    m_undo.Undo();
    ASSERT_EQ(LoadRecipe(kSmokePath).colliders.size(), count);
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders.back().shape, fluid::FluidColliderShape::Box);
}

TEST_F(EditorBusFluidTest, ColliderListStopsAtItsLimitAndTakesShapeLabels)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    const std::size_t limit = static_cast<std::size_t>(fluid::kMaxFluidColliders);
    for (std::size_t i = LoadRecipe(kSmokePath).colliders.size(); i < limit; ++i)
        ASSERT_TRUE(IsOk(Send(OperatorPayload("fluid.addOperator", "collider")))) << i;
    ASSERT_EQ(LoadRecipe(kSmokePath).colliders.size(), limit);

    const JsonValue full = Send(OperatorPayload("fluid.addOperator", "collider"));
    EXPECT_FALSE(IsOk(full));
    EXPECT_EQ(ErrorCode(full), "OPERATOR_LIMIT");
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders.size(), limit);

    /// @note 形のラベルは TOML と同じ綴りで、保存するときは添字になる。
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("collider.0.shape", JsonValue("plane"));
    JsonValue set = FluidPayload("fluid.set", kSmokePath);
    set.Set("fields", std::move(fields));
    const JsonValue response = Send(set);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    EXPECT_TRUE(ArrayHasString(response.Find("result")->Find("changed"), "collider.0.shape"));
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders[0].shape, fluid::FluidColliderShape::Plane);

    /// @note 配列ごと渡すと上限で切り詰めて clamped に載る。
    JsonValue many = JsonValue::MakeArray();
    for (std::size_t i = 0; i < limit + 3; ++i) many.Push(JsonValue::MakeObject());
    JsonValue arrayFields = JsonValue::MakeObject();
    arrayFields.Set("collider", std::move(many));
    JsonValue arraySet = FluidPayload("fluid.set", kSmokePath);
    arraySet.Set("fields", std::move(arrayFields));
    const JsonValue clamped = Send(arraySet);
    ASSERT_TRUE(IsOk(clamped)) << SerializeJson(clamped);
    EXPECT_NE(clamped.Find("result")->Find("clamped"), nullptr) << SerializeJson(clamped);
    EXPECT_EQ(LoadRecipe(kSmokePath).colliders.size(), limit);
}

TEST_F(EditorBusFluidTest, TextureSourceWithAMissingImageIsWrittenWithAWarning)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));

    /// @note 画像は後から置くことがあるので、無くても書いて warnings で知らせる。
    JsonValue payload = OperatorPayload("fluid.addOperator", "source");
    payload.Set("type", JsonValue("texture"));
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("texture", JsonValue("Assets/Textures/Missing.png"));
    payload.Set("fields", std::move(fields));
    const JsonValue response = Send(payload);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const JsonValue* warnings = response.Find("result")->Find("warnings");
    ASSERT_NE(warnings, nullptr) << SerializeJson(response);
    ASSERT_EQ(warnings->AsArray().size(), 1u) << SerializeJson(*warnings);
    EXPECT_NE(warnings->AsArray()[0].AsString().find("texture not found"), std::string::npos)
        << warnings->AsArray()[0].AsString();

    fluid::FluidRecipe recipe = LoadRecipe(kSmokePath);
    const std::size_t index = recipe.sources.size() - 1;
    EXPECT_EQ(recipe.sources[index].shape, fluid::FluidSourceShape::Texture);
    EXPECT_EQ(recipe.sources[index].texture, "Assets/Textures/Missing.png");

    /// @note 置いた画像を指せば warnings は出ない (中身は見ず、在るかだけを見る)。
    std::filesystem::create_directories(File("Assets/Textures"));
    { std::ofstream(File("Assets/Textures/Logo.png"), std::ios::binary) << "png"; }
    JsonValue found = JsonValue::MakeObject();
    found.Set("source." + std::to_string(index) + ".texture", JsonValue("Assets/Textures/Logo.png"));
    JsonValue set = FluidPayload("fluid.set", kSmokePath);
    set.Set("fields", std::move(found));
    const JsonValue updated = Send(set);
    ASSERT_TRUE(IsOk(updated)) << SerializeJson(updated);
    EXPECT_EQ(updated.Find("result")->Find("warnings"), nullptr) << SerializeJson(updated);
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[index].texture, "Assets/Textures/Logo.png");

    /// @note projectRoot の外は書かない。
    JsonValue outside = JsonValue::MakeObject();
    outside.Set("source." + std::to_string(index) + ".texture", JsonValue("../Outside.png"));
    JsonValue escape = FluidPayload("fluid.set", kSmokePath);
    escape.Set("fields", std::move(outside));
    EXPECT_EQ(ErrorCode(Send(escape)), "BAD_PATH");
    EXPECT_EQ(LoadRecipe(kSmokePath).sources[index].texture, "Assets/Textures/Logo.png");
}

TEST_F(EditorBusFluidTest, SetWritesPerSourceColorKeyAndTheAlbedoRamp)
{
    ASSERT_TRUE(IsOk(Send(FluidPayload("fluid.create", kSmokePath))));
    ASSERT_TRUE(IsOk(Send(OperatorPayload("fluid.addOperator", "source"))));

    /// @note Undo が直前の状態へ戻すことを見たいので開始状態はテスト自身で決める。プリセット既定に頼ると、絵を派手にした (煙を 2 色にした) だけでここが落ちる。
    {
        JsonValue off = JsonValue::MakeObject();
        off.Set("render.use_albedo_ramp", JsonValue(false));
        JsonValue reset = FluidPayload("fluid.set", kSmokePath);
        reset.Set("fields", std::move(off));
        ASSERT_TRUE(IsOk(Send(reset)));
        ASSERT_FALSE(LoadRecipe(kSmokePath).render.useAlbedoRamp);
    }

    /// @note 発生源ごとの色は «鍵 + ランプ» の 2 か所。どちらかが書けないと AI は色を塗り分けられない。
    JsonValue red = JsonValue::MakeArray();
    for (double v : { 1.0, 0.0, 0.0 }) red.Push(JsonValue(v));
    JsonValue fields = JsonValue::MakeObject();
    fields.Set("source.0.color_key", JsonValue(0.75));
    fields.Set("render.use_albedo_ramp", JsonValue(true));
    fields.Set("render.albedo_ramp.3.color", std::move(red));
    JsonValue payload = FluidPayload("fluid.set", kSmokePath);
    payload.Set("fields", std::move(fields));

    const JsonValue response = Send(payload);
    ASSERT_TRUE(IsOk(response)) << SerializeJson(response);
    const JsonValue* changed = response.Find("result")->Find("changed");
    EXPECT_TRUE(ArrayHasString(changed, "source.0.color_key")) << SerializeJson(response);
    EXPECT_TRUE(ArrayHasString(changed, "render.use_albedo_ramp")) << SerializeJson(response);

    const fluid::FluidRecipe recipe = LoadRecipe(kSmokePath);
    ASSERT_FALSE(recipe.sources.empty());
    EXPECT_FLOAT_EQ(recipe.sources[0].colorKey, 0.75f);
    EXPECT_TRUE(recipe.render.useAlbedoRamp);
    EXPECT_FLOAT_EQ(recipe.render.albedoRamp.stops[3].color.x, 1.0f);
    EXPECT_FLOAT_EQ(recipe.render.albedoRamp.stops[3].color.y, 0.0f);

    m_undo.Undo();
    EXPECT_FALSE(LoadRecipe(kSmokePath).render.useAlbedoRamp);

    /// @note color_key はどの形の発生源にも効く項目としてスキーマに載る。
    const JsonValue schema = Send(FluidPayload("fluid.schema", ""), "query");
    ASSERT_TRUE(IsOk(schema)) << SerializeJson(schema);
    const JsonValue* sphere = schema.Find("result")->Find("operators")->Find("source")->Find("fields")->Find("sphere");
    ASSERT_NE(sphere, nullptr);
    EXPECT_TRUE(ArrayHasString(sphere, "color_key")) << SerializeJson(*sphere);
}

} // namespace fbzz::tests
