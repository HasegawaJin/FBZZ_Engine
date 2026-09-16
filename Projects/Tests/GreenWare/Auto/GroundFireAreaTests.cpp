/// @file    GroundFireAreaTests.cpp
/// @brief   残り火の高速横断・高さ制限・端の交差を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <Scripts/Utils/GroundFireArea.hpp>

namespace fbzz::tests {
class GroundFireAreaTest : public testkit::Fixture {};

TEST_F(GroundFireAreaTest, FastCrossingHitsEvenWhenBothEndpointsAreOutside)
{
    EXPECT_TRUE(sandbox::GroundFireArea::Intersects({-4, 0, 0}, {4, 0, 0}, {}, 1.6f, 1.4f));
    EXPECT_FALSE(sandbox::GroundFireArea::Intersects({-4, 0, 2}, {4, 0, 2}, {}, 1.6f, 1.4f));
}

TEST_F(GroundFireAreaTest, JumpAboveFlameAndMovementBelowGroundAreSafe)
{
    EXPECT_FALSE(sandbox::GroundFireArea::Intersects({-4, 2, 0}, {4, 2, 0}, {}, 1.6f, 1.4f));
    EXPECT_FALSE(sandbox::GroundFireArea::Intersects({0, -2, 0}, {0, -1, 0}, {}, 1.6f, 1.4f));
    EXPECT_TRUE(sandbox::GroundFireArea::Intersects({0, 3, 0}, {0, 0, 0}, {}, 1.6f, 1.4f));
}

TEST_F(GroundFireAreaTest, HeightAndHorizontalCrossingMustOverlapInTime)
{
    EXPECT_FALSE(sandbox::GroundFireArea::Intersects({0, 3, 0}, {6, 0, 0}, {}, 1.6f, 1.4f));
    EXPECT_TRUE(sandbox::GroundFireArea::Intersects({0, 0, 0}, {6, 3, 0}, {}, 1.6f, 1.4f));
}

TEST_F(GroundFireAreaTest, StationaryFeetAndWorldOffsetUseTheSameFootprint)
{
    EXPECT_TRUE(sandbox::GroundFireArea::Intersects({11, 4, 7}, {11, 4, 7}, {10, 4, 7}, 1.6f, 1.4f));
    EXPECT_FALSE(sandbox::GroundFireArea::Intersects({12, 4, 7}, {12, 4, 7}, {10, 4, 7}, 1.6f, 1.4f));
}
} // namespace fbzz::tests
