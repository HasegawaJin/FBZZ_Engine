/// @file    RayTests.cpp
/// @brief   Ray の平面・球・三角形との交差と、NDC からのレイ生成の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// エディターのピッキングと当たり判定のレイキャストが全部ここを通る。
/// 交差点 t が «少しずれる» 壊れ方はマウスの選択が微妙に外れるだけなので、
/// 手で触っている限り «そういうもの» として見過ごされる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Plane.hpp>
#include <Math/Ray.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::tests {
namespace {

/// 原点を見下ろす標準的なカメラの逆ビュープロジェクション。
math::Matrix4 InverseViewProjection(const math::Vector3& eye)
{
    const math::Matrix4 view = math::Matrix4::LookAt(eye, math::Vector3::ZERO, math::Vector3::UP);
    const math::Matrix4 proj = math::Matrix4::Perspective(math::ToRad(60.0f), 1.0f, 0.1f, 100.0f);
    return math::Matrix4::Inverse(proj * view);
}

} // namespace

class RayTest : public testkit::Fixture {};

// --- パラメータ位置 ---------------------------------------------------------

TEST_F(RayTest, AtReturnsTheOriginForZero)
{
    const math::Ray ray({1.0f, 2.0f, 3.0f}, math::Vector3::FORWARD);

    EXPECT_VEC3_NEAR(ray.At(0.0f), ray.origin, testkit::kTolerance);
}

TEST_F(RayTest, AtAdvancesByTAlongTheDirection)
{
    const math::Ray ray({0.0f, 1.0f, 0.0f}, math::Vector3::FORWARD);

    EXPECT_VEC3_NEAR(ray.At(4.0f), math::Vector3(0.0f, 1.0f, 4.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(ray.At(-4.0f), math::Vector3(0.0f, 1.0f, -4.0f), testkit::kTolerance);
}

// --- 平面 -------------------------------------------------------------------

TEST_F(RayTest, IntersectPlaneReturnsTheDistanceToTheHitPoint)
{
    const math::Ray ray({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    const math::Plane ground(math::Vector3::UP, 0.0f);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectPlane(ground, t));

    EXPECT_NEAR(t, 5.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ray.At(t), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(RayTest, IntersectPlaneMissesWhenParallel)
{
    const math::Ray ray({0.0f, 5.0f, 0.0f}, math::Vector3::FORWARD);
    const math::Plane ground(math::Vector3::UP, 0.0f);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectPlane(ground, t));
}

TEST_F(RayTest, IntersectPlaneMissesWhenThePlaneIsBehindTheRay)
{
    // 後方の交点は «当たっていない» とする。ピッキングでカメラの背後を拾わないため。
    const math::Ray ray({0.0f, 5.0f, 0.0f}, math::Vector3::UP);
    const math::Plane ground(math::Vector3::UP, 0.0f);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectPlane(ground, t));
}

TEST_F(RayTest, IntersectPlaneHitsFromTheBackSide)
{
    // 法線の裏から入っても交差は成立する。片面判定はレイの契約ではない。
    const math::Ray ray({0.0f, -5.0f, 0.0f}, math::Vector3::UP);
    const math::Plane ground(math::Vector3::UP, 0.0f);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectPlane(ground, t));

    EXPECT_NEAR(t, 5.0f, testkit::kTolerance);
}

// --- 球 ---------------------------------------------------------------------

TEST_F(RayTest, IntersectSphereReturnsTheNearHitFromOutside)
{
    const math::Ray ray({0.0f, 0.0f, -5.0f}, math::Vector3::FORWARD);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectSphere(math::Vector3::ZERO, 1.0f, t));

    EXPECT_NEAR(t, 4.0f, testkit::kTolerance);
}

TEST_F(RayTest, IntersectSphereReturnsTheExitHitFromInside)
{
    // 手前の交点が背後にあるときは奥の交点を返す。カメラが球の内側にいる場合。
    const math::Ray ray(math::Vector3::ZERO, math::Vector3::FORWARD);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectSphere(math::Vector3::ZERO, 2.0f, t));

    EXPECT_NEAR(t, 2.0f, testkit::kTolerance);
}

TEST_F(RayTest, IntersectSphereMissesWhenTheRayPassesBeside)
{
    const math::Ray ray({0.0f, 10.0f, -5.0f}, math::Vector3::FORWARD);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectSphere(math::Vector3::ZERO, 1.0f, t));
}

TEST_F(RayTest, IntersectSphereMissesWhenTheSphereIsBehind)
{
    const math::Ray ray({0.0f, 0.0f, 5.0f}, math::Vector3::FORWARD);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectSphere(math::Vector3::ZERO, 1.0f, t));
}

TEST_F(RayTest, IntersectSphereHitPointLiesOnTheSurface)
{
    const math::Vector3 center(1.0f, 2.0f, 3.0f);
    const math::Ray ray({1.0f, 2.0f, -7.0f}, math::Vector3::FORWARD);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectSphere(center, 2.5f, t));

    EXPECT_NEAR((ray.At(t) - center).Length(), 2.5f, testkit::kTolerance);
}

// --- 三角形 -----------------------------------------------------------------

TEST_F(RayTest, IntersectTriangleReturnsTheDistanceToTheFace)
{
    const math::Ray ray(math::Vector3::ZERO, math::Vector3::FORWARD);
    float t = 0.0f;

    ASSERT_TRUE(ray.IntersectTriangle({-1.0f, -1.0f, 5.0f},
                                      {1.0f, -1.0f, 5.0f},
                                      {0.0f, 1.0f, 5.0f}, t));

    EXPECT_NEAR(t, 5.0f, testkit::kTolerance);
}

TEST_F(RayTest, IntersectTriangleMissesOutsideTheEdges)
{
    const math::Ray ray({0.0f, 0.9f, 0.0f}, math::Vector3::FORWARD);   // 頂点の外側を通る
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectTriangle({-1.0f, -1.0f, 5.0f},
                                       {1.0f, -1.0f, 5.0f},
                                       {0.0f, -0.5f, 5.0f}, t));
}

TEST_F(RayTest, IntersectTriangleHitsRegardlessOfWinding)
{
    // 裏面カリングはレンダラーの仕事。レイ側で落とすと «裏から選べない» になる。
    const math::Ray ray(math::Vector3::ZERO, math::Vector3::FORWARD);
    float front = 0.0f;
    float back  = 0.0f;

    ASSERT_TRUE(ray.IntersectTriangle({-1.0f, -1.0f, 5.0f}, {1.0f, -1.0f, 5.0f},
                                      {0.0f, 1.0f, 5.0f}, front));
    ASSERT_TRUE(ray.IntersectTriangle({-1.0f, -1.0f, 5.0f}, {0.0f, 1.0f, 5.0f},
                                      {1.0f, -1.0f, 5.0f}, back));

    EXPECT_NEAR(front, back, testkit::kTolerance);
}

TEST_F(RayTest, IntersectTriangleMissesWhenTheFaceIsBehind)
{
    const math::Ray ray(math::Vector3::ZERO, math::Vector3::FORWARD);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectTriangle({-1.0f, -1.0f, -5.0f},
                                       {1.0f, -1.0f, -5.0f},
                                       {0.0f, 1.0f, -5.0f}, t));
}

TEST_F(RayTest, IntersectTriangleMissesWhenTheRayLiesInTheFacePlane)
{
    // 縮退 (det ≒ 0)。0 除算で NaN を返さないことを固定する。
    const math::Ray ray({0.0f, 0.0f, 5.0f}, math::Vector3::RIGHT);
    float t = 0.0f;

    EXPECT_FALSE(ray.IntersectTriangle({-1.0f, -1.0f, 5.0f},
                                       {1.0f, -1.0f, 5.0f},
                                       {0.0f, 1.0f, 5.0f}, t));
}

// --- NDC からの生成 ---------------------------------------------------------

TEST_F(RayTest, FromNDCStartsAtTheCameraPosition)
{
    const math::Vector3 eye(0.0f, 0.0f, -10.0f);

    const math::Ray ray = math::Ray::FromNDC(0.0f, 0.0f, eye, InverseViewProjection(eye));

    EXPECT_VEC3_NEAR(ray.origin, eye, testkit::kTolerance);
}

TEST_F(RayTest, FromNDCProducesAUnitDirection)
{
    const math::Vector3 eye(0.0f, 0.0f, -10.0f);

    const math::Ray ray = math::Ray::FromNDC(0.5f, -0.5f, eye, InverseViewProjection(eye));

    EXPECT_UNIT_LENGTH(ray.direction, testkit::kTolerance);
}

TEST_F(RayTest, FromNDCAimsAlongTheViewAxisAtTheScreenCenter)
{
    const math::Vector3 eye(0.0f, 0.0f, -10.0f);

    const math::Ray ray = math::Ray::FromNDC(0.0f, 0.0f, eye, InverseViewProjection(eye));

    EXPECT_VEC3_NEAR(ray.direction, math::Vector3::FORWARD, testkit::kLooseTolerance);
}

TEST_F(RayTest, FromNDCMapsPositiveXToTheRightAndPositiveYToUp)
{
    // NDC の上下反転は «掴んだ場所と選択がずれる» という形でしか出ない。
    const math::Vector3 eye(0.0f, 0.0f, -10.0f);
    const math::Matrix4 invVP = InverseViewProjection(eye);

    const math::Ray right = math::Ray::FromNDC(0.5f, 0.0f, eye, invVP);
    const math::Ray up    = math::Ray::FromNDC(0.0f, 0.5f, eye, invVP);

    EXPECT_GT(right.direction.x, 0.0f);
    EXPECT_GT(up.direction.y, 0.0f);
}

TEST_F(RayTest, FromNDCHitsTheGroundWhereTheCameraLooks)
{
    // 画面中央のレイは、カメラが見下ろしている点で地面に当たる。
    const math::Vector3 eye(0.0f, 10.0f, -10.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(eye, math::Vector3::ZERO, math::Vector3::UP);
    const math::Matrix4 proj =
        math::Matrix4::Perspective(math::ToRad(60.0f), 1.0f, 0.1f, 100.0f);

    const math::Ray ray =
        math::Ray::FromNDC(0.0f, 0.0f, eye, math::Matrix4::Inverse(proj * view));
    float t = 0.0f;
    ASSERT_TRUE(ray.IntersectPlane(math::Plane(math::Vector3::UP, 0.0f), t));

    EXPECT_VEC3_NEAR(ray.At(t), math::Vector3::ZERO, testkit::kLooseTolerance);
}

} // namespace fbzz::tests
