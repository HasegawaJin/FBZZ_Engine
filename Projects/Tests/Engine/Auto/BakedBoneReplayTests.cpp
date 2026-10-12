/// @file    BakedBoneReplayTests.cpp
/// @brief   GPU palette replay readback for independent clocks, blends and frame lanes.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Graphics/Effects/BakedBoneReplay.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <limits>
#include <vector>

namespace fbzz::tests {
namespace {
class BakedBoneReplayTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Bone bake readback", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_ENGINE_SHADER_ROOT);
        m_replay = m_resources->LoadShader((root / "Pipeline/Skinning/BakedBoneReplay.cs.hlsl").generic_string());
        m_copy = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/BoneReplayCopy.hlsl").generic_string());
        m_state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_replay.IsValid() && m_copy.IsValid() && m_state.IsValid());
        asset::Skeleton skeleton;
        skeleton.rootNodeIndex = 0; skeleton.nodes.resize(3); skeleton.bones.resize(2);
        for (uint32_t i = 0; i < 3; ++i) {
            auto& node = skeleton.nodes[i]; node.name = "Node" + std::to_string(i);
            node.parentIndex = i == 0 ? -1 : static_cast<int>(i - 1);
            node.boneIndex = i == 0 ? -1 : static_cast<int>(i - 1);
            if (i != 2) node.children = {static_cast<int>(i + 1)};
            node.bindTranslation = {static_cast<float>(i) * 0.25f, 0, 0};
            node.localBindTransform = math::Matrix4::TRS(node.bindTranslation, node.bindRotation, node.bindScale);
            if (i) { skeleton.bones[i - 1].nodeIndex = static_cast<int>(i); skeleton.bones[i - 1].name = node.name; }
        }
        skeleton.rootInverseTransform = math::Matrix4::Translate({0.1f, -0.2f, 0.3f});
        asset::AnimationClip clip; clip.name = "Swim"; clip.loop = true;
        clip.durationTicks = clip.durationSeconds = 2; clip.ticksPerSecond = 1;
        asset::NodeAnimationTrack track; track.nodeName = "Node1";
        track.positions = {{0, {0.25f,0,0}}, {1,{0.5f,0.2f,0}}, {2,{0.25f,0,0}}};
        const auto q = math::Quaternion::FromAxisAngle({0,1,0}, 0.8f);
        track.rotations = {{0,math::Quaternion::Identity()}, {1,q}, {2,math::Quaternion::Identity()}};
        track.scales = {{0,{1,1,1}}, {1,{1.2f,0.8f,1}}, {2,{1,1,1}}};
        clip.tracks = {track};
        ASSERT_EQ(asset::BakeBoneAnimation(skeleton, clip, {60,1}, m_source), asset::BoneBakeStatus::OK);
        clip.name = "Turn"; clip.tracks[0].positions[1].value = {-0.1f,0.4f,0.2f};
        clip.tracks[0].rotations[1].value = math::Quaternion::FromAxisAngle({0,0,1}, -0.6f);
        ASSERT_EQ(asset::BakeBoneAnimation(skeleton, clip, {60,1}, m_target), asset::BoneBakeStatus::OK);
        auto samples = m_source.samples; samples.insert(samples.end(), m_target.samples.begin(), m_target.samples.end());
        const renderer::BoneReplaySource upload{std::as_bytes(std::span(samples)), std::as_bytes(std::span(m_source.nodes)),
            std::as_bytes(std::span(m_source.bones)), 3, 2, m_source.rootInverse};
        renderer::BoneReplayStatus status;
        m_gpu = renderer::UploadBakedBoneReplay(*m_resources, upload, status);
        ASSERT_EQ(status, renderer::BoneReplayStatus::OK); ASSERT_NE(m_gpu, nullptr);
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_gpu.reset();
        if (m_resources) renderer::ReleaseBakedBoneReplayCaches(*m_resources);
        m_resources.reset(); m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown(); m_bundle.renderer.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        testkit::EngineFixture::TearDown();
    }
    void Begin()
    {
        m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen = true;
    }
    renderer::BoneReplayResult Replay(std::span<const renderer::BoneReplayRow> rows)
    {
        renderer::RenderPassContext ctx{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
        return renderer::ReplayBakedBones(ctx, *m_gpu, rows, m_replay);
    }
    std::vector<float> Read(renderer::ResourceHandle<renderer::StructuredBufferTag> palette, uint32_t count)
    {
        const auto target = m_resources->CreateRenderTarget(4, count, {1, renderer::Format::RGBA32F, false});
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        renderer::DrawCall copy; copy.shader = m_copy; copy.pipelineState = m_state; copy.vertexCount = 3; copy.psBuffers[0] = palette;
        m_bundle.renderer->Submit(copy, *m_resources); m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame(); m_frameOpen = false;
        std::vector<float> output; uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, output, width, height));
        EXPECT_EQ(width, 4u); EXPECT_EQ(height, count);
        m_resources->Release(target); return output;
    }
    asset::BakedBoneAnimation m_source, m_target;
    std::unique_ptr<renderer::BoneReplayGpuAsset> m_gpu;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
private:
    renderer::Camera m_camera; renderer::RenderSettings m_settings; renderer::RenderPassHandles m_handles;
    renderer::ResourceHandle<renderer::ShaderTag> m_copy, m_replay;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_state;
    HRESULT m_com = E_FAIL; HWND m_window = nullptr; bool m_frameOpen = false;
};
}

TEST_F(BakedBoneReplayTest, ThousandPhasesSpeedsAndBlendsMatchCpuAcrossFrameLanes)
{
    for (uint32_t lane = 0; lane < 6; ++lane) {
        std::vector<renderer::BoneReplayRow> rows;
        std::vector<math::Matrix4> expected;
        for (uint32_t fish = 0; fish < 1000; ++fish) {
            asset::BakedBoneFrame first, second;
            const double speed = fish % 3 == 0 ? -0.5 : fish % 3 == 1 ? 0.0 : 1.25;
            ASSERT_EQ(asset::ResolveBakedBoneFrame(m_source, lane / 60.0, speed, fish * 0.002, true, first), asset::BoneBakeStatus::OK);
            ASSERT_EQ(asset::ResolveBakedBoneFrame(m_target, lane / 60.0, speed, fish * 0.002 + 0.3, true, second), asset::BoneBakeStatus::OK);
            const float blend = static_cast<float>(fish % 5) / 4.0f;
            const uint32_t targetOffset = static_cast<uint32_t>(m_source.samples.size());
            rows.push_back({first.first*3,first.second*3,first.alpha,0,targetOffset+second.first*3,targetOffset+second.second*3,second.alpha,blend});
            std::vector<math::Matrix4> matrices, globals;
            ASSERT_EQ(asset::EvaluateBakedBoneAnimation(m_source, first, matrices, globals, &m_target, &second, blend), asset::BoneBakeStatus::OK);
            expected.insert(expected.end(), matrices.begin(), matrices.end());
        }
        Begin(); const auto result = Replay(rows);
        ASSERT_EQ(result.status, renderer::BoneReplayStatus::OK);
        EXPECT_EQ(result.dispatchCount, 1u); EXPECT_EQ(result.rowUploadBytes, 1000u*32u+80u);
        const auto pixels = Read(result.palette, static_cast<uint32_t>(expected.size()));
        ASSERT_EQ(pixels.size(), expected.size()*16);
        /// @note HLSL reads column-major matrices; shader rows are CPU columns. The capture target retains float32 values.
        for (size_t i = 0; i < expected.size(); ++i)
            for (uint32_t row = 0; row < 4; ++row) for (uint32_t col = 0; col < 4; ++col)
                EXPECT_NEAR(pixels[i*16+row*4+col], expected[i].m[col][row], 0.0001f) << "fish=" << i/2 << " lane=" << lane;
    }
}

TEST_F(BakedBoneReplayTest, InvalidOffsetsAndStaleOwnerPublishNoPalette)
{
    Begin(); renderer::BoneReplayRow row;
    row.first = m_gpu->sampleCount;
    auto result = Replay(std::span(&row, 1));
    EXPECT_EQ(result.status, renderer::BoneReplayStatus::INVALID_ROW); EXPECT_FALSE(result.palette.IsValid());
    row.first = 1; result = Replay(std::span(&row,1)); EXPECT_EQ(result.status, renderer::BoneReplayStatus::INVALID_ROW);
    row.first = 0; row.alpha = std::numeric_limits<float>::quiet_NaN();
    result = Replay(std::span(&row,1)); EXPECT_EQ(result.status, renderer::BoneReplayStatus::INVALID_ROW);
    row.alpha = 0; ++m_gpu->owner;
    result = Replay(std::span(&row,1)); EXPECT_EQ(result.status, renderer::BoneReplayStatus::INVALID_SOURCE); EXPECT_FALSE(result.palette.IsValid());
    --m_gpu->owner;
}
}
