/// @file    RayPathHistoryTests.cpp
/// @brief   Path Progressive の実内容キー・成功サンプル進行・リセット条件を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayPathHistory.hpp>
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <cmath>
#include <limits>

namespace fbzz::tests {
namespace {

class RayPathHistoryTest : public testkit::Fixture {
protected:
    renderer::RayPathHistory m_history;
    renderer::RayPathScene m_scene;
    renderer::Camera m_camera;
    renderer::RayPathIntegratorSettings m_integrator;

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_scene.sceneGeneration = 7;
        m_scene.contentRevision = 1;
    }
    renderer::RayPathHistoryKey Key(uint32_t width = 1280, uint32_t height = 720,
        uint64_t deviceEpoch = 1, uint64_t shaderEpoch = 1)
    {
        return renderer::MakeRayPathHistoryKey(m_scene, m_camera, width, height, m_integrator, deviceEpoch, shaderEpoch);
    }
    void ExpectReset(const renderer::RayPathHistoryKey& key)
    {
        ASSERT_TRUE(m_history.Commit(4));
        EXPECT_TRUE(m_history.Prepare(key));
        EXPECT_EQ(m_history.GetSampleCount(), 0u);
        EXPECT_FALSE(m_history.Prepare(key));
    }
};

TEST_F(RayPathHistoryTest, AdvancesOnlyCommittedSamplesAndPreservesUnchangedHistory)
{
    EXPECT_FALSE(m_history.Commit(1));
    EXPECT_TRUE(m_history.Prepare(Key()));
    EXPECT_TRUE(m_history.Commit(2));
    EXPECT_FALSE(m_history.Prepare(Key()));
    EXPECT_EQ(m_history.GetSampleCount(), 2u);
    EXPECT_TRUE(m_history.Commit(3));
    EXPECT_EQ(m_history.GetSampleCount(), 5u);
    EXPECT_FALSE(m_history.Commit(0));
    EXPECT_EQ(m_history.GetSampleCount(), 5u);
    m_history.Reset();
    EXPECT_FALSE(m_history.IsPrepared());
    EXPECT_EQ(m_history.GetSampleCount(), 0u);
    EXPECT_FALSE(m_history.Commit(1));
}

TEST_F(RayPathHistoryTest, ResetsForSceneDeviceShaderAndInternalExtent)
{
    ASSERT_TRUE(m_history.Prepare(Key()));
    ++m_scene.contentRevision; ExpectReset(Key());
    ++m_scene.sceneGeneration; ExpectReset(Key());
    ExpectReset(Key(640, 720));
    ExpectReset(Key(640, 360));
    ExpectReset(Key(640, 360, 2));
    ExpectReset(Key(640, 360, 2, 2));
}

TEST_F(RayPathHistoryTest, UsesExactUnjitteredCameraAndProjectionInputs)
{
    ASSERT_TRUE(m_history.Prepare(Key()));
    m_camera.m_position.x = std::nextafter(0.0f, 1.0f); ExpectReset(Key());
    m_camera.m_rotation.y = std::nextafter(0.0f, 1.0f); ExpectReset(Key());
    m_camera.m_fovY = 45; ExpectReset(Key());
    m_camera.m_aspect = 1; ExpectReset(Key());
    m_camera.m_near = 0.2f; ExpectReset(Key());
    m_camera.m_far = 200; ExpectReset(Key());
    m_camera.m_projection = renderer::ProjectionMode::Orthographic; ExpectReset(Key());
    m_camera.m_orthoHeight = 15; ExpectReset(Key());
}

TEST_F(RayPathHistoryTest, ResetsForIntegratorAndSamplerMeaningButNotPostColorControls)
{
    ASSERT_TRUE(m_history.Prepare(Key()));
    ASSERT_TRUE(m_history.Commit(8));
    m_camera.m_backgroundColor = {5, 1, 2, 1};
    m_camera.m_clearMode = renderer::CameraClearMode::DepthOnly;
    EXPECT_FALSE(m_history.Prepare(Key()));
    EXPECT_EQ(m_history.GetSampleCount(), 8u);
    ++m_integrator.maxBounces; ExpectReset(Key());
    ++m_integrator.rouletteStartBounce; ExpectReset(Key());
    ++m_integrator.seed; ExpectReset(Key());
    ++m_integrator.samplerVersion; ExpectReset(Key());
    ++m_integrator.integratorVersion; ExpectReset(Key());
    m_integrator.maxDistance = std::nextafter(m_integrator.maxDistance, 2000.0f); ExpectReset(Key());
    m_integrator.radianceClamp = 10; ExpectReset(Key());
}

TEST_F(RayPathHistoryTest, RejectsSampleCounterOverflowWithoutChangingTheAverageDenominator)
{
    ASSERT_TRUE(m_history.Prepare(Key()));
    EXPECT_TRUE(m_history.Commit(std::numeric_limits<uint32_t>::max() - 2));
    EXPECT_FALSE(m_history.Commit(3));
    EXPECT_EQ(m_history.GetSampleCount(), std::numeric_limits<uint32_t>::max() - 2);
    EXPECT_TRUE(m_history.Commit(1));
    EXPECT_FALSE(m_history.Commit(1));
    EXPECT_EQ(m_history.GetSampleCount(), std::numeric_limits<uint32_t>::max() - 1);
}

} /// @note namespace
} /// @note namespace fbzz::tests
