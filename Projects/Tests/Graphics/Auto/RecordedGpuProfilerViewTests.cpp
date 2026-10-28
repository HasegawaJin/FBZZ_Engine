/// @file    RecordedGpuProfilerViewTests.cpp
/// @brief   非同期GPU計測区間の失敗抑止・所有値・同期境界と取消しを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-22
#include "../Fixtures/ProfilerRecordingProbe.hpp"
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <TestKit/TestKit.hpp>

namespace fbzz::tests {
class RecordedGpuProfilerViewTest : public testkit::Fixture {
protected:
    void SetUp() override { testkit::Fixture::SetUp(); m_bundle = MakeProfilerRecordingProbe(m_result); }
    void TearDown() override { m_bundle.renderer->WaitForCpuFrame(); m_bundle.imguiRenderer.reset(); m_bundle.renderer.reset(); testkit::Fixture::TearDown(); }
    renderer::GpuProfilerViewMetadata View(uint64_t id = 7) { renderer::GpuProfilerViewMetadata view; view.viewId = id; view.width = view.height = 32; return view; }
    void Pass(const char* name = "work") { m_bundle.renderer->GpuProfBeginPass(name); m_bundle.renderer->Clear({}); m_bundle.renderer->GpuProfEndPass(name); }
    void Finish() { m_bundle.renderer->EndFrame(); m_bundle.renderer->WaitForCpuFrame(); }
    ProfilerRecordingResult m_result;
    renderer::RendererBundle m_bundle;
};

TEST_F(RecordedGpuProfilerViewTest, OwnsMetadataAndNamesUntilWorkerExecutesInOrder)
{
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1); r.BeginFrame();
    auto view = View();
    ASSERT_TRUE(r.GpuProfRecordView(view));
    view.viewId = 99;
    std::string name = "owned";
    Pass(name.c_str()); name = "changed";
    r.GpuProfEndRecordedView(); Finish();
    ASSERT_EQ(m_result.views.size(), 1u);
    EXPECT_EQ(m_result.views.front().viewId, 7u);
    EXPECT_EQ(m_result.events, (std::vector<std::string>{"frame", "begin:7", "pass:owned", "draw", "end-pass:owned", "end", "end-frame"}));
}

TEST_F(RecordedGpuProfilerViewTest, RejectsNativeViewWithoutDroppingDrawsOrEndingAnotherView)
{
    m_result.reject = true;
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1); r.BeginFrame();
    ASSERT_TRUE(r.GpuProfRecordView(View())); Pass(); r.GpuProfEndRecordedView(); Finish();
    EXPECT_EQ(m_result.clears, 1u); EXPECT_EQ(m_result.passes, 0u); EXPECT_EQ(m_result.ends, 0u);
}

TEST_F(RecordedGpuProfilerViewTest, SuppressesRejectedNestedScopeAndRestoresParentMarkers)
{
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1); r.BeginFrame();
    ASSERT_TRUE(r.GpuProfRecordView(View(1))); Pass("before");
    ASSERT_TRUE(r.GpuProfRecordView(View(2))); Pass("nested"); r.GpuProfEndRecordedView();
    Pass("after"); r.GpuProfEndRecordedView(); Finish();
    EXPECT_EQ(m_result.clears, 3u); EXPECT_EQ(m_result.passes, 2u); EXPECT_EQ(m_result.ends, 1u);
    EXPECT_EQ(m_result.events, (std::vector<std::string>{"frame", "begin:1", "pass:before", "draw", "end-pass:before", "begin:2", "draw", "pass:after", "draw", "end-pass:after", "end", "end-frame"}));
}

TEST_F(RecordedGpuProfilerViewTest, PreservesRealDispatchReceiptAcrossPacketSplit)
{
    auto& r = *m_bundle.renderer;
    renderer::ResourceManager resources(r);
    r.BeginCpuFrame(1); r.BeginFrame(); ASSERT_TRUE(r.GpuProfRecordView(View())); Pass("before");
    EXPECT_FALSE(r.TryDispatch({}, resources));
    Pass("after"); r.GpuProfEndRecordedView(); Finish();
    EXPECT_EQ(m_result.begins, 1u); EXPECT_EQ(m_result.ends, 1u); EXPECT_EQ(m_result.passes, 2u);
    ASSERT_GT(m_result.events.size(), 5u);
    EXPECT_EQ(m_result.events[5], "dispatch-receipt");
}

TEST_F(RecordedGpuProfilerViewTest, CancelsUnsubmittedScopeWithoutNativeSideEffects)
{
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1); r.BeginFrame(); ASSERT_TRUE(r.GpuProfRecordView(View())); Pass(); r.GpuProfEndRecordedView();
    r.CancelCpuFrame(); r.WaitForCpuFrame(); EXPECT_TRUE(m_result.events.empty());
}

TEST_F(RecordedGpuProfilerViewTest, ClosesScopeWhosePendingEndWasCancelledAfterSynchronousSplit)
{
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1); r.BeginFrame(); ASSERT_TRUE(r.GpuProfRecordView(View(1)));
    r.ExecuteResourceOperation([] {}); r.GpuProfEndRecordedView(); r.CancelCpuFrame();
    r.BeginCpuFrame(2); r.BeginFrame(); ASSERT_TRUE(r.GpuProfRecordView(View(2))); Pass(); r.GpuProfEndRecordedView(); Finish();
    EXPECT_EQ(m_result.ends, 2u); EXPECT_EQ(m_result.passes, 1u);
    EXPECT_EQ(m_result.events, (std::vector<std::string>{"frame", "begin:1", "end", "frame", "begin:2", "pass:work", "draw", "end-pass:work", "end", "end-frame"}));
}

TEST_F(RecordedGpuProfilerViewTest, RecordsNextViewWhilePreviousPacketIsStillExecuting)
{
    auto& r = *m_bundle.renderer;
    m_result.blockClear = true;
    r.BeginCpuFrame(1); r.BeginFrame(); r.Clear({}); r.EndFrame(); m_result.entered.acquire();
    /// @note The worker cannot progress until this producer records the next complete scope and releases it.
    r.BeginCpuFrame(2); r.BeginFrame(); const bool accepted = r.GpuProfRecordView(View()); Pass(); r.GpuProfEndRecordedView();
    m_result.resume.release();
    EXPECT_TRUE(accepted); Finish(); EXPECT_EQ(m_result.ends, 1u); EXPECT_EQ(m_result.clears, 2u);
}

TEST_F(RecordedGpuProfilerViewTest, KeepsSynchronousBeginResultAndNativeFallbackContract)
{
    m_result.reject = true;
    EXPECT_FALSE(m_bundle.renderer->GpuProfBeginView(View()));
    ProfilerRecordingProbe native(m_result);
    EXPECT_FALSE(native.GpuProfRecordView(View()));
    m_result.reject = false;
    ASSERT_TRUE(native.GpuProfRecordView(View())); native.GpuProfEndRecordedView();
    EXPECT_EQ(m_result.ends, 1u);
}

TEST_F(RecordedGpuProfilerViewTest, PipelineRecordsViewAndGraphHooksWhilePreviousPacketIsExecuting)
{
    auto& r = *m_bundle.renderer;
    renderer::ResourceManager resources(r);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, r, resources, camera, settings, {}, ~0u, handles};
    renderer::RenderPipeline pipeline;
    renderer::ResolvedRenderPlan plan;
    auto view = View();
    pipeline.AddRawPass("pipeline", std::vector<renderer::RenderGraph::ResourceAccess>{}, [&] { r.Clear({}); }, false);
    pipeline.SetGpuProfilerHooks([&](std::string_view name) { r.GpuProfBeginPass(name.data()); },
        [&](std::string_view name) { r.GpuProfEndPass(name.data()); });
    m_result.blockClear = true;
    r.BeginCpuFrame(1); r.BeginFrame(); r.Clear({}); r.EndFrame(); m_result.entered.acquire();

    r.BeginCpuFrame(2); r.BeginFrame();
    const bool executed = pipeline.Execute(context, nullptr, &view, &plan);
    m_result.resume.release(); Finish();

    EXPECT_TRUE(executed); EXPECT_EQ(m_result.passes, 1u); EXPECT_EQ(m_result.ends, 1u);
    EXPECT_EQ(m_result.events, (std::vector<std::string>{"frame", "draw", "end-frame", "frame", "begin:7", "pass:pipeline", "draw", "end-pass:pipeline", "end", "end-frame"}));
}
}
