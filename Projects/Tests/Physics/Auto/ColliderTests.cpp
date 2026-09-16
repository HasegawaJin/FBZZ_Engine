/// @file    ColliderTests.cpp
/// @brief   各コライダーの体積・境界・Transform 同期の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// ComputeVolume は PhysicsMaterial::density から質量を決める入口なので、
/// 桁が狂うと «軽すぎて吹き飛ぶ / 重すぎて動かない» として現れる。
/// GetAABB は BroadPhase の候補数を決め、小さすぎると当たり判定が抜ける。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/SphereCollider.hpp>

#include <cmath>

namespace fbzz::tests {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// ComputeVolume を override しない形状。基底クラスの既定 (外接箱の体積) を確かめる。
class BareCollider final : public physics::Collider {
public:
    physics::AABB GetAABB() const override { return m_bounds; }
    physics::ColliderType GetType() const override { return physics::ColliderType::CONVEX_HULL; }
    void Update(const math::Vector3&, const math::Quaternion&) override {}

    physics::AABB m_bounds{math::Vector3::ZERO, math::Vector3::ZERO};
};

} // namespace

class ColliderTest : public testkit::Fixture {};

// --- Sphere -----------------------------------------------------------------

TEST_F(ColliderTest, SphereVolumeMatchesTheAnalyticFormula)
{
    physics::SphereCollider sphere(2.0f);

    EXPECT_NEAR(sphere.ComputeVolume(), (4.0f / 3.0f) * kPi * 8.0f, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, SphereBoundsFollowTheWorldPosition)
{
    physics::SphereCollider sphere(1.5f);
    sphere.Update(math::Vector3(10.0f, -4.0f, 2.0f), math::Quaternion::Identity());

    const physics::AABB bounds = sphere.GetAABB();

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(8.5f, -5.5f, 0.5f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(11.5f, -2.5f, 3.5f), testkit::kTolerance);
}

TEST_F(ColliderTest, SphereBoundsIgnoreRotation)
{
    // 球は回しても形が変わらない。回転を反映してしまうと境界が無意味に膨らむ。
    physics::SphereCollider rotated(1.0f);
    physics::SphereCollider upright(1.0f);
    rotated.Update(math::Vector3::ZERO, Rng().NextRotation());
    upright.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_VEC3_NEAR(rotated.GetAABB().min, upright.GetAABB().min, testkit::kTolerance);
    EXPECT_VEC3_NEAR(rotated.GetAABB().max, upright.GetAABB().max, testkit::kTolerance);
}

TEST_F(ColliderTest, SphereReportsItsType)
{
    physics::SphereCollider sphere(1.0f);

    EXPECT_EQ(sphere.GetType(), physics::ColliderType::SPHERE);
}

// --- AABB -------------------------------------------------------------------

TEST_F(ColliderTest, AxisAlignedBoxVolumeIsEightTimesTheHalfExtentProduct)
{
    physics::AABBCollider box(math::Vector3(1.0f, 2.0f, 3.0f));

    EXPECT_NEAR(box.ComputeVolume(), 48.0f, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, AxisAlignedBoxIgnoresRotation)
{
    // 「回らない箱」であることが AABBCollider の存在理由。回ってしまうと
    // 地面や壁がフレームごとに膨らんだり縮んだりする。
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO,
               math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f)));

    const physics::AABB bounds = box.GetAABB();

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(-1.0f, -1.0f, -1.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(1.0f, 1.0f, 1.0f), testkit::kTolerance);
}

// --- OBB --------------------------------------------------------------------

TEST_F(ColliderTest, OrientedBoxVolumeIsInvariantUnderRotation)
{
    physics::OBBCollider box(math::Vector3(1.0f, 2.0f, 0.5f));
    const float          before = box.ComputeVolume();

    box.Update(math::Vector3::ZERO, Rng().NextRotation());

    EXPECT_NEAR(box.ComputeVolume(), before, testkit::kTolerance);
    EXPECT_NEAR(box.ComputeVolume(), 8.0f, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, OrientedBoxBoundsGrowWhenRotatedFortyFiveDegrees)
{
    // 一辺 2 の立方体を Y 回りに 45 度回すと、XZ の外接幅が sqrt(2) 倍になる。
    physics::OBBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO,
               math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f)));

    const physics::AABB bounds  = box.GetAABB();
    const float         kRoot2 = std::sqrt(2.0f);

    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(kRoot2, 1.0f, kRoot2), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(-kRoot2, -1.0f, -kRoot2), testkit::kLooseTolerance);
}

TEST_F(ColliderTest, OrientedBoxAxesAreOrthonormalAfterUpdate)
{
    physics::OBBCollider   box(math::Vector3(1.0f, 1.0f, 1.0f));
    const math::Quaternion rotation = Rng().NextRotation();
    box.Update(math::Vector3::ZERO, rotation);

    for (int i = 0; i < 3; ++i) EXPECT_UNIT_LENGTH(box.GetAxis(i), testkit::kLooseTolerance);

    EXPECT_NEAR(math::Vector3::Dot(box.GetAxis(0), box.GetAxis(1)), 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(box.GetAxis(1), box.GetAxis(2)), 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(box.GetAxis(2), box.GetAxis(0)), 0.0f, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, OrientedBoxCornersAreAllInsideItsBounds)
{
    physics::OBBCollider   box(math::Vector3(1.0f, 2.0f, 0.5f));
    const math::Quaternion rotation = Rng().NextRotation();
    box.Update(math::Vector3(3.0f, -1.0f, 2.0f), rotation);

    const physics::AABB bounds = box.GetAABB();
    for (const math::Vector3& corner : box.GetCorners()) {
        EXPECT_GE(corner.x, bounds.min.x - testkit::kLooseTolerance);
        EXPECT_GE(corner.y, bounds.min.y - testkit::kLooseTolerance);
        EXPECT_GE(corner.z, bounds.min.z - testkit::kLooseTolerance);
        EXPECT_LE(corner.x, bounds.max.x + testkit::kLooseTolerance);
        EXPECT_LE(corner.y, bounds.max.y + testkit::kLooseTolerance);
        EXPECT_LE(corner.z, bounds.max.z + testkit::kLooseTolerance);
    }
}

TEST_F(ColliderTest, OrientedBoxSupportPointIsTheFarthestCorner)
{
    physics::OBBCollider box(math::Vector3(1.0f, 2.0f, 3.0f));
    box.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    // 回転なしなら、支持点は各軸の符号で決まる角そのもの。
    EXPECT_VEC3_NEAR(box.SupportPoint(math::Vector3(1.0f, 1.0f, 1.0f)),
                     math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(box.SupportPoint(math::Vector3(-1.0f, -1.0f, -1.0f)),
                     math::Vector3(-1.0f, -2.0f, -3.0f), testkit::kTolerance);
}

TEST_F(ColliderTest, OrientedBoxSupportPointIsNeverBeatenByAnyCorner)
{
    physics::OBBCollider   box(math::Vector3(1.0f, 2.0f, 0.5f));
    const math::Quaternion rotation = Rng().NextRotation();
    box.Update(math::Vector3(1.0f, 0.0f, -2.0f), rotation);

    for (int i = 0; i < 32; ++i) {
        const math::Vector3 direction = Rng().NextUnitVector3();
        const float         best      = math::Vector3::Dot(box.SupportPoint(direction), direction);

        for (const math::Vector3& corner : box.GetCorners()) {
            EXPECT_LE(math::Vector3::Dot(corner, direction), best + testkit::kLooseTolerance);
        }
    }
}

// --- Capsule ----------------------------------------------------------------

TEST_F(ColliderTest, CapsuleVolumeIsTheCylinderPlusOneWholeSphere)
{
    physics::CapsuleCollider capsule(0.5f, 1.0f);

    const float cylinder    = kPi * 0.25f * 2.0f;
    const float hemispheres = (4.0f / 3.0f) * kPi * 0.125f;

    EXPECT_NEAR(capsule.ComputeVolume(), cylinder + hemispheres, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, CapsuleSegmentRunsAlongTheLocalUpAxis)
{
    physics::CapsuleCollider capsule(0.5f, 2.0f);
    capsule.Update(math::Vector3(1.0f, 5.0f, -3.0f), math::Quaternion::Identity());

    EXPECT_VEC3_NEAR(capsule.GetSegmentStart(), math::Vector3(1.0f, 3.0f, -3.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(capsule.GetSegmentEnd(), math::Vector3(1.0f, 7.0f, -3.0f), testkit::kTolerance);
}

TEST_F(ColliderTest, CapsuleSegmentFollowsRotation)
{
    // X 回りに 90 度回すと、中心線は Y 軸から Z 軸へ倒れる。
    physics::CapsuleCollider capsule(0.5f, 2.0f);
    capsule.Update(math::Vector3::ZERO,
                   math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    EXPECT_VEC3_NEAR(capsule.GetSegmentStart(), math::Vector3(0.0f, 0.0f, -2.0f),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(capsule.GetSegmentEnd(), math::Vector3(0.0f, 0.0f, 2.0f),
                     testkit::kLooseTolerance);
}

TEST_F(ColliderTest, CapsuleBoundsIncludeTheRadiusAroundTheSegment)
{
    physics::CapsuleCollider capsule(0.5f, 2.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::AABB bounds = capsule.GetAABB();

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(-0.5f, -2.5f, -0.5f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(0.5f, 2.5f, 0.5f), testkit::kTolerance);
}

// --- 基底クラスの既定 -------------------------------------------------------

TEST_F(ColliderTest, DefaultVolumeFallsBackToTheBoundingBox)
{
    // 三角メッシュや凸包は厳密な体積を出さず、外接箱で近似する契約。
    BareCollider collider;
    collider.m_bounds = {math::Vector3(-1.0f, -2.0f, -3.0f), math::Vector3(1.0f, 2.0f, 3.0f)};

    EXPECT_NEAR(collider.ComputeVolume(), 48.0f, testkit::kLooseTolerance);
}

TEST_F(ColliderTest, DefaultVolumeIsZeroForDegenerateBounds)
{
    // 厚みのない境界で負や NaN を返すと、質量計算がそのまま壊れる。
    BareCollider collider;
    collider.m_bounds = {math::Vector3::ZERO, math::Vector3(1.0f, 0.0f, 1.0f)};

    EXPECT_NEAR(collider.ComputeVolume(), 0.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
