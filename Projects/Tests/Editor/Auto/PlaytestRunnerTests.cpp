/// @file    PlaytestRunnerTests.cpp
/// @brief   Playtest シナリオのフレーム駆動 (待機・表明・打ち切り・レポート・ロックステップ) を偽のバスで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Playtest/ImageCompare.hpp>
#include <Editor/Playtest/PlaytestRunner.hpp>
#include <Engine/Core/Time.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;
using editor::ai::ParseJson;
using editor::ai::SerializeJson;
using editor::playtest::PlaytestHooks;
using editor::playtest::PlaytestOptions;
using editor::playtest::PlaytestRunner;
using editor::playtest::PlaytestState;

JsonValue Parse(const std::string& text)
{
    auto parsed = ParseJson(text, nullptr);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(JsonValue{});
}

/// @brief 要求の t ごとに結果を返す偽のバス。呼ばれた t を順に記録する。
class FakeBus {
public:
    int counter = 0;
    std::vector<std::string> calls;

    PlaytestHooks Hooks()
    {
        PlaytestHooks hooks;
        hooks.bus = [this](const std::string& line) { return Handle(line); };
        hooks.capture = [](std::string_view, std::vector<uint8_t>&) { return false; };
        return hooks;
    }

private:
    std::string Handle(const std::string& line)
    {
        const JsonValue request = Parse(line);
        const std::string id = request.Find("id")->AsString();
        const JsonValue& payload = *request.Find("payload");
        const std::string type = payload.Find("t")->AsString();
        calls.push_back(type);

        JsonValue result = JsonValue::MakeObject();
        if (type == "fake.counter") {
            result.Set("n", JsonValue(++counter));
        } else if (type == "fake.echo") {
            if (const JsonValue* value = payload.Find("value")) result.Set("value", *value);
        } else if (type == "fake.fail") {
            return SerializeJson(editor::ai::MakeErrorResponse(id, "BOOM", "失敗させた"));
        } else if (type == "editor.state") {
            result.Set("playState", JsonValue("editor"));
        }
        return SerializeJson(editor::ai::MakeOkResponse(id, std::move(result)));
    }
};

class PlaytestRunnerTest : public testkit::EngineFixture {
protected:
    void TearDown() override
    {
        fbzz::Time::SetLockstepDelta(0.0f);
        EngineFixture::TearDown();
    }

    bool Start(PlaytestRunner& runner, const std::string& scenarioJson)
    {
        std::string error;
        const bool started = runner.Start(Parse(scenarioJson), {}, m_temp.Path(), m_temp.Path() / "out",
                                          PlaytestOptions{}, error);
        EXPECT_TRUE(started) << error;
        return started;
    }

    /// @return 終わるまでに回した Tick の回数。上限で打ち切る。
    int RunToEnd(PlaytestRunner& runner, FakeBus& bus, int limit = 1000)
    {
        const PlaytestHooks hooks = bus.Hooks();
        int ticks = 0;
        while (runner.IsRunning() && ticks < limit) {
            runner.Tick(hooks);
            ++ticks;
        }
        return ticks;
    }

    testkit::TempDir m_temp{"playtest"};
};

} /// @note namespace

TEST_F(PlaytestRunnerTest, CapturesCommandResultsAndPreservesTypesInLaterRequestsAndAssertions)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"bus","request":{"t":"fake.counter"},"capture":{"number":"n","whole":""}},
        {"do":"bus","kind":"query","request":{"t":"fake.echo","value":[{"$ref":"whole.n"},true,null,"token"]},"capture":{"items":"value"}},
        {"do":"assert","query":{"t":"fake.echo","value":{"$ref":"items"}},"path":"value.0","op":"==","value":{"$ref":"number"}},
        {"do":"assert","query":{"t":"fake.echo","value":{"$ref":"items"}},"path":"value.1","op":"==","value":true},
        {"do":"assert","query":{"t":"fake.echo","value":{"$ref":"items"}},"path":"value.2","op":"==","value":null}
    ]})"));
    RunToEnd(runner, bus);
    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
    EXPECT_EQ(bus.counter, 1);
    const JsonValue report = runner.Report();
    const auto& steps = report.Find("steps")->AsArray();
    ASSERT_GE(steps.size(), 2u);
    ASSERT_NE(steps[0].Find("result"), nullptr);
    EXPECT_TRUE(steps[0].Find("result")->Find("n")->IsNumber());
}

TEST_F(PlaytestRunnerTest, WaitCaptureUsesOnlyTheSatisfiedResponseAndStartClearsOldValues)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"waitUntil","query":{"t":"fake.counter"},"path":"n","op":">=","value":3,"capture":{"ready":"n"}},
        {"do":"assert","query":{"t":"fake.echo","value":{"$ref":"ready"}},"path":"value","op":"==","value":3}
    ]})"));
    RunToEnd(runner, bus);
    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
    EXPECT_EQ(bus.counter, 3);
    const auto echoCalls = std::count(bus.calls.begin(), bus.calls.end(), "fake.echo");
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"waitUntil","query":{"t":"fake.echo","value":{"$ref":"ready"}},"path":"value","op":"exists"}
    ]})"));
    RunToEnd(runner, bus);
    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    EXPECT_EQ(std::count(bus.calls.begin(), bus.calls.end(), "fake.echo"), echoCalls);
    EXPECT_NE(runner.Report().Find("failure")->AsString().find("ready"), std::string::npos);
}

TEST_F(PlaytestRunnerTest, RejectsInvalidCaptureAndUnresolvedReferenceBeforeCallingBus)
{
    const char* scenarios[] = {
        R"({"steps":[{"do":"bus","request":{"t":"fake.counter"},"capture":{"bad.key":"n"}}]})",
        R"({"steps":[{"do":"bus","request":{"t":"fake.counter"},"capture":{"key":2}}]})",
        R"({"steps":[{"do":"bus","request":{"t":"fake.counter"},"capture":{"key":"n"},"expectError":"BOOM"}]})",
        R"({"steps":[{"do":"bus","request":{"t":"fake.echo","value":{"$ref":"missing"}}}]})",
        R"({"steps":[{"do":"bus","request":{"t":"fake.echo","value":{"$ref":"missing","extra":1}}}]})",
        R"({"steps":[{"do":"waitUntil","query":{"t":"fake.counter"},"path":"n","op":"<","value":0,"capture":{"bad.key":"n"}}]})"
    };
    for (const char* scenario : scenarios) {
        SCOPED_TRACE(scenario);
        PlaytestRunner runner;
        FakeBus bus;
        ASSERT_TRUE(Start(runner, scenario));
        RunToEnd(runner, bus);
        EXPECT_EQ(runner.State(), PlaytestState::FAILED);
        EXPECT_EQ(std::count(bus.calls.begin(), bus.calls.end(), "fake.counter"), 0);
        EXPECT_EQ(std::count(bus.calls.begin(), bus.calls.end(), "fake.echo"), 0);
    }
}

TEST_F(PlaytestRunnerTest, MissingCapturePathStopsFollowingMutation)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"bus","request":{"t":"fake.counter"},"capture":{"key":"doesNotExist"}},
        {"do":"bus","request":{"t":"fake.counter"}}
    ]})"));
    RunToEnd(runner, bus);
    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    EXPECT_EQ(bus.counter, 1);
    const JsonValue report = runner.Report();
    ASSERT_FALSE(report.Find("steps")->AsArray().empty());
    const JsonValue& step = report.Find("steps")->AsArray().front();
    ASSERT_NE(step.Find("result"), nullptr);
    EXPECT_EQ(step.Find("result")->Find("n")->AsInt(), 1);
}

TEST_F(PlaytestRunnerTest, RejectsScenarioWithoutSteps)
{
    PlaytestRunner runner;
    std::string error;
    EXPECT_FALSE(runner.Start(Parse(R"({"name":"x"})"), {}, m_temp.Path(), m_temp.Path(), PlaytestOptions{}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(runner.Start(Parse(R"({"steps":[{"nope":1}]})"), {}, m_temp.Path(), m_temp.Path(), PlaytestOptions{}, error));
}

TEST_F(PlaytestRunnerTest, CapturesPreparedFluidPlaybackFrame)
{
    PlaytestRunner runner;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"capture","view":"fluidBaked","name":"JetFlame_3D_Seam", "fluidPath":"Assets/JetFlame.fluid","bakeMode":"3d","frame":63.5,"minForegroundPixels":1}]})"));
    EXPECT_TRUE(runner.IsFluidPlaybackCaptureStep());

    int prepared = 0;
    int captured = 0;
    PlaytestHooks hooks;
    hooks.prepareFluidPlayback = [&](const JsonValue& step, std::string&) {
        ++prepared;
        EXPECT_EQ(step.Find("frame")->AsNumber(), 63.5);
        return true;
    };
    hooks.capture = [&](std::string_view view, std::vector<uint8_t>& png) {
        ++captured;
        EXPECT_EQ(view, "fluidBaked");
        editor::playtest::RgbaImage image{ 2, 1, { 0, 0, 0, 255, 255, 100, 20, 255 } };
        return editor::playtest::EncodePng(image, png);
    };
    for (int tick = 0; tick < 5 && runner.IsRunning(); ++tick) runner.Tick(hooks);

    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
    EXPECT_FALSE(runner.IsFluidPlaybackCaptureStep());
    EXPECT_EQ(prepared, 1);
    EXPECT_EQ(captured, 1);
    EXPECT_TRUE(std::filesystem::exists(m_temp.Path() / "out" / "JetFlame_3D_Seam.actual.png"));
}

TEST_F(PlaytestRunnerTest, RejectsEmptyFluidPlaybackCapture)
{
    PlaytestRunner runner;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"capture","view":"fluidBaked","name":"blank","minForegroundPixels":1}]})"));
    PlaytestHooks hooks;
    hooks.prepareFluidPlayback = [](const JsonValue&, std::string&) { return true; };
    hooks.capture = [](std::string_view, std::vector<uint8_t>& png) {
        editor::playtest::RgbaImage image{ 2, 1, { 0, 0, 0, 255, 0, 0, 0, 255 } };
        return editor::playtest::EncodePng(image, png);
    };
    for (int tick = 0; tick < 5 && runner.IsRunning(); ++tick) runner.Tick(hooks);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    EXPECT_EQ(runner.Report().Find("images")->AsArray().front().Find("status")->AsString(), "empty-playback");
}

TEST_F(PlaytestRunnerTest, FramesStepWaitsExactlyThatManyTicks)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"frames","count":5},{"do":"log","message":"done"}]})"));

    const int ticks = RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
    /// @note 1 Tick 目で frames を受け、続く 5 Tick を待ち、7 Tick 目で log を実行して終わる。
    EXPECT_EQ(ticks, 7);
}

TEST_F(PlaytestRunnerTest, ZeroSecondWaitReportsTheMonotonicClockAndCompletes)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"waitSeconds","seconds":0},{"do":"waitSeconds","seconds":0}]})"));
    RunToEnd(runner, bus);
    ASSERT_EQ(runner.State(), PlaytestState::PASSED);
    const auto report = runner.Report();
    ASSERT_EQ(report.Find("steps")->AsArray().size(), 2u);
    for (const auto& step : report.Find("steps")->AsArray()) {
        const auto& result = *step.Find("result");
        EXPECT_EQ(result.Find("requestedSeconds")->AsNumber(), 0.0);
        EXPECT_GE(result.Find("elapsedSeconds")->AsNumber(), 0.0);
        EXPECT_EQ(result.Find("clock")->AsString(), "steady_clock");
    }
}

TEST_F(PlaytestRunnerTest, SecondWaitStartsWhileRenderingAndKeepsTheFollowingStepPending)
{
    PlaytestRunner runner;
    int renders = 0;
    PlaytestHooks hooks;
    hooks.keepViewportsRendering = [&] { ++renders; };
    ASSERT_TRUE(Start(runner, R"({"lockstep":1000,"steps":[{"do":"waitSeconds","seconds":3600},{"do":"log","message":"done"}]})"));
    runner.Tick(hooks);
    EXPECT_EQ(runner.State(), PlaytestState::RUNNING);
    EXPECT_EQ(renders, 1);
    EXPECT_TRUE(runner.Report().Find("steps")->AsArray().empty());
    runner.Cancel("test completed");
}

TEST_F(PlaytestRunnerTest, RejectsInvalidSecondWaitBeforeFollowingSteps)
{
    for (const auto* seconds : {"-1", "3601", "null"}) {
        PlaytestRunner runner;
        FakeBus bus;
        ASSERT_TRUE(Start(runner, std::string(R"({"steps":[{"do":"waitSeconds","seconds":)") + seconds + R"(},{"do":"log","message":"unreachable"}]})"));
        RunToEnd(runner, bus);
        EXPECT_EQ(runner.State(), PlaytestState::FAILED);
        EXPECT_EQ(runner.Report().Find("steps")->AsArray().size(), 1u);
    }
}

TEST_F(PlaytestRunnerTest, WaitUntilPollsEveryFrameUntilTheConditionHolds)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"waitUntil","query":{"t":"fake.counter"},"path":"n","op":">=","value":4,"timeoutFrames":100}
    ]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
    EXPECT_EQ(bus.counter, 4);
}

TEST_F(PlaytestRunnerTest, WaitUntilFailsAfterTheTimeout)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"waitUntil","query":{"t":"fake.counter"},"path":"n","op":"<","value":0,"timeoutFrames":3}
    ]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    const JsonValue report = runner.Report();
    ASSERT_NE(report.Find("failure"), nullptr);
    EXPECT_NE(report.Find("failure")->AsString().find("3"), std::string::npos);
}

TEST_F(PlaytestRunnerTest, AssertStopsAtTheFirstFailureAndSkipsTheRest)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"assert","query":{"t":"fake.counter"},"path":"n","op":"==","value":99},
        {"do":"bus","request":{"t":"never.called"}}
    ]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    EXPECT_EQ(std::count(bus.calls.begin(), bus.calls.end(), "never.called"), 0);
}

TEST_F(PlaytestRunnerTest, BusStepCanExpectASpecificError)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"bus","request":{"t":"fake.fail"},"expectError":"BOOM"}]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::PASSED);
}

TEST_F(PlaytestRunnerTest, BusQueryResultIsKeptInTheReport)
{
    /// @note 計測値 (profiler.snapshot 等) をシナリオから取り出すには、query の応答が報告に残る必要がある。command の応答は残さない。
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[
        {"do":"bus","kind":"query","request":{"t":"fake.counter"}},
        {"do":"bus","request":{"t":"fake.counter"}}
    ]})"));

    RunToEnd(runner, bus);

    ASSERT_EQ(runner.State(), PlaytestState::PASSED);
    const JsonValue report = runner.Report();
    const auto& steps = report.Find("steps")->AsArray();
    ASSERT_EQ(steps.size(), 2u);
    const JsonValue* queried = steps[0].Find("result");
    ASSERT_NE(queried, nullptr);
    EXPECT_EQ(queried->Find("n")->AsNumber(), 1.0);
    EXPECT_EQ(steps[1].Find("result"), nullptr);
}

TEST_F(PlaytestRunnerTest, UnknownStepFails)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"teleport"}]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
}

TEST_F(PlaytestRunnerTest, LockstepIsAppliedWhileRunningAndReleasedAfter)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"lockstep":0.02,"steps":[{"do":"frames","count":2}]})"));
    const PlaytestHooks hooks = bus.Hooks();

    runner.Tick(hooks);
    EXPECT_FLOAT_EQ(fbzz::Time::GetLockstepDelta(), 0.02f);

    RunToEnd(runner, bus);
    EXPECT_FLOAT_EQ(fbzz::Time::GetLockstepDelta(), 0.0f);
}

TEST_F(PlaytestRunnerTest, WritesTheReportFileWhenFinished)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"name":"report","steps":[{"do":"log","message":"hi"}]})"));

    RunToEnd(runner, bus);

    ASSERT_TRUE(std::filesystem::exists(runner.ReportPath()));
    EXPECT_EQ(runner.Report().Find("state")->AsString(), "passed");
}

TEST_F(PlaytestRunnerTest, CompareImageFailsWhenCaptureIsUnavailable)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"compareImage","baseline":"Missing"}]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
}

TEST_F(PlaytestRunnerTest, CompareImageRejectsNamesThatEscapeTheGoldenDirectory)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"compareImage","baseline":"../../evil"}]})"));

    RunToEnd(runner, bus);

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
}

TEST_F(PlaytestRunnerTest, CancelEndsAsFailed)
{
    PlaytestRunner runner;
    FakeBus bus;
    ASSERT_TRUE(Start(runner, R"({"steps":[{"do":"frames","count":100}]})"));
    runner.Tick(bus.Hooks());

    runner.Cancel("テストで中断");

    EXPECT_EQ(runner.State(), PlaytestState::FAILED);
    EXPECT_FLOAT_EQ(fbzz::Time::GetLockstepDelta(), 0.0f);
}

} /// @note namespace fbzz::tests
