/// @file    RenderEffectExtractionTests.cpp
/// @brief   デカール・ビーム・カスタムポスト処理の描画入力を抽出境界で検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderCustomPostExtractor.hpp>
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderTrailExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

namespace fbzz::tests {
namespace {

class RenderEffectExtractionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousTime = Time::time;
        m_previousDeltaTime = Time::deltaTime;
        Time::time = 1.0f;
        Time::deltaTime = 0.25f;
    }

    void TearDown() override
    {
        Time::time = m_previousTime;
        Time::deltaTime = m_previousDeltaTime;
        EngineFixture::TearDown();
    }

    renderer::RenderScene Extract(void (*extractor)(scene::RenderPassContext&, renderer::RenderScene&))
    {
        renderer::Camera camera;
        scene::RenderPassContext context(m_scene, m_device, m_resources, camera, m_settings, {}, UINT32_MAX, m_handles);
        renderer::RenderScene output;
        extractor(context, output);
        return output;
    }

    testkit::ResourceTrackingRenderer m_device;
    renderer::ResourceManager m_resources{m_device};
    scene::Scene m_scene;
    renderer::RenderSettings m_settings;
    renderer::RenderPassHandles m_handles;
    float m_previousTime = 0.0f;
    float m_previousDeltaTime = 0.0f;
};

/// @brief 同じフレームでは全ビューが同じデカールのコマとフェード値を受け取る。
TEST_F(RenderEffectExtractionTest, DecalAgeAndAtlasFrameAdvanceOncePerFrame)
{
    auto& decal = m_scene.CreateGameObject("Decal").AddComponent<scene::DecalComponent>();
    decal.lifetime = 0.5f;
    decal.fadeTime = 0.25f;
    decal.opacity = 0.5f;
    decal.albedoColor[3] = 0.5f;
    decal.frameCount = 4;
    decal.framesPerRow = 2;
    decal.frameRate = 4.0f;

    const auto firstView = Extract(scene::ExtractRenderDecals);
    const auto secondView = Extract(scene::ExtractRenderDecals);
    ASSERT_EQ(firstView.decals.size(), 1u);
    ASSERT_EQ(secondView.decals.size(), 1u);
    EXPECT_FLOAT_EQ(firstView.decals[0].projection.alpha, 0.25f);
    EXPECT_FLOAT_EQ(firstView.decals[0].projection.frameIndex, 1.0f);
    EXPECT_FLOAT_EQ(secondView.decals[0].projection.alpha, firstView.decals[0].projection.alpha);
    EXPECT_FLOAT_EQ(secondView.decals[0].projection.frameIndex, firstView.decals[0].projection.frameIndex);

    m_resources.AdvanceFrame();
    EXPECT_TRUE(Extract(scene::ExtractRenderDecals).decals.empty());
}

/// @brief 同一フレームの別ビューはビームの点列を再採取しない。
TEST_F(RenderEffectExtractionTest, BeamSamplesWorldPositionsOncePerFrame)
{
    auto& owner = m_scene.CreateGameObject("Beam");
    owner.transform.worldPosition = {2.0f, 0.0f, 0.0f};
    owner.transform.worldScale = {2.0f, 1.0f, 1.0f};
    auto& trail = owner.AddComponent<scene::TrailComponent>();
    trail.beamMode = true;
    trail.beamPoints = {{1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};

    const auto firstView = Extract(scene::ExtractRenderTrails);
    trail.beamPoints[1].x = 3.0f;
    const auto secondView = Extract(scene::ExtractRenderTrails);
    m_resources.AdvanceFrame();
    const auto nextFrame = Extract(scene::ExtractRenderTrails);

    ASSERT_EQ(firstView.trails.size(), 1u);
    ASSERT_EQ(secondView.trails.size(), 1u);
    ASSERT_EQ(nextFrame.trails.size(), 1u);
    ASSERT_EQ(firstView.trails[0].points.size(), 2u);
    ASSERT_EQ(secondView.trails[0].points.size(), 2u);
    ASSERT_EQ(nextFrame.trails[0].points.size(), 2u);
    EXPECT_VEC3_NEAR(firstView.trails[0].points.front().position, (math::Vector3{4.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(secondView.trails[0].points.back().position, (math::Vector3{6.0f, 0.0f, 0.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(nextFrame.trails[0].points.back().position, (math::Vector3{8.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

/// @brief カスタム効果を無効にしても、後続のシェーダーと合成設定は同じスロットに残る。
TEST_F(RenderEffectExtractionTest, CustomPostKeepsEnabledEffectsAtTheirConfiguredSlots)
{
    const auto skipped = m_resources.LoadShader("SkippedPost.hlsl");
    const auto fallback = m_resources.LoadShader("FallbackPost.hlsl");
    ASSERT_TRUE(skipped.IsValid());
    ASSERT_TRUE(fallback.IsValid());
    m_handles.customPostProcessShaders = {skipped, fallback};
    auto& effects = m_settings.postProcess.customEffects;
    effects.resize(2);
    effects[1].enabled = true;
    effects[1].blendMode = renderer::BlendMode::ALPHA_BLEND;

    const auto output = Extract(scene::ExtractRenderCustomPost);
    ASSERT_EQ(output.customPost.size(), 2u);
    EXPECT_FALSE(output.customPost[0].IsValid());
    EXPECT_EQ(output.customPost[1].shader, fallback);
    EXPECT_EQ(output.customPost[1].blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_FALSE(output.customPost[1].paramsBuffer.IsValid());
}

} /// @note namespace
} /// @note namespace fbzz::tests
