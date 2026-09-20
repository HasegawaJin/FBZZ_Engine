/// @file    RenderVisibilityTests.cpp
/// @brief   Scene を持たない可視性判定の境界条件と除外理由。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/RenderVisibility.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>

namespace fbzz::tests {
namespace {

using renderer::EvaluateVisibility;
using renderer::IsWithinDrawDistance;
using renderer::RenderCullingItem;
using renderer::RenderCullingView;
using renderer::VisibilityResult;

class RenderVisibilityTest : public testkit::Fixture {};

TEST_F(RenderVisibilityTest, UnknownBoundsRemainVisible)
{
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    RenderCullingView view;
    view.frustum = &frustum;
    view.projectionScaleY = 1.0f;
    view.smallObjectScreenHeight = 1.0f;
    for (float radius : { 0.0f, -1.0f })
        EXPECT_EQ(EvaluateVisibility(view, { { 0.0f, 0.0f, 100.0f }, radius, 1.0f }),
                  VisibilityResult::VISIBLE);
}

TEST_F(RenderVisibilityTest, DistanceUsesNearestSpherePointAndKeepsBoundary)
{
    RenderCullingView view;
    view.position = { 0.0f, 0.0f, 5.0f };
    RenderCullingItem item{ { 0.0f, 0.0f, 17.0f }, 2.0f, 10.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
    EXPECT_TRUE(IsWithinDrawDistance(view, item));
    item.center.z = 17.25f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::DISTANCE_CULLED);
    EXPECT_FALSE(IsWithinDrawDistance(view, item));
    item.maxDrawDistance = 0.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
}

TEST_F(RenderVisibilityTest, SphericalAndDepthDistancesUseTheSelectedView)
{
    RenderCullingView view;
    const RenderCullingItem item{ { 12.0f, 0.0f, 3.0f }, 1.0f, 5.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::DISTANCE_CULLED);
    view.distanceSpherical = false;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
    view.forward = { 1.0f, 0.0f, 0.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::DISTANCE_CULLED);
}

TEST_F(RenderVisibilityTest, OrthographicSizeDoesNotShrinkWithDistance)
{
    RenderCullingView view;
    view.projectionScaleY = 2.0f;
    view.smallObjectScreenHeight = 0.25f;
    const RenderCullingItem item{ { 0.0f, 0.0f, 100.0f }, 0.5f, 0.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::SMALL_OBJECT_CULLED);
    view.orthographic = true;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
}

TEST_F(RenderVisibilityTest, ScreenSizeBoundaryAndNonpositiveDepthRemainVisible)
{
    RenderCullingView view;
    view.projectionScaleY = 2.0f;
    view.smallObjectScreenHeight = 0.25f;
    view.distanceSpherical = false;
    RenderCullingItem item{ { 0.0f, 0.0f, 8.0f }, 1.0f, 0.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
    item.center.z = 9.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::SMALL_OBJECT_CULLED);
    item.center.z = 0.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
    item.center.z = -9.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
}

TEST_F(RenderVisibilityTest, FirstRejectionReasonIsStable)
{
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    RenderCullingView view;
    view.frustum = &frustum;
    view.projectionScaleY = 1.0f;
    view.smallObjectScreenHeight = 0.5f;
    RenderCullingItem item{ { 0.0f, 0.0f, 10.0f }, 1.0f, 5.0f };
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::DISTANCE_CULLED);
    item.maxDrawDistance = 0.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::SMALL_OBJECT_CULLED);
    view.smallObjectScreenHeight = 0.0f;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::FRUSTUM_CULLED);
    view.frustum = nullptr;
    EXPECT_EQ(EvaluateVisibility(view, item), VisibilityResult::VISIBLE);
}

TEST_F(RenderVisibilityTest, FrustumUsesSphereIntersection)
{
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    RenderCullingView view;
    view.frustum = &frustum;
    EXPECT_EQ(EvaluateVisibility(view, { { 1.25f, 0.0f, 0.5f }, 0.5f, 0.0f }),
              VisibilityResult::VISIBLE);
    EXPECT_EQ(EvaluateVisibility(view, { { 2.0f, 0.0f, 0.5f }, 0.5f, 0.0f }),
              VisibilityResult::FRUSTUM_CULLED);
}

TEST_F(RenderVisibilityTest, DistanceOnlyQueryIgnoresSizeAndFrustum)
{
    const auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    RenderCullingView view;
    view.frustum = &frustum;
    view.projectionScaleY = 1.0f;
    view.smallObjectScreenHeight = 1.0f;
    const RenderCullingItem item{ { 0.0f, 0.0f, 10.0f }, 0.0f, 5.0f };
    EXPECT_FALSE(IsWithinDrawDistance(view, item));
    EXPECT_TRUE(IsWithinDrawDistance(view, { item.center, 1.0f, 0.0f }));
}

} /// @note namespace
} /// @note namespace fbzz::tests
