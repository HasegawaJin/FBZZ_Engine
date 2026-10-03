/// @file    RenderPassCaptureTests.cpp
/// @brief   パス選択の同名識別と構成変更・計測欠損時の診断状態を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>
#include <limits>

namespace fbzz::tests {

class RenderPassCaptureTest : public testkit::Fixture {
protected:
    renderer::GpuProfilerViewMetadata Metadata() const
    {
        renderer::GpuProfilerViewMetadata result;
        result.applicationFrameSerial = 42;
        result.viewId = 2;
        result.sceneGeneration = 19;
        result.planGeneration = 7;
        result.resourceEpoch = 3;
        result.width = 640;
        result.height = 480;
        result.outputId = 11;
        result.outputGeneration = 5;
        return result;
    }

    renderer::GpuProfilerSnapshot Snapshot(const renderer::GpuProfilerViewMetadata& metadata) const
    {
        renderer::GpuProfilerSnapshot result;
        result.supported = result.available = result.complete = true;
        result.physicalFrameSerial = 23;
        result.deviceEpoch = 9;
        result.recordedPassCount = 1;
        renderer::GpuPassProfile pass;
        pass.name = "Effect";
        pass.gpuMs = 4;
        pass.metadata = metadata;
        pass.physicalFrameSerial = result.physicalFrameSerial;
        pass.deviceEpoch = result.deviceEpoch;
        pass.available = true;
        result.passes.push_back(pass);
        return result;
    }

    renderer::RenderGraph::ExecutionReport BeginSinglePass(scene::RenderPassCapture& capture) const
    {
        renderer::RenderGraph graph;
        graph.AddPass("Effect", {}, {"Output"}, [] {});
        graph.SetOutputs({"Output"});
        EXPECT_TRUE(graph.Plan());
        capture.Begin(graph.GetPasses(), graph.GetLastReport());
        auto report = graph.GetLastReport();
        report.profiles = {{"Effect", 2}};
        return report;
    }
};

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
    const auto metadata = Metadata();
    auto snapshot = Snapshot(metadata);
    auto composite = snapshot.passes.front();
    composite.name = "Composite";
    composite.gpuMs = 5;
    snapshot.passes.push_back(composite);
    snapshot.recordedPassCount = 2;
    capture.Finish(report, snapshot, metadata);
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

TEST_F(RenderPassCaptureTest, ExactTaggedSamplesRequireFinalPlanProvenance)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    auto metadata = Metadata();
    const auto snapshot = Snapshot(metadata);
    capture.Finish(report, snapshot, metadata);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].cpuMs, 2);
    metadata.planGeneration = 0;
    capture.Finish(report, snapshot, metadata);
    EXPECT_LT(capture.Passes()[0].gpuMs, 0);
}

TEST_F(RenderPassCaptureTest, EverySourceMetadataDifferenceRejectsTheDelayedGpuSample)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    const auto metadata = Metadata();
    const auto valid = Snapshot(metadata);
    for (uint32_t difference = 0; difference < 9; ++difference) {
        SCOPED_TRACE(difference);
        capture.Finish(report, valid, metadata);
        EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
        auto delayed = valid;
        auto& source = delayed.passes[0].metadata;
        switch (difference) {
        case 0: --source.applicationFrameSerial; break;
        case 1: ++source.viewId; break;
        case 2: ++source.sceneGeneration; break;
        case 3: ++source.planGeneration; break;
        case 4: ++source.resourceEpoch; break;
        case 5: ++source.width; break;
        case 6: ++source.height; break;
        case 7: ++source.outputId; break;
        default: ++source.outputGeneration; break;
        }
        capture.Finish(report, delayed, metadata);
        EXPECT_LT(capture.Passes()[0].gpuMs, 0);
        EXPECT_DOUBLE_EQ(capture.Passes()[0].cpuMs, 2);
    }
}

TEST_F(RenderPassCaptureTest, PhysicalFrameAndDeviceMustMatchTheCompletedSnapshot)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    const auto metadata = Metadata();
    const auto valid = Snapshot(metadata);
    for (uint32_t difference = 0; difference < 4; ++difference) {
        SCOPED_TRACE(difference);
        capture.Finish(report, valid, metadata);
        EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
        auto mismatched = valid;
        switch (difference) {
        case 0: ++mismatched.passes[0].physicalFrameSerial; break;
        case 1: ++mismatched.passes[0].deviceEpoch; break;
        case 2: ++mismatched.physicalFrameSerial; break;
        default: ++mismatched.deviceEpoch; break;
        }
        capture.Finish(report, mismatched, metadata);
        EXPECT_LT(capture.Passes()[0].gpuMs, 0);
    }
}

TEST_F(RenderPassCaptureTest, PartialUnavailableAndMissingSnapshotsNeverRetainAnEarlierValue)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    const auto metadata = Metadata();
    const auto valid = Snapshot(metadata);
    for (uint32_t failure = 0; failure < 4; ++failure) {
        SCOPED_TRACE(failure);
        capture.Finish(report, valid, metadata);
        EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
        auto missing = valid;
        switch (failure) {
        case 0: missing.complete = false; missing.droppedPassCount = 1; break;
        case 1: missing.available = false; break;
        case 2: missing.passes.clear(); missing.recordedPassCount = 0; break;
        default: missing = {}; break;
        }
        capture.Finish(report, missing, metadata);
        EXPECT_LT(capture.Passes()[0].gpuMs, 0);
    }
}

TEST_F(RenderPassCaptureTest, NonfiniteNegativeAndUnavailablePassDurationsAreNotPublished)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    const auto metadata = Metadata();
    const auto valid = Snapshot(metadata);
    for (uint32_t failure = 0; failure < 4; ++failure) {
        SCOPED_TRACE(failure);
        capture.Finish(report, valid, metadata);
        EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
        auto invalid = valid;
        switch (failure) {
        case 0: invalid.passes[0].gpuMs = std::numeric_limits<double>::quiet_NaN(); break;
        case 1: invalid.passes[0].gpuMs = std::numeric_limits<double>::infinity(); break;
        case 2: invalid.passes[0].gpuMs = -1; break;
        default: invalid.passes[0].available = false; break;
        }
        capture.Finish(report, invalid, metadata);
        EXPECT_LT(capture.Passes()[0].gpuMs, 0);
    }
    auto zero = valid;
    zero.passes[0].gpuMs = 0;
    capture.Finish(report, zero, metadata);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 0);
}

TEST_F(RenderPassCaptureTest, DuplicateTaggedSamplesAreRejectedButAnotherViewCannotHideTheMatchingSample)
{
    scene::RenderPassCapture capture;
    const auto report = BeginSinglePass(capture);
    ASSERT_EQ(capture.Passes().size(), 1u);
    const auto metadata = Metadata();
    const auto valid = Snapshot(metadata);
    capture.Finish(report, valid, metadata);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
    auto duplicate = valid;
    duplicate.passes.push_back(duplicate.passes.front());
    duplicate.recordedPassCount = 2;
    capture.Finish(report, duplicate, metadata);
    EXPECT_LT(capture.Passes()[0].gpuMs, 0);
    ++duplicate.passes[1].metadata.viewId;
    duplicate.passes[1].gpuMs = 17;
    capture.Finish(report, duplicate, metadata);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 4);
    auto otherView = valid;
    ++otherView.passes[0].metadata.viewId;
    capture.Finish(report, otherView, metadata);
    EXPECT_LT(capture.Passes()[0].gpuMs, 0);
}

TEST_F(RenderPassCaptureTest, CulledAndInvalidatedPassesNeverAcquireDelayedGpuValues)
{
    scene::RenderPassCapture capture;
    renderer::RenderGraph graph;
    graph.AddPass("Effect", {}, {"HDR"}, [] {});
    graph.AddPass("Composite", {}, {"Output"}, [] {});
    graph.SetOutputs({"Output"});
    ASSERT_TRUE(graph.Plan());
    capture.Begin(graph.GetPasses(), graph.GetLastReport());
    auto report = graph.GetLastReport();
    report.profiles = {{"Composite", 2}};
    const auto metadata = Metadata();
    auto snapshot = Snapshot(metadata);
    auto composite = snapshot.passes[0];
    composite.name = "Composite";
    composite.gpuMs = 5;
    snapshot.passes.push_back(composite);
    snapshot.recordedPassCount = 2;
    capture.Finish(report, snapshot, metadata);
    ASSERT_EQ(capture.Passes().size(), 2u);
    EXPECT_FALSE(capture.Passes()[0].culled);
    EXPECT_DOUBLE_EQ(capture.Passes()[0].gpuMs, 5);
    EXPECT_TRUE(capture.Passes()[1].culled);
    EXPECT_LT(capture.Passes()[1].gpuMs, 0);
    capture.Invalidate();
    capture.Finish(report, snapshot, metadata);
    EXPECT_TRUE(capture.Passes().empty());
    EXPECT_FALSE(capture.HasPreview());
}

} /// @note namespace fbzz::tests
