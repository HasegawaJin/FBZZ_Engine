/// @file    BVHTests.cpp
/// @brief   三角メッシュ用 BVH の構築不変条件と、Query が総当たりと一致することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// BVH が «取りこぼす» と地形をすり抜け、«余分に返す» と重くなるだけで結果は正しい。
/// つまりバグは «たまに落ちる» という形でしか表に出ない。総当たりとの一致を直接見る。
#include <TestKit/TestKit.hpp>

#include <Physics/BVHNode.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace fbzz::tests {

namespace {

/// X 軸に沿って 1m 間隔で並ぶ、一辺 0.8m の直角三角形。
std::vector<physics::Triangle> TriangleStrip(int count)
{
    std::vector<physics::Triangle> triangles;
    triangles.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(i);

        physics::Triangle tri;
        tri.v[0]   = {x, 0.0f, 0.0f};
        tri.v[1]   = {x + 0.8f, 0.0f, 0.0f};
        tri.v[2]   = {x, 0.8f, 0.0f};
        tri.normal = math::Vector3::FORWARD;
        tri.index  = static_cast<uint32_t>(i);
        triangles.push_back(tri);
    }
    return triangles;
}

physics::AABB BoundsOf(const physics::Triangle& tri)
{
    physics::AABB bounds{tri.v[0], tri.v[0]};
    for (int k = 1; k < 3; ++k) {
        bounds.min.x = std::min(bounds.min.x, tri.v[k].x);
        bounds.min.y = std::min(bounds.min.y, tri.v[k].y);
        bounds.min.z = std::min(bounds.min.z, tri.v[k].z);
        bounds.max.x = std::max(bounds.max.x, tri.v[k].x);
        bounds.max.y = std::max(bounds.max.y, tri.v[k].y);
        bounds.max.z = std::max(bounds.max.z, tri.v[k].z);
    }
    return bounds;
}

/// BVH を通さずに、同じ «三角形の外接箱が重なるか» の規則で総当たりする。
std::set<uint32_t> BruteForceQuery(const std::vector<physics::Triangle>& triangles,
                                   const physics::AABB&                  query)
{
    std::set<uint32_t> hits;
    for (const physics::Triangle& tri : triangles) {
        if (BoundsOf(tri).Overlaps(query)) hits.insert(tri.index);
    }
    return hits;
}

std::set<uint32_t> TreeQuery(const physics::BVHTree& tree, const physics::AABB& query)
{
    std::set<uint32_t> hits;
    tree.Query(query, [&hits](const physics::Triangle& tri) { hits.insert(tri.index); });
    return hits;
}

physics::AABB Box(const math::Vector3& min, const math::Vector3& max) { return {min, max}; }

} // namespace

class BVHTest : public testkit::Fixture {};

// --- 構築 -------------------------------------------------------------------

TEST_F(BVHTest, BuildProducesNoNodesForAnEmptyMesh)
{
    physics::BVHTree tree;
    tree.Build({});

    EXPECT_TRUE(tree.nodes.empty());
    EXPECT_TRUE(tree.triangles.empty());
}

TEST_F(BVHTest, BuildKeepsEveryTriangle)
{
    physics::BVHTree tree;
    tree.Build(TriangleStrip(37));

    EXPECT_EQ(tree.triangles.size(), 37u);
    EXPECT_FALSE(tree.nodes.empty());
}

TEST_F(BVHTest, RootBoundsEncloseEveryTriangle)
{
    physics::BVHTree tree;
    tree.Build(TriangleStrip(20));

    ASSERT_FALSE(tree.nodes.empty());
    const physics::AABB& root = tree.nodes[0].aabb;

    for (const physics::Triangle& tri : tree.triangles) {
        for (const math::Vector3& vertex : tri.v) {
            EXPECT_GE(vertex.x, root.min.x - testkit::kTolerance);
            EXPECT_GE(vertex.y, root.min.y - testkit::kTolerance);
            EXPECT_GE(vertex.z, root.min.z - testkit::kTolerance);
            EXPECT_LE(vertex.x, root.max.x + testkit::kTolerance);
            EXPECT_LE(vertex.y, root.max.y + testkit::kTolerance);
            EXPECT_LE(vertex.z, root.max.z + testkit::kTolerance);
        }
    }
}

TEST_F(BVHTest, EveryTriangleAppearsInExactlyOneLeaf)
{
    // 重複して入っていると同じ三角形と 2 回衝突判定し、押し戻しが二重に効く。
    // 抜けていると、その面だけすり抜ける。
    physics::BVHTree tree;
    tree.Build(TriangleStrip(50), 4);

    std::vector<int> appearances(tree.triangles.size(), 0);
    for (const physics::BVHNode& node : tree.nodes) {
        if (!node.IsLeaf()) continue;
        for (uint32_t index : node.triIndices) ++appearances[index];
    }

    for (size_t i = 0; i < appearances.size(); ++i) {
        EXPECT_EQ(appearances[i], 1) << "三角形 " << i;
    }
}

TEST_F(BVHTest, InternalNodesOnlyAppearWhenTheLeafLimitIsExceeded)
{
    physics::BVHTree small;
    small.Build(TriangleStrip(4), 8);

    ASSERT_EQ(small.nodes.size(), 1u);
    EXPECT_TRUE(small.nodes[0].IsLeaf());
}

TEST_F(BVHTest, LeavesHoldNoMoreThanTheRequestedTriangleCount)
{
    constexpr int    kMaxLeafTris = 3;
    physics::BVHTree tree;
    tree.Build(TriangleStrip(64), kMaxLeafTris);

    for (const physics::BVHNode& node : tree.nodes) {
        if (!node.IsLeaf()) continue;
        EXPECT_LE(node.triIndices.size(), static_cast<size_t>(kMaxLeafTris));
    }
}

TEST_F(BVHTest, ChildBoundsAreContainedInTheirParent)
{
    physics::BVHTree tree;
    tree.Build(TriangleStrip(64), 4);

    for (const physics::BVHNode& node : tree.nodes) {
        if (node.IsLeaf()) continue;
        for (const int childIndex : {node.left, node.right}) {
            ASSERT_GE(childIndex, 0);
            ASSERT_LT(static_cast<size_t>(childIndex), tree.nodes.size());

            const physics::AABB& child = tree.nodes[childIndex].aabb;
            EXPECT_GE(child.min.x, node.aabb.min.x - testkit::kTolerance);
            EXPECT_GE(child.min.y, node.aabb.min.y - testkit::kTolerance);
            EXPECT_GE(child.min.z, node.aabb.min.z - testkit::kTolerance);
            EXPECT_LE(child.max.x, node.aabb.max.x + testkit::kTolerance);
            EXPECT_LE(child.max.y, node.aabb.max.y + testkit::kTolerance);
            EXPECT_LE(child.max.z, node.aabb.max.z + testkit::kTolerance);
        }
    }
}

// --- 問い合わせ -------------------------------------------------------------

TEST_F(BVHTest, QueryOnAnEmptyTreeVisitsNothing)
{
    physics::BVHTree tree;

    int visited = 0;
    tree.Query(Box(math::Vector3(-100.0f, -100.0f, -100.0f), math::Vector3(100.0f, 100.0f, 100.0f)),
               [&visited](const physics::Triangle&) { ++visited; });

    EXPECT_EQ(visited, 0);
}

TEST_F(BVHTest, QueryFindsNothingWhenTheBoxMissesTheMesh)
{
    physics::BVHTree tree;
    tree.Build(TriangleStrip(32), 4);

    const std::set<uint32_t> hits =
        TreeQuery(tree, Box(math::Vector3(0.0f, 50.0f, 0.0f), math::Vector3(10.0f, 60.0f, 1.0f)));

    EXPECT_TRUE(hits.empty());
}

TEST_F(BVHTest, QueryFindsEveryTriangleWhenTheBoxCoversTheMesh)
{
    physics::BVHTree tree;
    tree.Build(TriangleStrip(32), 4);

    const std::set<uint32_t> hits = TreeQuery(
        tree, Box(math::Vector3(-10.0f, -10.0f, -10.0f), math::Vector3(100.0f, 100.0f, 100.0f)));

    EXPECT_EQ(hits.size(), 32u);
}

TEST_F(BVHTest, QueryFindsOnlyTheTrianglesUnderTheBox)
{
    // 三角形 i は x 方向に [i, i+0.8]。問い合わせ [4.9, 5.5] と重なるのは 5 番だけ。
    physics::BVHTree tree;
    tree.Build(TriangleStrip(32), 4);

    const std::set<uint32_t> hits =
        TreeQuery(tree, Box(math::Vector3(4.9f, 0.0f, -0.5f), math::Vector3(5.5f, 0.5f, 0.5f)));

    EXPECT_EQ(hits, std::set<uint32_t>{5u});
}

TEST_F(BVHTest, QueryMatchesABruteForceScanForRandomBoxes)
{
    // BVH の枝刈りが «結果を変えない» ことが本質。総当たりと突き合わせる。
    physics::BVHTree                     tree;
    const std::vector<physics::Triangle> triangles = TriangleStrip(64);
    tree.Build(std::vector<physics::Triangle>(triangles), 4);

    for (int i = 0; i < 128; ++i) {
        const math::Vector3 corner = Rng().NextVector3(-5.0f, 70.0f);
        const math::Vector3 size   = Rng().NextVector3(0.1f, 6.0f);
        const physics::AABB query  = Box(corner, corner + size);

        EXPECT_EQ(TreeQuery(tree, query), BruteForceQuery(triangles, query))
            << "query.min = " << ::testing::PrintToString(query.min)
            << " query.max = " << ::testing::PrintToString(query.max);
    }
}

TEST_F(BVHTest, QueryMatchesABruteForceScanForASingleTriangle)
{
    // 三角形 1 枚は «根がそのまま葉» という最小構成。境界条件が壊れやすい。
    physics::BVHTree                     tree;
    const std::vector<physics::Triangle> triangles = TriangleStrip(1);
    tree.Build(std::vector<physics::Triangle>(triangles));

    for (int i = 0; i < 64; ++i) {
        const math::Vector3 corner = Rng().NextVector3(-3.0f, 3.0f);
        const math::Vector3 size   = Rng().NextVector3(0.1f, 2.0f);
        const physics::AABB query  = Box(corner, corner + size);

        EXPECT_EQ(TreeQuery(tree, query), BruteForceQuery(triangles, query));
    }
}

} // namespace fbzz::tests
