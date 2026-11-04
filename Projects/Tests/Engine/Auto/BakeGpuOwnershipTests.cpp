/// @file    BakeGpuOwnershipTests.cpp
/// @brief   GPU ベイク資源の単独所有、作成元切替と失敗時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/FluidGpuSolver.hpp>
#include <Engine/Asset/FluidGpuLiquidSolver.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <type_traits>

namespace fbzz::tests {
static_assert(!std::is_copy_constructible_v<asset::FluidGpuSolver>);
static_assert(!std::is_copy_assignable_v<asset::FluidGpuSolver>);
static_assert(!std::is_copy_constructible_v<renderer::OwnedResource<renderer::TextureTag>>);
class BakeGpuOwnershipTest : public testkit::EngineFixture {
protected:
    template<class Solver> void SwitchAndRelease(fluid::FluidKind kind)
    {
        testkit::ResourceTrackingRenderer firstDevice, secondDevice;
        renderer::ResourceManager first(firstDevice), second(secondDevice);
        fluid::FluidRecipe recipe;
        recipe.kind = kind;
        recipe.output.substeps = 1;
        std::string error;
        {
            Solver solver;
            ASSERT_TRUE(solver.Initialize(first, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
            EXPECT_GT(firstDevice.m_constantsAlive, 0u);
            EXPECT_FALSE(solver.StepFrame(secondDevice, second));
            EXPECT_EQ(secondDevice.m_dispatches, 0u);
            ASSERT_TRUE(solver.Initialize(second, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
            EXPECT_EQ(firstDevice.m_texturesAlive, 0u);
            EXPECT_EQ(firstDevice.m_structuredAlive, 0u);
            EXPECT_EQ(firstDevice.m_constantsAlive, 0u);
            EXPECT_GT(secondDevice.m_shadersCreated, 0u);
            solver.BeginTick();
            EXPECT_TRUE(solver.StepFrame(secondDevice, second));
            EXPECT_GT(secondDevice.m_dispatches, 0u);
            /// @note 引数の Manager と独立に、各資源の作成元へ返却する。
            solver.Release(first);
            EXPECT_EQ(secondDevice.m_constantsAlive, 0u);
            ASSERT_TRUE(solver.Initialize(second, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
        }
        EXPECT_EQ(secondDevice.m_constantsAlive, 0u);
        EXPECT_EQ(secondDevice.m_structuredAlive, 0u);
        /// @note Step が借りる白テクスチャは Manager の共有キャッシュに残る。
        second.Reset();
        EXPECT_EQ(secondDevice.m_texturesAlive, 0u);
    }
    template<class Solver> void ResetAndFailure(fluid::FluidKind kind)
    {
        testkit::ResourceTrackingRenderer device;
        renderer::ResourceManager resources(device);
        fluid::FluidRecipe recipe;
        recipe.kind = kind;
        std::string error;
        {
            Solver solver;
            ASSERT_TRUE(solver.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
            resources.Reset();
            const auto unrelated = resources.CreateConstantBuffer(16);
            EXPECT_FALSE(solver.StepFrame(device, resources));
            ASSERT_TRUE(solver.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
            EXPECT_NE(resources.Get(unrelated), nullptr);
            resources.Release(unrelated);
        }
        EXPECT_EQ(device.m_constantsAlive, 0u);
        EXPECT_EQ(device.m_texturesAlive, 0u);
        EXPECT_EQ(device.m_structuredAlive, 0u);
        device.m_failConstantAfter = 1;
        {
            Solver solver;
            EXPECT_FALSE(solver.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error));
            EXPECT_FALSE(solver.IsReady());
            device.m_failConstantAfter = -1;
            ASSERT_TRUE(solver.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
            EXPECT_TRUE(solver.IsReady());
        }
        EXPECT_EQ(device.m_constantsAlive, 0u);
        EXPECT_EQ(device.m_texturesAlive, 0u);
        EXPECT_EQ(device.m_structuredAlive, 0u);
    }
};
TEST_F(BakeGpuOwnershipTest, GasSwitchReturnsOriginalResourcesAndDestructorCleansUp)
{ SwitchAndRelease<asset::FluidGpuSolver>(fluid::FluidKind::Gas); }
TEST_F(BakeGpuOwnershipTest, LiquidSwitchReturnsOriginalResourcesAndDestructorCleansUp)
{ SwitchAndRelease<asset::FluidGpuLiquidSolver>(fluid::FluidKind::Liquid); }
TEST_F(BakeGpuOwnershipTest, GasResetAndFailedInitializationPreserveUnrelatedResources)
{ ResetAndFailure<asset::FluidGpuSolver>(fluid::FluidKind::Gas); }
TEST_F(BakeGpuOwnershipTest, LiquidResetAndFailedInitializationPreserveUnrelatedResources)
{ ResetAndFailure<asset::FluidGpuLiquidSolver>(fluid::FluidKind::Liquid); }

TEST_F(BakeGpuOwnershipTest, MovingOwnedHandleTransfersExactlyOneReleaseResponsibility)
{
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    const auto first = resources.CreateConstantBuffer(16);
    const auto second = resources.CreateConstantBuffer(32);
    {
        renderer::OwnedResource<renderer::ConstantBufferTag> owner(first);
        renderer::OwnedResource<renderer::ConstantBufferTag> replacement(second);
        replacement = std::move(owner);
        EXPECT_FALSE(owner.IsValid());
        EXPECT_EQ(resources.Get(second), nullptr);
        EXPECT_NE(resources.Get(first), nullptr);
        replacement.Reset(first);
        EXPECT_EQ(device.m_constantsAlive, 1u);
    }
    EXPECT_EQ(device.m_constantsAlive, 0u);
}

TEST_F(BakeGpuOwnershipTest, SolverDestructionAfterOwnerDestructionIsSafe)
{
    testkit::ResourceTrackingRenderer device;
    asset::FluidGpuSolver gas;
    asset::FluidGpuLiquidSolver liquid;
    {
        renderer::ResourceManager resources(device);
        fluid::FluidRecipe recipe;
        std::string error;
        ASSERT_TRUE(gas.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
        recipe.kind = fluid::FluidKind::Liquid;
        ASSERT_TRUE(liquid.Initialize(resources, recipe, 16, 1.0f / 24.0f, 1.0f, error)) << error;
    }
    EXPECT_EQ(device.m_constantsAlive, 0u);
    EXPECT_EQ(device.m_texturesAlive, 0u);
    EXPECT_EQ(device.m_structuredAlive, 0u);
}

TEST_F(BakeGpuOwnershipTest, PreviewSwitchResetAndDestructorReturnOwnedResources)
{
    testkit::ResourceTrackingRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    asset::VolumeFlipbookBakeSettings settings;
    settings.volumeResolution = 16;
    settings.tileSize = 32;
    settings.supersampling = 1;
    {
        asset::VolumeFlipbookBaker baker;
        baker.RecordPreview(firstDevice, first, settings, 0.0f);
        const auto initial = baker.PreviewTarget();
        ASSERT_NE(first.Get(initial), nullptr);
        baker.RecordPreview(secondDevice, second, settings, 0.0f);
        EXPECT_EQ(first.Get(initial), nullptr);
        EXPECT_EQ(firstDevice.m_constantsAlive, 0u);
        EXPECT_EQ(firstDevice.m_texturesAlive, 0u);
        EXPECT_EQ(firstDevice.m_structuredAlive, 0u);
        EXPECT_EQ(firstDevice.m_pipelinesAlive, 0u);
        ASSERT_NE(second.Get(baker.PreviewTarget()), nullptr);
        second.Reset();
        const auto unrelated = second.CreateConstantBuffer(16);
        baker.RecordPreview(secondDevice, second, settings, 0.1f);
        EXPECT_NE(second.Get(baker.PreviewTarget()), nullptr);
        EXPECT_NE(second.Get(unrelated), nullptr);
        second.Release(unrelated);
    }
    EXPECT_EQ(secondDevice.m_constantsAlive, 0u);
    EXPECT_EQ(secondDevice.m_texturesAlive, 0u);
    EXPECT_EQ(secondDevice.m_structuredAlive, 0u);
    EXPECT_EQ(secondDevice.m_pipelinesAlive, 0u);
    EXPECT_EQ(secondDevice.m_targetsAlive, 0u);
}

TEST_F(BakeGpuOwnershipTest, PreviewAllocationFailureCanRetryAndReturnsPartiallyCreatedResources)
{
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    asset::VolumeFlipbookBakeSettings settings;
    settings.volumeResolution = 16;
    settings.tileSize = 32;
    settings.supersampling = 1;
    {
        asset::VolumeFlipbookBaker baker;
        device.m_failConstantAfter = 1;
        baker.RecordPreview(device, resources, settings, 0.0f);
        EXPECT_FALSE(baker.PreviewTarget().IsValid());
        EXPECT_EQ(device.m_dispatches, 0u);
        device.m_failConstantAfter = -1;
        baker.RecordPreview(device, resources, settings, 0.0f);
        EXPECT_NE(resources.Get(baker.PreviewTarget()), nullptr);
        EXPECT_GT(device.m_dispatches, 0u);
    }
    EXPECT_EQ(device.m_constantsAlive, 0u);
    EXPECT_EQ(device.m_texturesAlive, 0u);
    EXPECT_EQ(device.m_structuredAlive, 0u);
    EXPECT_EQ(device.m_pipelinesAlive, 0u);
    EXPECT_EQ(device.m_targetsAlive, 0u);
}
} /// @note namespace fbzz::tests
