/// @file    MeshColliderTests.cpp
/// @brief   三角メッシュ / ハイトフィールドが生データから BVH を組み、ワールド変換へ追従することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
/// @note BVH 自体の不変条件は BVHTests で、地形などから生成されるワールド三角形はここで検証する。
#include <TestKit/TestKit.hpp>

#include <Math/Scalar/MathUtils.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <Physics/Collision/Shapes/TriangleMeshCollider.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

/// @note y=0 に広がる 2 三角形の板。角は (±1, 0, ±1)。
std::vector<math::Vector3> QuadPositions()
{
    return {
        { -1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, -1.0f },
        { -1.0f, 0.0f,  1.0f }, { 1.0f, 0.0f,  1.0f },
    };
}

std::vector<std::uint32_t> QuadIndices() { return { 0, 2, 1, 1, 2, 3 }; }

/// @note ノードの境界とは独立に、保持されたワールド三角形から測る。
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

} /// @note namespace

/// @name 三角メッシュ

class TriangleMeshColliderTest : public testkit::Fixture {};

/// @brief Builds one triangle per index triple.
TEST_F(TriangleMeshColliderTest, BuildsOneTrianglePerIndexTriple)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(mesh.GetBVH().triangles.size(), 2u);
}

/// @brief Reports its type.
TEST_F(TriangleMeshColliderTest, ReportsItsType)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());

    EXPECT_EQ(mesh.GetType(), physics::ColliderType::TRIANGLE_MESH);
}

/// @brief Places triangles at the world transform.
TEST_F(TriangleMeshColliderTest, PlacesTrianglesAtTheWorldTransform)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update({ 10.0f, 5.0f, 0.0f }, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(9.0f, 5.0f, -1.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(11.0f, 5.0f, 1.0f), testkit::kLooseTolerance);
}

/// @brief Rotates the triangles with the transform.
TEST_F(TriangleMeshColliderTest, RotatesTheTrianglesWithTheTransform)
{
    /// @note 板を X 軸まわりに 90 度倒すと、床だった面が壁になる。
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO,
                math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_NEAR(bounds.max.y - bounds.min.y, 2.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 0.0f, testkit::kLooseTolerance);
}

/// @brief Scales the triangles.
TEST_F(TriangleMeshColliderTest, ScalesTheTriangles)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(),
                         { 3.0f, 1.0f, 1.0f });

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());

    EXPECT_NEAR(bounds.max.x, 3.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 1.0f, testkit::kLooseTolerance);
}

/// @brief Bounds enclose every triangle.
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

/// @brief Survives an empty mesh.
TEST_F(TriangleMeshColliderTest, SurvivesAnEmptyMesh)
{
    /// @note インポート途中や «コライダーだけ付けた» 状態は普通に通る。
    physics::TriangleMeshCollider mesh({}, {});
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_TRUE(mesh.GetBVH().triangles.empty());
}

/// @brief Ignores a trailing partial triangle.
TEST_F(TriangleMeshColliderTest, IgnoresATrailingPartialTriangle)
{
    /// @note インデックスが 3 の倍数でないデータが来ても、途中まで使って落ちないこと。
    physics::TriangleMeshCollider mesh(QuadPositions(), { 0, 2, 1, 1, 2 });
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(mesh.GetBVH().triangles.size(), 1u);
}

/// @brief Refit and rebuild agree after a move.
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
    /// @note GetAABB() は BroadPhase が読む値。refit 後もノード AABB から引き直せていること。
    EXPECT_VEC3_NEAR(refitted.GetAABB().min, math::Vector3(9.0f, 5.0f, -1.0f),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(refitted.GetAABB().max, math::Vector3(11.0f, 5.0f, 1.0f),
                     testkit::kLooseTolerance);
}

/// @brief Refit follows a rotation after the first update.
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

TEST_F(TriangleMeshColliderTest, RefitAndQueryKeepTheInputIndexAfterSkippingADegenerateTriangle)
{
    physics::TriangleMeshCollider mesh(QuadPositions(), {0, 0, 0, 0, 2, 1});
    const auto& bvh = mesh.GetBVH();
    ASSERT_EQ(bvh.triangles.size(), 1u);
    EXPECT_EQ(bvh.triangles.front().index, 1u);

    mesh.Update({10.0f, 0.0f, 0.0f}, math::Quaternion::Identity());
    EXPECT_VEC3_NEAR(bvh.triangles.front().v[0], math::Vector3(9.0f, 0.0f, -1.0f), testkit::kLooseTolerance);

    std::vector<uint32_t> foundTriangleIndices;
    const auto recordTriangle = [&](const physics::Triangle& triangle) {
        foundTriangleIndices.push_back(triangle.index);
    };
    bvh.Query({{-2.0f, -1.0f, -2.0f}, {2.0f, 1.0f, 2.0f}}, recordTriangle);
    EXPECT_TRUE(foundTriangleIndices.empty());
    bvh.Query({{8.0f, -1.0f, -2.0f}, {12.0f, 1.0f, 2.0f}}, recordTriangle);
    EXPECT_EQ(foundTriangleIndices, std::vector<uint32_t>{1u});
}

/// @brief Rebuilds instead of refitting when the scale changes.
TEST_F(TriangleMeshColliderTest, RebuildsInsteadOfRefittingWhenTheScaleChanges)
{
    /// @note refit しない。非一様スケールは三角形どうしの相対配置ごと変えるため、構築時に選んだ
    ///       分割軸が的外れなまま残る (組み直し側に落ちる)。
    physics::TriangleMeshCollider mesh(QuadPositions(), QuadIndices());
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    mesh.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 3.0f, 1.0f, 1.0f });

    const physics::AABB bounds = BoundsOf(mesh.GetBVH());
    EXPECT_NEAR(bounds.max.x, 3.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 1.0f, testkit::kLooseTolerance);
    EXPECT_EQ(mesh.GetBVH().triangles.size(), 2u);
}

/// @name ハイトフィールド

class HeightFieldColliderTest : public testkit::Fixture {};

/// @brief Builds two triangles per cell.
TEST_F(HeightFieldColliderTest, BuildsTwoTrianglesPerCell)
{
    /// @note 3x3 の格子は 2x2 のセル = 8 三角形。枚数が違うと地形に穴か重なりができる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(field.GetBVH().triangles.size(), 8u);
}

/// @brief Reports its grid settings.
TEST_F(HeightFieldColliderTest, ReportsItsGridSettings)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 10.0f);

    EXPECT_EQ(field.GetRows(), 3);
    EXPECT_EQ(field.GetCols(), 3);
    EXPECT_NEAR(field.GetCellSize(), 2.0f, testkit::kTolerance);
    EXPECT_NEAR(field.GetMaxHeight(), 10.0f, testkit::kTolerance);
    EXPECT_EQ(field.GetType(), physics::ColliderType::HEIGHT_FIELD);
}

/// @brief Spans cell size times the cell count.
TEST_F(HeightFieldColliderTest, SpansCellSizeTimesTheCellCount)
{
    /// @note 格子の原点は隅。cellSize 2 の 2x2 セルなら [0, 4] に広がる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3::ZERO, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(4.0f, 0.0f, 4.0f), testkit::kLooseTolerance);
}

/// @brief Scales the stored heights by max height.
TEST_F(HeightFieldColliderTest, ScalesTheStoredHeightsByMaxHeight)
{
    /// @note heights は [-1, 1] の正規化値。maxHeight を掛け忘れると地形が平らになる。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.5f), 3, 3, 1.0f, 20.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_NEAR(bounds.min.y, 10.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.y, 10.0f, testkit::kLooseTolerance);
}

/// @brief Places the grid at the world transform.
TEST_F(HeightFieldColliderTest, PlacesTheGridAtTheWorldTransform)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 2.0f, 1.0f);
    field.Update({ -2.0f, 3.0f, -2.0f }, math::Quaternion::Identity());

    const physics::AABB bounds = BoundsOf(field.GetBVH());

    EXPECT_VEC3_NEAR(bounds.min, math::Vector3(-2.0f, 3.0f, -2.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(bounds.max, math::Vector3(2.0f, 3.0f, 2.0f), testkit::kLooseTolerance);
}

/// @brief Rebuild replaces the terrain shape.
TEST_F(HeightFieldColliderTest, RebuildReplacesTheTerrainShape)
{
    /// @note 地形を彫った後に呼ぶ経路。呼んでも高さが変わらないと «見えない古い地面» が残る。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 10.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    ASSERT_NEAR(BoundsOf(field.GetBVH()).max.y, 0.0f, testkit::kLooseTolerance);

    field.Rebuild(std::vector<float>(9, 1.0f), 3, 3, 1.0f, 10.0f);

    EXPECT_NEAR(BoundsOf(field.GetBVH()).max.y, 10.0f, testkit::kLooseTolerance);
}

/// @brief Rebuild can change the resolution.
TEST_F(HeightFieldColliderTest, RebuildCanChangeTheResolution)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    field.Rebuild(std::vector<float>(16, 0.0f), 4, 4, 1.0f, 1.0f);

    EXPECT_EQ(field.GetRows(), 4);
    /// @note 3x3 セル
    EXPECT_EQ(field.GetBVH().triangles.size(), 18u);
}

/// @note 2 回目以降の Update で、高さ場の構築後に姿勢へ追従する経路を検証する。

/// @brief Follows a move after the first update.
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

/// @brief Follows a rotation after the first update.
TEST_F(HeightFieldColliderTest, FollowsARotationAfterTheFirstUpdate)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    /// @note X 軸まわりに 90 度倒すと、z∈[0,2] に広がっていた床が y 方向の壁になる。
    field.Update(math::Vector3::ZERO,
                 math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::HALF_PI));

    const physics::AABB bounds = BoundsOf(field.GetBVH());
    EXPECT_NEAR(bounds.max.y - bounds.min.y, 2.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 2.0f, testkit::kLooseTolerance);
}

/// @brief Follows a scale change after the first update.
TEST_F(HeightFieldColliderTest, FollowsAScaleChangeAfterTheFirstUpdate)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 1.0f, 1.0f, 1.0f });

    field.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(), { 3.0f, 1.0f, 3.0f });

    const physics::AABB bounds = BoundsOf(field.GetBVH());
    EXPECT_NEAR(bounds.max.x, 6.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(bounds.max.z, 6.0f, testkit::kLooseTolerance);
}

/// @brief BVH nodes and bounds follow the move too.
TEST_F(HeightFieldColliderTest, BVHNodesAndBoundsFollowTheMoveToo)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    field.Update({ 100.0f, 0.0f, 0.0f }, math::Quaternion::Identity());

    /// @note 三角形の頂点だけ書き換えてノード AABB の refit を忘れると、頂点は正しい位置なのに
    ///       BroadPhase が候補を返さず «すり抜ける床» になる。GetAABB() は World のブロードフェーズが
    ///       読む値で、こちらがずれると同じ症状が出るため Query まで見る。
    int atOldPlace = 0;
    field.GetBVH().Query(physics::AABB{ { -3.0f, -3.0f, -3.0f }, { 1.0f, 3.0f, 3.0f } },
                         [&](const physics::Triangle&) { ++atOldPlace; });
    int atNewPlace = 0;
    field.GetBVH().Query(physics::AABB{ { 99.0f, -3.0f, -3.0f }, { 103.0f, 3.0f, 3.0f } },
                         [&](const physics::Triangle&) { ++atNewPlace; });

    EXPECT_EQ(atOldPlace, 0);
    /// @note 2x2 セル
    EXPECT_EQ(atNewPlace, 8);
    EXPECT_NEAR(field.GetAABB().min.x, 100.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(field.GetAABB().max.x, 102.0f, testkit::kLooseTolerance);
}

/// @name 穴
/// @see Docs/design/terrain-layers.md §4 穴

/// @brief Hole cells build no triangles.
TEST_F(HeightFieldColliderTest, HoleCellsBuildNoTriangles)
{
    /// @note 2x2 セルのうち (1, 0) だけ穴。index = cz * (cols - 1) + cx。
    const std::vector<std::uint8_t> holes{ 0, 1, 0, 0 };
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f, holes);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(field.GetBVH().triangles.size(), 6u);
    EXPECT_TRUE(field.IsHoleCell(1, 0));
    EXPECT_FALSE(field.IsHoleCell(0, 0));
    for (const physics::Triangle& tri : field.GetBVH().triangles)
        EXPECT_NE(tri.index / 2u, 1u) << "穴セルの三角形が残っている";
}

/// @brief Mismatched hole mask is ignored.
TEST_F(HeightFieldColliderTest, MismatchedHoleMaskIsIgnored)
{
    /// @note セル数と合わない穴マスクを部分的に読むと格子とずれた位置に穴が開く。
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f, { 1, 1 });
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(field.GetBVH().triangles.size(), 8u);
    EXPECT_TRUE(field.GetHoles().empty());
}

/// @brief Refit after move keeps hole cells empty.
TEST_F(HeightFieldColliderTest, RefitAfterMoveKeepsHoleCellsEmpty)
{
    /// @note RefitTransform は Triangle::index から格子を逆算する。穴で index を進め忘れると
    ///       移動後に三角形が隣のセルへずれ、穴の位置が動く。
    const std::vector<std::uint8_t> holes{ 1, 0, 0, 0 };
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f, holes);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    field.Update({ 100.0f, 0.0f, 0.0f }, math::Quaternion::Identity());

    int inHole = 0;
    field.GetBVH().Query(physics::AABB{ { 100.1f, -1.0f, 0.1f }, { 100.9f, 1.0f, 0.9f } },
                         [&](const physics::Triangle&) { ++inHole; });
    int inNeighbor = 0;
    field.GetBVH().Query(physics::AABB{ { 101.1f, -1.0f, 0.1f }, { 101.9f, 1.0f, 0.9f } },
                         [&](const physics::Triangle&) { ++inNeighbor; });

    EXPECT_EQ(inHole, 0);
    EXPECT_EQ(inNeighbor, 2);
    EXPECT_EQ(field.GetBVH().triangles.size(), 6u);
}

TEST_F(HeightFieldColliderTest, RebuildWithEveryCellMaskedCollapsesBoundsToTheTerrainOrigin)
{
    const std::vector<float> heightSamples(9, 0.5f);
    const math::Vector3 originalPosition{8.0f, 3.0f, -4.0f};
    physics::HeightFieldCollider field(heightSamples, 3, 3, 1.0f, 2.0f);
    field.Update(originalPosition, math::Quaternion::Identity());
    ASSERT_FALSE(field.GetBVH().nodes.empty());

    field.Rebuild(heightSamples, 3, 3, 1.0f, 2.0f, {1, 2, 3, 4});
    EXPECT_TRUE(field.GetBVH().triangles.empty());
    EXPECT_TRUE(field.GetBVH().nodes.empty());
    EXPECT_VEC3_NEAR(field.GetAABB().min, originalPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(field.GetAABB().max, originalPosition, testkit::kTolerance);

    const math::Vector3 movedPosition{-5.0f, 0.0f, 2.0f};
    field.Update(movedPosition, math::Quaternion::Identity());
    EXPECT_TRUE(field.GetBVH().triangles.empty());
    EXPECT_TRUE(field.GetBVH().nodes.empty());
    EXPECT_VEC3_NEAR(field.GetAABB().min, movedPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(field.GetAABB().max, movedPosition, testkit::kTolerance);
}

/// @brief Rebuild can clear holes.
TEST_F(HeightFieldColliderTest, RebuildCanClearHoles)
{
    physics::HeightFieldCollider field(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f, { 1, 1, 1, 1 });
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    ASSERT_TRUE(field.GetBVH().triangles.empty());

    field.Rebuild(std::vector<float>(9, 0.0f), 3, 3, 1.0f, 1.0f);

    EXPECT_EQ(field.GetBVH().triangles.size(), 8u);
}

} /// @note namespace fbzz::tests
