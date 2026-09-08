/// @file    MeshColliderTests.cpp
/// @brief   三角メッシュ / ハイトフィールドが生データから BVH を組み、ワールド変換へ追従することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// BVH そのものの不変条件は BVHTests が見ている。ここで見るのは «入口» ──
/// 頂点とインデックス、あるいは高さの格子から、正しい枚数・正しい位置の三角形が
/// 作られるか。ここがずれると地形の «見えない床» が実際の地面とずれ、
/// 足が浮くか沈むかのどちらかになる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

/// y=0 に広がる 2 三角形の板。角は (±1, 0, ±1)。
std::vector<math::Vector3> QuadPositions()
{
    return {
        { -1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, -1.0f },
        { -1.0f, 0.0f,  1.0f }, { 1.0f, 0.0f,  1.0f },
    };
}

std::vector<std::uint32_t> QuadIndices() { return { 0, 2, 1, 1, 2, 3 }; }

/// 三角形群の外接箱。BVH が持つワールド三角形から直接測る。
physics::AABB BoundsOf(const physics::BVHTree& bvh)
{
    physics::AABB bounds{ { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };
    for (const physics::Triangle& tri : bvh.triangles) {
        for (const math::Vector3& v : tri.v) {
            bounds.min = { std::min(bounds.min.x, v.x), std::min(bounds.min.y, v.y),
                           std::min(bounds.min.z, v.z) };
            bounds.max = { std::max(bounds.max.x, v.x), std::max(bounds.max.y, v.y),
                           std::max(bounds.max.z, v.z) };
        }
    }
    return bounds;
}

} // namespace

// --- 三角メッシュ -----------------------------------------------------------

class TriangleMeshColliderTest : public testkit::Fixture {};

TEST_F(TriangleMeshColliderTest, BuildsOneTrianglePerIndexTriple)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(mesh.GetBVH().triangles.size(), 2u);
}

TEST_F(TriangleMeshColliderTest, ReportsItsType)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());

    EXPECT_EQ(mesh.GetType(), physics::ColliderType::TRIANGLE_MESH);
}

TEST_F(TriangleMeshColliderTest, PlacesTrianglesAtTheWorldTransform)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update({ 10.0f, 5.0f, 0.0f }, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(9.0f, 5.0f, -1.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(11.0f, 5.0f, 1.0f), testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, RotatesTheTrianglesWithTheTransform)
{
    // 板を X 軸まわりに 90 度倒すと、床だった面が壁になる。
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO,
                math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_NEAR(bounds.max.y - bounds.min.y, 2.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 0.0f, testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, ScalesTheTriangles)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(),
                         { 3.0f, 1.0f, 1.0f });

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_NEAR(bounds.max.x, 3.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 1.0f, testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, BoundsEncloseEveryTriangle)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update({ 2.0f, -1.0f, 3.0f },
                math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(30.0f)));

    const physics::AABB reported = mesh.GetAABB();
    const physics::AABB actual   = BoundsOf(mesh.GetBVH());

    EXPECT_LE(reported.min.x, actual.min.x + testkit::kLooseTolerance);
    EXPECT_LE(reported.min.z, actual.min.z + testkit::kLooseTolerance);
    EXPECT_GE(reported.max.x, actual.max.x - testkit::kLooseTolerance);
    EXPECT_GE(reported.max.z, actual.max.z - testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, SurvivesAnEmptyMesh)
{
    // インポート途中や «コライダーだけ付けた» 状態は普通に通る。
    physics::TriangleMeshCollider mesh({}, {});
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_TRUE(mesh.GetBVH().triangles.empty());
}

TEST_F(TriangleMeshColliderTest, IgnoresATrailingPartialTriangle)
{
    // インデックスが 3 の倍数でないデータが来ても、途中まで使って落ちないこと。
    physics::TriangleMeshCollider mesh(QuadPositions(), { 0, 2, 1, 1, 2 });
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(mesh.GetBVH().triangles.size(), 1u);
}

// --- ハイトフィールド -------------------------------------------------------

class HeightFieldColliderTest : public testkit::Fixture {};

TEST_F(HeightFieldColliderTest, BuildsTwoTrianglesPerCell)
{
    // 3x3 の格子は 2x2 のセル = 8 三角形。枚数が違うと地形に穴か重なりができる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(field.GetBVH().triangles.size(), 8u);
}

TEST_F(HeightFieldColliderTest, ReportsItsGridSettings)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 10.0f);

    EXPECT_EQ(field.GetRows(), 3);
    EXPECT_EQ(field.GetCols(), 3);
    EXPECT_NEAR(field.GetCellSize(), 2.0f, testkit::kTolerance);
    EXPECT_NEAR(field.GetMaxHeight(), 10.0f, testkit::kTolerance);
    EXPECT_EQ(field.GetType(), physics::ColliderType::HEIGHT_FIELD);
}

TEST_F(HeightFieldColliderTest, SpansCellSizeTimesTheCellCount)
{
    // 格子の原点は隅。cellSize 2 の 2x2 セルなら [0, 4] に広がる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3::ZERO, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(4.0f, 0.0f, 4.0f), testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, ScalesTheStoredHeightsByMaxHeight)
{
    // heights は [-1, 1] の正規化値。maxHeight を掛け忘れると地形が平らになる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.5f), 3, 3, 1.0f, 20.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_NEAR(bounds.min.y, 10.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.y, 10.0f, testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, PlacesTheGridAtTheWorldTransform)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 1.0f);
    field.Update({ -2.0f, 3.0f, -2.0f }, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(-2.0f, 3.0f, -2.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(2.0f, 3.0f, 2.0f), testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, RebuildReplacesTheTerrainShape)
{
    // 地形を彫った後に呼ぶ経路。呼んでも高さが変わらないと «見えない古い地面» が残る。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 10.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    ASSERT_NEAR(BoundsOf(field.GetBVH()).max.y, 0.0f, testkit::kLooseTolerance);

    field.Rebuild(std::vector<float>(9, 1.0f), 3, 3, 1.0f, 10.0f);

    EXPECT_NEAR(BoundsOf(field.GetBVH()).max.y, 10.0f, testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, RebuildCanChangeTheResolution)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    field.Rebuild(std::vector<float>(16, 0.0f), 4, 4, 1.0f, 1.0f);

    EXPECT_EQ(field.GetRows(), 4);
    EXPECT_EQ(field.GetBVH().triangles.size(), 18u);   // 3x3 セル
}

} // namespace fbzz::tests
