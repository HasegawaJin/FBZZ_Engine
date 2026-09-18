/// @file    PlaneTests.cpp
/// @brief   Plane の符号付き距離・正規化・3 点からの生成が dot(n,p)+d=0 の規約どおりであることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// distance の符号規約 (+d か -d か) を取り違えると、視錐台カリングが «裏返って»
/// 見えるものだけ消える。落ちないので、絵を見るまで気づけない。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Plane.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {

class PlaneTest : public testkit::Fixture {};

/// @name 符号付き距離

TEST_F(PlaneTest, SignedDistanceIsPositiveOnTheNormalSide)
{
    const math::Plane ground(math::Vector3::UP, 0.0f);

    EXPECT_NEAR(ground.SignedDistanceTo({0.0f, 5.0f, 0.0f}), 5.0f, testkit::kTolerance);
    EXPECT_NEAR(ground.SignedDistanceTo({0.0f, -5.0f, 0.0f}), -5.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, SignedDistanceIsZeroOnThePlane)
{
    const math::Plane ground(math::Vector3::UP, 0.0f);

    EXPECT_NEAR(ground.SignedDistanceTo({100.0f, 0.0f, -100.0f}), 0.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, DistanceOffsetsThePlaneAgainstTheNormal)
{
    /// @note dot(n,p) + d = 0 なので、d = -3 の平面は法線方向へ +3 だけ動く。
    const math::Plane raised(math::Vector3::UP, -3.0f);

    EXPECT_NEAR(raised.SignedDistanceTo({0.0f, 3.0f, 0.0f}), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(raised.SignedDistanceTo({0.0f, 0.0f, 0.0f}), -3.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, IsOnPositiveSideIncludesThePlaneItself)
{
    const math::Plane ground(math::Vector3::UP, 0.0f);

    EXPECT_TRUE(ground.IsOnPositiveSide({0.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(ground.IsOnPositiveSide({0.0f, 0.1f, 0.0f}));
    EXPECT_FALSE(ground.IsOnPositiveSide({0.0f, -0.1f, 0.0f}));
}

/// @name 正規化

TEST_F(PlaneTest, NormalizedScalesTheDistanceWithTheNormal)
{
    /// @note 法線だけ割って distance を放置すると、平面が原点方向へずれる。
    const math::Plane scaled({0.0f, 4.0f, 0.0f}, -8.0f);

    const math::Plane normalized = scaled.Normalized();

    EXPECT_VEC3_NEAR(normalized.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(normalized.distance, -2.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, NormalizedPreservesTheSetOfPointsOnThePlane)
{
    const math::Plane scaled({0.0f, 4.0f, 0.0f}, -8.0f);

    const math::Plane normalized = scaled.Normalized();

    EXPECT_NEAR(normalized.SignedDistanceTo({0.0f, 2.0f, 0.0f}), 0.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, NormalizedLeavesADegeneratePlaneUntouched)
{
    /// @note 長さ 0 の法線を割ると NaN になる。呼び出し側で分岐させないため、そのまま返す契約。
    const math::Plane degenerate(math::Vector3::ZERO, 7.0f);

    const math::Plane normalized = degenerate.Normalized();

    EXPECT_VEC3_NEAR(normalized.normal, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_NEAR(normalized.distance, 7.0f, testkit::kTolerance);
}

/// @name 生成

TEST_F(PlaneTest, FromPointsUsesTheCrossProductOfTheTwoEdges)
{
    /// @note p0→p1 と p0→p2 の外積が法線。頂点の順が逆なら平面も裏返る。
    const math::Plane plane = math::Plane::FromPoints({0.0f, 0.0f, 0.0f},
                                                      {0.0f, 0.0f, 1.0f},
                                                      {1.0f, 0.0f, 0.0f});

    EXPECT_VEC3_NEAR(plane.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(plane.distance, 0.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, FromPointsFlipsTheNormalWhenTheWindingIsReversed)
{
    const math::Plane front = math::Plane::FromPoints({0.0f, 0.0f, 0.0f},
                                                       {0.0f, 0.0f, 1.0f},
                                                       {1.0f, 0.0f, 0.0f});
    const math::Plane back = math::Plane::FromPoints({0.0f, 0.0f, 0.0f},
                                                      {1.0f, 0.0f, 0.0f},
                                                      {0.0f, 0.0f, 1.0f});

    EXPECT_VEC3_NEAR(back.normal, -front.normal, testkit::kTolerance);
}

TEST_F(PlaneTest, FromPointsPutsEveryInputPointOnThePlane)
{
    const math::Vector3 p0(1.0f, 2.0f, 3.0f);
    const math::Vector3 p1(4.0f, 0.0f, -1.0f);
    const math::Vector3 p2(-2.0f, 5.0f, 2.0f);

    const math::Plane plane = math::Plane::FromPoints(p0, p1, p2);

    EXPECT_NEAR(plane.SignedDistanceTo(p0), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(plane.SignedDistanceTo(p1), 0.0f, testkit::kTolerance);
    EXPECT_NEAR(plane.SignedDistanceTo(p2), 0.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, FromNormalAndPointNormalizesTheGivenNormal)
{
    const math::Plane plane = math::Plane::FromNormalAndPoint({0.0f, 10.0f, 0.0f},
                                                              {0.0f, 2.0f, 0.0f});

    EXPECT_VEC3_NEAR(plane.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(plane.distance, -2.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, FromNormalAndPointPutsThePointOnThePlane)
{
    const math::Vector3 point(3.0f, -4.0f, 5.0f);
    const math::Vector3 normal = Rng().NextUnitVector3();

    const math::Plane plane = math::Plane::FromNormalAndPoint(normal, point);

    EXPECT_NEAR(plane.SignedDistanceTo(point), 0.0f, testkit::kTolerance);
}

TEST_F(PlaneTest, DefaultPlaneIsTheGroundAtTheOrigin)
{
    const math::Plane plane;

    EXPECT_VEC3_NEAR(plane.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(plane.distance, 0.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
