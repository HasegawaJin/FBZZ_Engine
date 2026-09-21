/// @file    ProfilerTests.cpp
/// @brief   フレーム公開、階層、収集停止の契約を時間計測値に依存せず検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>

namespace fbzz::tests {

using profiler::Profiler;
using profiler::ProfilerMarker;

class ProfilerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_wasEnabled = Profiler::IsEnabled();
        Profiler::SetEnabled(false);
        Profiler::SetEnabled(true);
    }

    void TearDown() override
    {
        Profiler::SetEnabled(false);
        Profiler::SetEnabled(m_wasEnabled);
        EngineFixture::TearDown();
    }

private:
    bool m_wasEnabled = true;
};

TEST_F(ProfilerTest, PublishesSamplesOnlyWhenTheFrameEnds)
{
    Profiler::BeginFrame();
    Profiler::PushSample(ProfilerMarker("job", "worker", 123u), 2.5);
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());

    Profiler::EndFrame();

    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_STREQ(records[0].name, "job");
    EXPECT_STREQ(records[0].category, "worker");
    EXPECT_EQ(records[0].color, 123u);
    EXPECT_DOUBLE_EQ(records[0].elapsedMs, 2.5);
    EXPECT_EQ(records[0].depth, 0u);
    EXPECT_EQ(records[0].frameIndex, Profiler::GetLastFrameIndex());
}

TEST_F(ProfilerTest, KeepsThePublishedSnapshotUntilTheNextFrameEnds)
{
    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("previous"));
    Profiler::EndFrame();
    const auto previousIndex = Profiler::GetLastFrameIndex();

    Profiler::BeginFrame();

    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "previous");
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex);
    Profiler::EndFrame();
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex + 1);
}

TEST_F(ProfilerTest, RecordsNestedSamplesAndInstantMarkersAtTheirStackDepth)
{
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("parent"));
    Profiler::BeginSample(ProfilerMarker("child"));
    Profiler::PushMarker(ProfilerMarker("event", "flow", 456u));
    Profiler::PushSample(ProfilerMarker("external"), 1.25);
    Profiler::EndSample();
    Profiler::EndSample();

    Profiler::EndFrame();

    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), 4u);
    EXPECT_STREQ(records[0].name, "event");
    EXPECT_STREQ(records[0].category, "flow");
    EXPECT_EQ(records[0].color, 456u);
    EXPECT_DOUBLE_EQ(records[0].elapsedMs, 0.0);
    EXPECT_EQ(records[0].depth, 2u);
    EXPECT_EQ(records[1].depth, 2u);
    EXPECT_DOUBLE_EQ(records[1].elapsedMs, 1.25);
    EXPECT_STREQ(records[2].name, "child");
    EXPECT_EQ(records[2].depth, 1u);
    EXPECT_STREQ(records[3].name, "parent");
    EXPECT_EQ(records[3].depth, 0u);
}

TEST_F(ProfilerTest, DisablingClearsPublishedAndPendingSamplesAndIgnoresCollection)
{
    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("published"));
    Profiler::EndFrame();
    const auto previousIndex = Profiler::GetLastFrameIndex();
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("unfinished"));
    Profiler::PushMarker(ProfilerMarker("pending"));

    Profiler::SetEnabled(false);
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("ignored"));
    Profiler::PushSample(ProfilerMarker("ignored"), 3.0);
    Profiler::PushMarker(ProfilerMarker("ignored"));
    Profiler::EndSample();
    Profiler::EndFrame();

    EXPECT_FALSE(Profiler::IsEnabled());
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex);
    Profiler::SetEnabled(true);
    Profiler::BeginFrame();
    Profiler::EndFrame();
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
}

} /// @note namespace fbzz::tests
