/// @file    InputRecorderTests.cpp
/// @brief   入力の記録が «変化したフレームだけ» を再生可能な input.inject として残すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Ai/Json.hpp>
#include <Editor/Playtest/InputRecorder.hpp>
#include <Engine/Input/Input.hpp>

#include <string>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;
using editor::playtest::InputRecorder;

class InputRecorderTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        input::Input::Reset();
    }

    void TearDown() override
    {
        input::Input::ClearInjected();
        input::Input::Reset();
        EngineFixture::TearDown();
    }

    /// @brief 1 フレームぶん: 前フレームの状態を退避してから記録する (Application のループと同じ順)。
    static void Frame(InputRecorder& recorder)
    {
        recorder.Capture();
        input::Input::Update();
    }
};

const JsonValue* FindKeyEvent(const JsonValue& events, const std::string& key, bool pressed)
{
    for (const JsonValue& event : events.AsArray()) {
        const JsonValue* inject = event.Find("inject");
        if (inject == nullptr || inject->Find("kind")->AsString() != "key") continue;
        if (inject->Find("key")->AsString() == key && inject->Find("pressed")->AsBool() == pressed) return &event;
    }
    return nullptr;
}

} // namespace

TEST_F(InputRecorderTest, RecordsKeyPressAndReleaseOnTheFramesTheyChanged)
{
    InputRecorder recorder;
    recorder.Start();

    Frame(recorder);                                        ///< frame 0: 何も押していない
    ASSERT_TRUE(input::Input::InjectKey('W', true));
    Frame(recorder);                                        ///< frame 1: 押した
    Frame(recorder);                                        ///< frame 2: 押しっぱなし (記録しない)
    ASSERT_TRUE(input::Input::InjectKey('W', false));
    Frame(recorder);                                        ///< frame 3: 離した

    const JsonValue result = recorder.Stop();
    const JsonValue& events = *result.Find("events");
    const std::string key = "#" + std::to_string(static_cast<int>('W'));

    const JsonValue* press = FindKeyEvent(events, key, true);
    const JsonValue* release = FindKeyEvent(events, key, false);
    ASSERT_NE(press, nullptr);
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(press->Find("frame")->AsInt(), 1);
    EXPECT_EQ(release->Find("frame")->AsInt(), 3);
    EXPECT_EQ(result.Find("frames")->AsInt(), 4);
}

TEST_F(InputRecorderTest, CapturesNothingWhenNotRecording)
{
    InputRecorder recorder;
    ASSERT_TRUE(input::Input::InjectKey('A', true));
    Frame(recorder);

    EXPECT_FALSE(recorder.IsRecording());
    EXPECT_EQ(recorder.Stop().Find("events")->AsArray().size(), 0u);
}

TEST_F(InputRecorderTest, StopEndsTheRecording)
{
    InputRecorder recorder;
    recorder.Start();
    Frame(recorder);
    static_cast<void>(recorder.Stop());

    EXPECT_FALSE(recorder.IsRecording());
}

} // namespace fbzz::tests
