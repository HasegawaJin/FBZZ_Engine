/// @file    StaticMeshRenderMotionTests.cpp
/// @brief   初回Sceneと連続移動を従来経路へ戻し、停止後の再利用とsnapshotを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include "../Fixtures/StaticMeshRenderReuseFixture.hpp"

namespace fbzz::tests {
TEST_F(StaticMeshRenderReuseTest, KeepsMovingGeometryCurrentAndReentersReuseAfterMotionStops)
{
    auto& owner = Object(m_scene);
    const auto retained = Extract(1);
    owner.transform.worldPosition.x = 1;
    (void)Extract(2);
    owner.transform.worldPosition.x = 2;
    const auto moving = Extract(3);
    EXPECT_EQ(SampleCount("RenderSceneExtractor::MovingGeometry"), 1u);
    EXPECT_FLOAT_EQ(moving.Objects()[0].world.m[0][3], 2);
    EXPECT_FLOAT_EQ(moving.Objects()[0].previousWorld.m[0][3], 1);
    owner.transform.worldPosition.x = 3;
    const auto continued = Extract(4);
    EXPECT_EQ(SampleCount("RenderSceneExtractor::MovingGeometry"), 1u);
    EXPECT_FLOAT_EQ(continued.Objects()[0].world.m[0][3], 3);
    EXPECT_VEC3_NEAR(continued.items[0].boundsCenter, (math::Vector3{4, 0, 0}), testkit::kTolerance);
    const auto stopped = Extract(5);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_FLOAT_EQ(stopped.Objects()[0].world.m[0][3], 3);
    EXPECT_VEC3_NEAR(stopped.items[0].boundsCenter, (math::Vector3{4, 0, 0}), testkit::kTolerance);
    (void)Extract(6);
    EXPECT_EQ(Rebuilds(), 0u);
    EXPECT_FLOAT_EQ(retained.Objects()[0].world.m[0][3], 0);
}

TEST_F(StaticMeshRenderReuseTest, LeavesColdScenesOnTheOriginalPathBeforeCreatingReusableValues)
{
    scene::Scene cold;
    Object(cold);
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        profiler::Profiler::BeginFrame();
        const auto result = scene::ExtractRenderSceneGeometry(cold, frame);
        profiler::Profiler::EndFrame();
        ASSERT_EQ(result.items.size(), 1u);
        EXPECT_VEC3_NEAR(result.items[0].boundsCenter, (math::Vector3{1, 0, 0}), testkit::kTolerance);
        EXPECT_EQ(SampleCount("RenderSceneExtractor::ColdGeometry"), frame == 1 ? 1u : 0u);
        EXPECT_EQ(Rebuilds(), frame == 2 ? 1u : 0u);
    }
}

TEST_F(StaticMeshRenderReuseTest, RechecksVolatileScenesWithinABoundedNumberOfBuildsAfterMotionStops)
{
    std::array<scene::EntityID, 128> entities;
    for (auto& id : entities) id = Object(m_scene).GetID();
    (void)Extract(1);
    for (uint64_t frame = 2; frame <= 4; ++frame) {
        for (auto id : entities) m_scene.GetGameObject(id)->transform.worldPosition.x = static_cast<float>(frame);
        const auto moving = Extract(frame);
        ASSERT_EQ(moving.Objects().size(), entities.size());
        EXPECT_FLOAT_EQ(moving.Objects().back().world.m[0][3], static_cast<float>(frame));
    }
    EXPECT_EQ(SampleCount("RenderSceneExtractor::DenseGeometryFallback"), 1u);
    bool rebuilt = false, reusedAfterRebuild = false;
    for (uint64_t frame = 5; frame < 30; ++frame) {
        const auto stopped = Extract(frame);
        ASSERT_EQ(stopped.Objects().size(), entities.size());
        EXPECT_FLOAT_EQ(stopped.Objects()[0].world.m[0][3], 4);
        EXPECT_FLOAT_EQ(stopped.Objects()[0].previousWorld.m[0][3], 4);
        EXPECT_VEC3_NEAR(stopped.items.back().boundsCenter, (math::Vector3{5, 0, 0}), testkit::kTolerance);
        if (Rebuilds() != 0) rebuilt = true;
        else if (rebuilt && SampleCount("RenderSceneExtractor::DenseGeometryFallback") == 0) reusedAfterRebuild = true;
    }
    EXPECT_TRUE(rebuilt);
    EXPECT_TRUE(reusedAfterRebuild);
}
}
