/// @file    RenderPassCaptureTests.cpp
/// @brief   パス選択の同名識別と構成変更・計測欠損時の診断状態を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>

namespace fbzz::tests {

class RenderPassCaptureTest : public testkit::Fixture {};

TEST_F(RenderPassCaptureTest, SelectionTracksNameAndOccurrenceWhenEarlierPassesAreInserted)
{
    scene::RenderPassCapture capture;
    capture.request.passName = "Effect";
    capture.request.occurrence = 1;
    renderer::RenderGraph graph;
    graph.AddPass("Effect", {}, { "HDR" }, [] {});
    graph.AddPass("Effect", { "HDR" }, { "HDR" }, [] {});
    graph.SetOutputs({ "HDR" });
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    EXPECT_FALSE(capture.WantsPass(0));
    EXPECT_TRUE(capture.WantsPass(1));

    graph.Clear();
    graph.AddPass("Setup", {}, {}, [] {}, false);
    graph.AddPass("Effect", {}, { "HDR" }, [] {});
    graph.AddPass("Effect", { "HDR" }, { "HDR" }, [] {});
    graph.SetOutputs({ "HDR" });
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    EXPECT_FALSE(capture.WantsPass(1));
    EXPECT_TRUE(capture.WantsPass(2));
    ASSERT_EQ(capture.Passes().size(), 3u);
    EXPECT_EQ(capture.Passes()[2].occurrence, 1u);
}

TEST_F(RenderPassCaptureTest, CulledAndMissingSelectionsNeverKeepThePreviousIndex)
{
    scene::RenderPassCapture capture;
    capture.request.passName = "Bloom";
    renderer::RenderGraph graph;
    graph.AddPass("Bloom", {}, { "Bloom" }, [] {});
    graph.SetOutputs({ "Bloom" });
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    EXPECT_TRUE(capture.WantsPass(0));

    graph.AddPass("Composite", {}, { "Output" }, [] {});
    graph.SetOutputs({ "Output" });
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    EXPECT_FALSE(capture.WantsPass(0));
    EXPECT_FALSE(capture.HasPreview());
    ASSERT_EQ(capture.Passes().size(), 2u);
    EXPECT_TRUE(capture.Passes().back().culled);
    EXPECT_NE(capture.Status().find("culled"), std::string::npos);

    graph.Clear();
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    EXPECT_FALSE(capture.WantsPass(0));
    EXPECT_TRUE(capture.Passes().empty());
    EXPECT_TRUE(capture.Outputs().empty());
    EXPECT_NE(capture.Status().find("not active"), std::string::npos);
}

TEST_F(RenderPassCaptureTest, AmbiguousGpuSamplesStayUnavailable)
{
    scene::RenderPassCapture capture;
    renderer::RenderGraph graph;
    graph.AddPass("Effect", {}, { "HDR" }, [] {});
    graph.AddPass("Effect", { "HDR" }, { "HDR" }, [] {});
    graph.AddPass("Composite", { "HDR" }, { "Output" }, [] {});
    graph.SetOutputs({ "Output" });
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    auto report = graph.GetLastReport();
    report.profiles = { { "Effect", 1.0 }, { "Effect", 2.0 }, { "Composite", 3.0 } };
    capture.Finish(report, { { "Effect", 4.0 }, { "Composite", 5.0 } });
    ASSERT_EQ(capture.Passes().size(), 3u);
    EXPECT_LT(capture.Passes()[0].gpuMs, 0.0);
    EXPECT_LT(capture.Passes()[1].gpuMs, 0.0);
    EXPECT_NEAR(capture.Passes()[2].gpuMs, 5.0, 0.000001);
    EXPECT_NEAR(capture.Passes()[1].cpuMs, 2.0, 0.000001);
    capture.Invalidate();
    EXPECT_TRUE(capture.Passes().empty());
    EXPECT_FALSE(capture.WantsPass(2));
    EXPECT_FALSE(capture.HasPreview());
}

} // namespace fbzz::tests
