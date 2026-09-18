/// @file    FrustumTests.cpp
/// @brief   Frustum の平面抽出 (Gribb–Hartmann) と点・球・AABB の内外判定を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// カリングの誤りは 2 通りしか出ない。«見えているものが消える» か «無駄に描く» か。
/// 後者は絵に出ないので、平面の向きが 1 枚裏返っていても誰も気づけない。
#include <TestKit/TestKit.hpp>

#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {
namespace {

constexpr float kNear = 0.1f;
constexpr float kFar  = 100.0f;

/// 原点を +Z 方向に見る、視野角 60 度・アスペクト 1 のカメラ。
math::Matrix4 PerspectiveViewProjection(const math::Vector3& eye)
{
    const math::Matrix4 view = math::Matrix4::LookAt(eye, math::Vector3::ZERO, math::Vector3::UP);
    const math::Matrix4 proj =
        math::Matrix4::Perspective(math::ToRad(60.0f), 1.0f, kNear, kFar);
    return proj * view;
}

} // namespace

class FrustumTest : public testkit::Fixture {
protected:
    const math::Vector3 eye{0.0f, 0.0f, -10.0f};
    math::Frustum       frustum =
        math::Frustum::FromViewProjection(PerspectiveViewProjection(eye));
};

/// @name 平面の性質

TEST_F(FrustumTest, ExtractedPlanesAreNormalized)
{
    /// @note 正規化されていないと SignedDistanceTo が «スケール込みの距離» になり、
    ///       球や AABB の半径との比較が全部狂う。
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i)
        EXPECT_NEAR(frustum.GetPlane(i).normal.Length(), 1.0f, testkit::kTolerance);
}

TEST_F(FrustumTest, EveryPlaneNormalPointsInward)
{
    /// @note 視錐台の «内側» にある点は、6 枚すべてに対して正の側にいる。
    const math::Vector3 inside = eye + math::Vector3::FORWARD * 10.0f;

    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i)
        EXPECT_GT(frustum.GetPlane(i).SignedDistanceTo(inside), 0.0f);
}

/// @name 点

TEST_F(FrustumTest, ContainsAPointInFrontOfTheCamera)
{
    EXPECT_TRUE(frustum.Contains(math::Vector3::ZERO));
}

TEST_F(FrustumTest, RejectsAPointBehindTheCamera)
{
    EXPECT_FALSE(frustum.Contains(eye - math::Vector3::FORWARD));
}

TEST_F(FrustumTest, RejectsAPointBeyondTheFarPlane)
{
    EXPECT_FALSE(frustum.Contains(eye + math::Vector3::FORWARD * (kFar + 1.0f)));
}

TEST_F(FrustumTest, RejectsAPointOutsideTheSideWalls)
{
    /// @note カメラから 10 の距離で、半視野角 30 度の外側 (半幅は tan(30°)*10 ≒ 5.77)。
    EXPECT_TRUE(frustum.Contains({5.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(frustum.Contains({7.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(frustum.Contains({0.0f, 7.0f, 0.0f}));
}

/// @name 球

TEST_F(FrustumTest, IntersectsASphereFullyInside)
{
    EXPECT_TRUE(frustum.IntersectsSphere(math::Vector3::ZERO, 1.0f));
}

TEST_F(FrustumTest, IntersectsASphereWhoseCenterIsOutsideButOverlaps)
{
    /// @note 中心は視野の外。半径で届くなら «交差している» としなければ、
    ///       画面端の大きなオブジェクトが消える。
    EXPECT_TRUE(frustum.IntersectsSphere({7.0f, 0.0f, 0.0f}, 3.0f));
}

TEST_F(FrustumTest, RejectsASphereFullyBehindTheCamera)
{
    EXPECT_FALSE(frustum.IntersectsSphere(eye - math::Vector3::FORWARD * 10.0f, 0.5f));
}

TEST_F(FrustumTest, AcceptsASphereBehindTheCameraThatReachesIntoTheView)
{
    EXPECT_TRUE(frustum.IntersectsSphere(eye - math::Vector3::FORWARD * 10.0f, 20.0f));
}

/// @name AABB

TEST_F(FrustumTest, IntersectsAnAABBFullyInside)
{
    EXPECT_TRUE(frustum.IntersectsAABB(math::Vector3::ZERO, {1.0f, 1.0f, 1.0f}));
}

TEST_F(FrustumTest, RejectsASmallAABBBehindTheCamera)
{
    EXPECT_FALSE(frustum.IntersectsAABB(eye - math::Vector3::FORWARD * 10.0f,
                                        {1.0f, 1.0f, 1.0f}));
}

TEST_F(FrustumTest, AcceptsALargeAABBThatEnclosesTheFrustum)
{
    EXPECT_TRUE(frustum.IntersectsAABB(eye - math::Vector3::FORWARD * 10.0f,
                                       {50.0f, 50.0f, 50.0f}));
}

TEST_F(FrustumTest, ProjectsHalfExtentsOntoTheSlantedSideWalls)
{
    /// @note 側面の法線は斜め。half-extent をそのまま半径として使うと、
    ///       画面の外に出た瞬間に細長い箱が消える。
    const math::Vector3 center{9.0f, 0.0f, 0.0f};

    EXPECT_FALSE(frustum.IntersectsAABB(center, {1.0f, 1.0f, 1.0f}));
    EXPECT_TRUE(frustum.IntersectsAABB(center, {5.0f, 1.0f, 1.0f}));
}

/// @name 正投影

TEST_F(FrustumTest, ExtractsABoxFromAnOrthographicProjection)
{
    /// @note 影のカスケードは正投影で組む。深度 [0,1] の near 平面を取り違えると、
    ///       カメラ手前のキャスターが影を落とさなくなる。
    const math::Frustum ortho = math::Frustum::FromViewProjection(
        math::Matrix4::Orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 10.0f));

    EXPECT_TRUE(ortho.Contains({0.0f, 0.0f, 5.0f}));
    EXPECT_FALSE(ortho.Contains({2.0f, 0.0f, 5.0f}));
    EXPECT_FALSE(ortho.Contains({0.0f, 2.0f, 5.0f}));
    EXPECT_FALSE(ortho.Contains({0.0f, 0.0f, -1.0f}));
    EXPECT_FALSE(ortho.Contains({0.0f, 0.0f, 11.0f}));
}

TEST_F(FrustumTest, OrthographicSideWallsAreAxisAligned)
{
    const math::Frustum ortho = math::Frustum::FromViewProjection(
        math::Matrix4::Orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 10.0f));

    EXPECT_VEC3_NEAR(ortho.GetPlane(0).normal, math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ortho.GetPlane(1).normal, -math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ortho.GetPlane(4).normal, math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ortho.GetPlane(5).normal, -math::Vector3::FORWARD, testkit::kTolerance);
}

} // namespace fbzz::tests
