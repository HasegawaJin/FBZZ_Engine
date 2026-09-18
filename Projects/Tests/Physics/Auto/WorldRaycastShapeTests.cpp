/// @file    WorldRaycastShapeTests.cpp
/// @brief   World の空間クエリが、球以外のコライダー形状それぞれで正しく交差を返すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// WorldQueryTests は «最も近い 1 件を返す» «フィルタで除外する» という検索そのものの契約を見る。
/// ここで見るのはその 1 段下 — 形状ごとの交差計算。ここが形状ごとに壊れると、球では拾えるのに
/// 箱やカプセルだけ «すり抜ける» という、形状を差し替えるまで気づけない不具合になる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/World.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace fbzz::tests {
namespace {

physics::ColliderInstance Instance(physics::Collider& collider)
{
    physics::ColliderInstance instance;
    instance.collider = &collider;
    return instance;
}

/// y = 0 に広がる 1x1 の板 (2 三角形)。斜辺は (0,0,1)-(1,0,0) を結ぶので、
/// レイは辺に乗らない位置へ落とすこと。
std::vector<math::Vector3> QuadPositions()
{
    return { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
             { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 1.0f } };
}

std::vector<std::uint32_t> QuadIndices() { return { 0, 2, 1, 1, 2, 3 }; }

std::vector<math::Vector3> CubePoints(float half)
{
    return {
        { -half, -half, -half }, {  half, -half, -half },
        { -half,  half, -half }, {  half,  half, -half },
        { -half, -half,  half }, {  half, -half,  half },
        { -half,  half,  half }, {  half,  half,  half },
    };
}

const math::Vector3 kDown{ 0.0f, -1.0f, 0.0f };

} // namespace

class WorldRaycastShapeTest : public testkit::Fixture {
protected:
    void Register(std::initializer_list<physics::ColliderInstance> colliders)
    {
        world.BeginSceneSync();
        for (const physics::ColliderInstance& instance : colliders)
            world.SyncCollider(physics::ColliderHandle{}, instance);
        world.EndSceneSync();
    }

    physics::World             world;
    physics::World::RaycastHit hit;
};

/// @name AABB

TEST_F(WorldRaycastShapeTest, RaycastHitsTheNearFaceOfABox)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3(0.0f, 0.0f, 5.0f), math::Quaternion::Identity());
    Register({ Instance(box) });

    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 4.0f, testkit::kLooseTolerance);
    /// @note 法線は «レイが入ってきた面の外向き»。裏返ると、跳ね返りも接地判定も逆を向く。
    EXPECT_VEC3_NEAR(hit.normal, -math::Vector3::FORWARD, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastMissesABoxWhenTravellingAlongsideAnAxis)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(box) });

    /// @note 軸に平行なレイでは、その軸のスラブに «入る時刻» が計算できない。
    ///       0 除算を避けたつもりで «常に当たる» 側へ倒すと、箱の真横を通る弾が全部当たる。
    EXPECT_FALSE(world.Raycast(math::Vector3(0.0f, 5.0f, 0.0f),
                               math::Vector3::RIGHT, 100.0f, hit));
}

TEST_F(WorldRaycastShapeTest, RaycastReportsZeroDistanceFromInsideABox)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(box) });

    /// @note めり込んだ状態から撃つのはよくある (足元判定・脱出方向探し)。当たり無しにすると
    ///       «埋まっているときだけ何も検出できない» という一番困る挙動になる。
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::RIGHT, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 0.0f, testkit::kLooseTolerance);
}

/// @name OBB

TEST_F(WorldRaycastShapeTest, RaycastFollowsTheRotationOfAnOBB)
{
    physics::OBBCollider obb(math::Vector3(1.0f, 1.0f, 1.0f));
    obb.Update(math::Vector3::ZERO,
               math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.25f));
    Register({ Instance(obb) });

    ASSERT_TRUE(world.Raycast(math::Vector3(0.0f, 0.0f, -5.0f),
                              math::Vector3::FORWARD, 100.0f, hit));

    /// @note 45° 回した立方体は角がレイ側へ出る。外接 AABB で判定していると 4.0 になり、
    ///       回転を無視していることがここで露見する。
    EXPECT_NEAR(hit.distance, 5.0f - std::sqrt(2.0f), testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(hit.normal, testkit::kLooseTolerance);
    EXPECT_LT(math::Vector3::Dot(hit.normal, math::Vector3::FORWARD), 0.0f);
}

/// @name カプセル

TEST_F(WorldRaycastShapeTest, RaycastHitsTheCylindricalSideOfACapsule)
{
    physics::CapsuleCollider capsule(0.5f, 1.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(capsule) });

    ASSERT_TRUE(world.Raycast(math::Vector3(-5.0f, 0.0f, 0.0f),
                              math::Vector3::RIGHT, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 4.5f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastHitsTheRoundedEndOfACapsule)
{
    physics::CapsuleCollider capsule(0.5f, 1.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(capsule) });

    /// @note 中心線の «外» にある半球。無限円柱の解だけを見ていると軸の外側で棄却され、
    ///       頭の上から撃った弾がすり抜ける。
    ASSERT_TRUE(world.Raycast(math::Vector3(0.0f, 5.0f, 0.0f), kDown, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 3.5f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, math::Vector3::UP, testkit::kLooseTolerance);
}

/// @name 円柱

TEST_F(WorldRaycastShapeTest, RaycastHitsTheSideOfACylinder)
{
    physics::CylinderCollider cylinder(1.0f, 1.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(cylinder) });

    ASSERT_TRUE(world.Raycast(math::Vector3(-5.0f, 0.0f, 0.0f),
                              math::Vector3::RIGHT, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 4.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastHitsTheFlatCapOfACylinder)
{
    physics::CylinderCollider cylinder(1.0f, 1.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(cylinder) });

    /// @note 円柱をカプセルで代用すると天面が丸くなり、ここが 3.5 付近へずれる。
    ASSERT_TRUE(world.Raycast(math::Vector3(0.0f, 5.0f, 0.0f), kDown, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 4.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, math::Vector3::UP, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastMissesACylinderOutsideItsRim)
{
    physics::CylinderCollider cylinder(1.0f, 1.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(cylinder) });

    /// @note 軸に平行で半径の外。円板の平面との交点だけを見て半径を確かめ忘れると当たってしまう。
    EXPECT_FALSE(world.Raycast(math::Vector3(0.0f, 5.0f, 2.0f), kDown, 100.0f, hit));
}

/// @name 三角メッシュ / ハイトフィールド

TEST_F(WorldRaycastShapeTest, RaycastHitsATriangleMeshThroughItsBVH)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(mesh) });

    ASSERT_TRUE(world.Raycast(math::Vector3(0.3f, 5.0f, 0.3f), kDown, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 5.0f, testkit::kLooseTolerance);
    /// @note 面の向きに関わらず、法線はレイを迎える側へ揃える。裏から撃っても «床» として扱える。
    EXPECT_VEC3_NEAR(hit.normal, math::Vector3::UP, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastMissesATriangleMeshBesideThePath)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(mesh) });

    EXPECT_FALSE(world.Raycast(math::Vector3(5.0f, 5.0f, 5.0f), kDown, 100.0f, hit));
}

TEST_F(WorldRaycastShapeTest, RaycastHitsAHeightField)
{
    const std::vector<float> flat(9, 0.0f);
    physics::HeightFieldCollider field(flat, 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(field) });

    /// @note 地形は接地判定の受け皿。ここが抜けると «地面はあるのに落ち続ける» になる。
    ASSERT_TRUE(world.Raycast(math::Vector3(0.3f, 5.0f, 0.3f), kDown, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 5.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, math::Vector3::UP, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, RaycastFallsThroughAHeightFieldHoleButHitsTheNeighborCell)
{
    /// @note セル (0, 0) だけ穴。穴は三角形を作らないことで表す。
    /// @see Docs/design/terrain-layers.md §4 穴
    const std::vector<float> flat(9, 0.0f);
    physics::HeightFieldCollider field(flat, 3, 3, 1.0f, 1.0f, { 1, 0, 0, 0 });
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(field) });

    EXPECT_FALSE(world.Raycast(math::Vector3(0.3f, 5.0f, 0.3f), kDown, 100.0f, hit));
    ASSERT_TRUE(world.Raycast(math::Vector3(1.3f, 5.0f, 0.3f), kDown, 100.0f, hit));
    EXPECT_NEAR(hit.distance, 5.0f, testkit::kLooseTolerance);
}

/// @name 凸包

TEST_F(WorldRaycastShapeTest, RaycastHitsTheFaceOfAConvexHull)
{
    physics::ConvexHullCollider hull(CubePoints(1.0f));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(hull) });

    ASSERT_TRUE(world.Raycast(math::Vector3(-5.0f, 0.3f, 0.1f),
                              math::Vector3::RIGHT, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 4.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
}

/// @name SphereCast

TEST_F(WorldRaycastShapeTest, SphereCastFindsABoxThatAZeroWidthRayPassesBeside)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3(0.0f, 0.0f, 10.0f), math::Quaternion::Identity());
    Register({ Instance(box) });

    const math::Vector3 origin(0.0f, 1.5f, 0.0f);
    ASSERT_FALSE(world.Raycast(origin, math::Vector3::FORWARD, 100.0f, hit));

    /// @note 太さのあるキャラクターの移動判定はこちらを使う。箱を半径ぶん膨らませるので、
    ///       手前の面 (z = 9) が半径 1 だけ手前 (z = 8) へせり出す。
    ASSERT_TRUE(world.SphereCast(origin, 1.0f, math::Vector3::FORWARD, 100.0f, hit));
    EXPECT_NEAR(hit.distance, 8.0f, testkit::kLooseTolerance);
}

TEST_F(WorldRaycastShapeTest, SphereCastFindsACapsuleThatAZeroWidthRayPassesBeside)
{
    physics::CapsuleCollider capsule(0.5f, 1.0f);
    capsule.Update(math::Vector3(0.0f, 0.0f, 10.0f), math::Quaternion::Identity());
    Register({ Instance(capsule) });

    const math::Vector3 origin(1.0f, 0.0f, 0.0f);
    ASSERT_FALSE(world.Raycast(origin, math::Vector3::FORWARD, 100.0f, hit));

    ASSERT_TRUE(world.SphereCast(origin, 0.6f, math::Vector3::FORWARD, 100.0f, hit));
    EXPECT_EQ(hit.collider, static_cast<const physics::Collider*>(&capsule));
}

TEST_F(WorldRaycastShapeTest, SphereCastStillMissesWhatIsFartherThanTheCastRadius)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3(0.0f, 0.0f, 10.0f), math::Quaternion::Identity());
    Register({ Instance(box) });

    /// @note 膨張がそのまま «全部当たる» にならないこと。ここが緩いと壁抜け判定が常時真になる。
    EXPECT_FALSE(world.SphereCast(math::Vector3(0.0f, 5.0f, 0.0f), 1.0f,
                                  math::Vector3::FORWARD, 100.0f, hit));
}

/// @name OverlapSphere

TEST_F(WorldRaycastShapeTest, OverlapSphereMeasuresTheNearestPointOnABox)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(box) });

    /// @note 中心間距離で判定していると、角に寄った球を «遠い» と誤判定する。
    EXPECT_EQ(world.OverlapSphere(math::Vector3(2.5f, 0.0f, 0.0f), 2.0f).size(), 1u);
    EXPECT_TRUE(world.OverlapSphere(math::Vector3(2.5f, 0.0f, 0.0f), 1.0f).empty());
}

TEST_F(WorldRaycastShapeTest, OverlapSphereMeasuresDistanceToTheCapsuleSegment)
{
    physics::CapsuleCollider capsule(0.5f, 1.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(capsule) });

    EXPECT_EQ(world.OverlapSphere(math::Vector3(0.0f, 2.0f, 0.0f), 0.6f).size(), 1u);
    EXPECT_TRUE(world.OverlapSphere(math::Vector3(0.0f, 3.0f, 0.0f), 1.0f).empty());
}

TEST_F(WorldRaycastShapeTest, OverlapSphereUsesTheClosestPointOnACylinder)
{
    physics::CylinderCollider cylinder(1.0f, 1.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(cylinder) });

    EXPECT_EQ(world.OverlapSphere(math::Vector3(1.4f, 0.0f, 0.0f), 0.5f).size(), 1u);
    EXPECT_TRUE(world.OverlapSphere(math::Vector3(2.0f, 0.0f, 0.0f), 0.5f).empty());
}

TEST_F(WorldRaycastShapeTest, OverlapSphereApproximatesAConvexHullWithItsBounds)
{
    physics::ConvexHullCollider hull(CubePoints(1.0f));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    Register({ Instance(hull) });

    /// @note 凸包は外接箱で保守的に判定する。取りこぼしはしないが、角の外側でも拾う。
    EXPECT_EQ(world.OverlapSphere(math::Vector3(1.5f, 0.0f, 0.0f), 0.6f).size(), 1u);
    EXPECT_TRUE(world.OverlapSphere(math::Vector3(3.0f, 0.0f, 0.0f), 0.5f).empty());
}

} // namespace fbzz::tests
