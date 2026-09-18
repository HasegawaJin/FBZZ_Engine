/// @file    PlaytestRunnerTests.cpp
/// @brief   Playtest シナリオのフレーム駆動 (待機・表明・打ち切り・レポート・ロックステップ) を偽のバスで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
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

} // namespace

TEST_F(PlaytestRunnerTest, RejectsScenarioWithoutSteps)
{
    PlaytestRunner runner;
    std::string error;
    EXPECT_FALSE(runner.Start(Parse(R"({"name":"x"})"), {}, m_temp.Path(), m_temp.Path(), PlaytestOptions{}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(runner.Start(Parse(R"({"steps":[{"nope":1}]})"), {}, m_temp.Path(), m_temp.Path(), PlaytestOptions{}, error));
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

} // namespace fbzz::tests
