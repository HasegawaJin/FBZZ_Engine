/// @file    SegmentTests.cpp
/// @brief   有限線分の端点制限と縮退時の距離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <Math/Segment.hpp>

namespace fbzz::tests {
class SegmentTest : public testkit::Fixture {};

TEST_F(SegmentTest, ClosestPointIsClampedToBothEndpoints)
{
    const math::Vector3 from{1, 2, 3}, to{5, 2, 3};
    EXPECT_VEC3_NEAR(math::ClosestPointOnSegment({-2, 9, 3}, from, to), from, 1.0e-6f);
    EXPECT_VEC3_NEAR(math::ClosestPointOnSegment({8, 9, 3}, from, to), to, 1.0e-6f);
    float along = -1.0f;
    EXPECT_NEAR(math::DistanceToSegment({3, 5, 3}, from, to, along), 3.0f, 1.0e-6f);
    EXPECT_NEAR(along, 2.0f, 1.0e-6f);
}

TEST_F(SegmentTest, CoincidentEndpointsHaveZeroAlongDistance)
{
    const math::Vector3 point{0, 3, 4};
    float along = -1.0f;
    EXPECT_NEAR(math::DistanceToSegment(point, math::Vector3::ZERO, math::Vector3::ZERO, along),
                5.0f, 1.0e-6f);
    EXPECT_NEAR(along, 0.0f, 1.0e-6f);
}

TEST_F(SegmentTest, ShortNonzeroSegmentsAreNotComparedToASquaredLengthWithLinearTolerance)
{
    const math::Vector3 to{0.0001f, 0, 0}, expected{0.00005f, 0, 0};
    EXPECT_VEC3_NEAR(math::ClosestPointOnSegment({0.00005f, 1, 0}, math::Vector3::ZERO, to),
                     expected, 1.0e-7f);
}
} // namespace fbzz::tests
