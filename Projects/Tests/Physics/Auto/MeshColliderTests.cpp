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

// 以下 3 本は «2 回目以降の Update»。スケールが変わらない移動・回転は BVH を組み直さず
// refit で済ませる経路に入るので、組み直した場合と同じ結果になることを見る。

TEST_F(TriangleMeshColliderTest, RefitAndRebuildAgreeAfterAMove)
{
    physics::TriangleMeshCollider refitted(QuadPositions(), QuadIndices());
    refitted.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    refitted.Update({ 10.0f, 5.0f, 0.0f }, math::Quaternion::Identity());

    physics::TriangleMeshCollider rebuilt(QuadPositions(), QuadIndices());
    rebuilt.Update({ 10.0f, 5.0f, 0.0f }, math::Quaternion::Identity());

    EXPECT_EQ(refitted.GetBVH().triangles.size(), rebuilt.GetBVH().triangles.size());
    EXPECT_VEC3_NEAR(BoundsOf(refitted.GetBVH()).min, BoundsOf(rebuilt.GetBVH()).min,
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(BoundsOf(refitted.GetBVH()).max, BoundsOf(rebuilt.GetBVH()).max,
                     testkit::kLooseTolerance);
    // GetAABB() は BroadPhase が読む値。refit 後もノード AABB から引き直せていること。
    EXPECT_VEC3_NEAR(refitted.GetAABB().min, math::Vector3(9.0f, 5.0f, -1.0f),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(refitted.GetAABB().max, math::Vector3(11.0f, 5.0f, 1.0f),
                     testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, RefitFollowsARotationAfterTheFirstUpdate)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    mesh.Update(math::Vector3::ZERO,
                math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());
    EXPECT_NEAR(bounds.max.y - bounds.min.y, 2.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 0.0f, testkit::kLooseTolerance);
}

TEST_F(TriangleMeshColliderTest, RebuildsInsteadOfRefittingWhenTheScaleChanges)
{
    // WHY refit しないか: 非一様スケールは三角形どうしの相対配置ごと変えるため、
    //     構築時に選んだ分割軸が的外れなまま残る。ここは組み直し側に落ちる。
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    mesh.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 3.0f, 1.0f, 1.0f });

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());
    EXPECT_NEAR(bounds.max.x, 3.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 1.0f, testkit::kLooseTolerance);
    EXPECT_EQ(mesh.GetBVH().triangles.size(), 2u);
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

// 以下 4 本は «2 回目以降の Update» を見る。
// WHY 分けて置くか: 初回の Update は BVH の «構築» 経路を通るため、1 回しか呼ばない
//     テストでは transform 追従を確かめたことにならない。地形を置いたあとエディタで
//     動かす経路はこちらで、ここが抜けていたせいで «当たり判定だけ元の場所に残る»
//     不具合が通っていた。

TEST_F(HeightFieldColliderTest, FollowsAMoveAfterTheFirstUpdate)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    ASSERT_NEAR(BoundsOf(field.GetBVH()).min.x, 0.0f, testkit::kLooseTolerance);

    field.Update({ 100.0f, 5.0f, -20.0f }, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());
    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(100.0f, 5.0f, -20.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(102.0f, 5.0f, -18.0f), testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, FollowsARotationAfterTheFirstUpdate)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    // X 軸まわりに 90 度倒すと、z∈[0,2] に広がっていた床が y 方向の壁になる。
    field.Update(math::Vector3::ZERO,
                 math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    const physics::AABB bounds = BoundsOf(field.GetBVH());
    EXPECT_NEAR(bounds.max.y - bounds.min.y, 2.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 2.0f, testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, FollowsAScaleChangeAfterTheFirstUpdate)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 1.0f, 1.0f, 1.0f });

    field.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 3.0f, 1.0f, 3.0f });

    const physics::AABB bounds = BoundsOf(field.GetBVH());
    EXPECT_NEAR(bounds.max.x, 6.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 6.0f, testkit::kLooseTolerance);
}

TEST_F(HeightFieldColliderTest, BVHNodesAndBoundsFollowTheMoveToo)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    field.Update({ 100.0f, 0.0f, 0.0f }, math::Quaternion::Identity());

    // WHY Query まで見るか: 三角形の頂点だけ書き換えてノード AABB の refit を忘れると、
    //     頂点は正しい位置なのに BroadPhase が候補を 1 つも返さず «すり抜ける床» になる。
    //     GetAABB() は World のブロードフェーズが読む値で、こちらがずれると同じ症状が出る。
    int atOldPlace = 0;
    field.GetBVH().Query(physics::AABB{ { -3.0f, -3.0f, -3.0f }, { 1.0f, 3.0f, 3.0f } },
                         [&](const physics::Triangle&) { ++atOldPlace; });
    int atNewPlace = 0;
    field.GetBVH().Query(physics::AABB{ { 99.0f, -3.0f, -3.0f }, { 103.0f, 3.0f, 3.0f } },
                         [&](const physics::Triangle&) { ++atNewPlace; });

    EXPECT_EQ(atOldPlace, 0);
    EXPECT_EQ(atNewPlace, 8);   // 2x2 セル
    EXPECT_NEAR(field.GetAABB().min.x, 100.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(field.GetAABB().max.x, 102.0f, testkit::kLooseTolerance);
}

} // namespace fbzz::tests
