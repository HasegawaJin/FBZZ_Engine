/// @file    ConvexHullTests.cpp
/// @brief   点群から組む凸包 (Quickhull) と、そのサポート関数・ワールド変換を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 凸包は GJK / EPA の «形» そのもの。頂点を取りこぼすとその方向だけ当たり判定が痩せ、
/// 内部の点を残すとサポート関数が誤った最遠点を返す。どちらも «たまにめり込む» という
/// 形でしか出ないので、包含関係と支持点の性質を不変条件として固定する。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/ConvexHullCollider.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

std::vector<math::Vector3> CubePoints(float half = 1.0f)
{
    return {
        { -half, -half, -half }, {  half, -half, -half },
        { -half,  half, -half }, {  half,  half, -half },
        { -half, -half,  half }, {  half, -half,  half },
        { -half,  half,  half }, {  half,  half,  half },
    };
}

/// dir 方向で «どの入力点よりも遠い» ことを確かめる。凸包の支持点の定義そのもの。
void ExpectSupportsAtLeast(const physics::ConvexHullCollider& hull,
                           const std::vector<math::Vector3>&  points,
                           const math::Vector3&               dir)
{
    const float best = math::Vector3::Dot(hull.SupportPoint(dir), dir);
    for (const math::Vector3& p : points)
        EXPECT_LE(math::Vector3::Dot(p, dir), best + testkit::kLooseTolerance);
}

} // namespace

class ConvexHullTest : public testkit::Fixture {};

// --- 構築 -------------------------------------------------------------------

TEST_F(ConvexHullTest, KeepsEveryCornerOfACube)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    // 立方体は 8 頂点すべてが凸包の頂点。1 つでも落ちると、その角だけ判定が痩せる。
    EXPECT_EQ(hull.GetWorldVertices().size(), 8u);
}

TEST_F(ConvexHullTest, DropsPointsInsideTheHull)
{
    std::vector<math::Vector3> points = CubePoints();
    points.push_back(math::Vector3::ZERO);            // 中心
    points.push_back({ 0.2f, -0.1f, 0.3f });          // 内部の適当な点

    physics::ConvexHullCollider hull(std::move(points));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    // 内部の点を残すと頂点数だけ増え、サポート点探索が毎回それを舐める。
    EXPECT_EQ(hull.GetWorldVertices().size(), 8u);
}

TEST_F(ConvexHullTest, IgnoresDuplicatedPoints)
{
    std::vector<math::Vector3> points = CubePoints();
    const std::vector<math::Vector3> again = CubePoints();
    points.insert(points.end(), again.begin(), again.end());

    physics::ConvexHullCollider hull(std::move(points));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(hull.GetWorldVertices().size(), 8u);
}

TEST_F(ConvexHullTest, BuildsFacesThatReferenceRealVertices)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const std::size_t vertexCount = hull.GetWorldVertices().size();
    ASSERT_FALSE(hull.GetFaces().empty());

    for (const std::array<std::uint32_t, 3>& face : hull.GetFaces()) {
        EXPECT_LT(face[0], vertexCount);
        EXPECT_LT(face[1], vertexCount);
        EXPECT_LT(face[2], vertexCount);
        // 縮退した面 (同じ頂点を 2 度使う) は法線を作れない。
        EXPECT_NE(face[0], face[1]);
        EXPECT_NE(face[1], face[2]);
        EXPECT_NE(face[2], face[0]);
    }
}

// --- サポート関数 -----------------------------------------------------------

TEST_F(ConvexHullTest, SupportPointIsTheFarthestCornerAlongTheAxes)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_VEC3_NEAR(hull.SupportPoint({ 1.0f, 1.0f, 1.0f }),
                     math::Vector3(1.0f, 1.0f, 1.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(hull.SupportPoint({ -1.0f, -1.0f, -1.0f }),
                     math::Vector3(-1.0f, -1.0f, -1.0f), testkit::kTolerance);
}

TEST_F(ConvexHullTest, SupportPointIsNeverBeatenByAnInputPoint)
{
    const std::vector<math::Vector3> points = {
        { 0.0f, 2.0f, 0.0f },  { 1.5f, -0.5f, 1.0f }, { -1.0f, -0.5f, 1.2f },
        { 0.3f, -1.8f, -0.7f }, { -1.4f, 0.6f, -1.1f }, { 0.9f, 1.1f, -1.3f },
        { 0.0f, 0.0f, 0.0f },
    };
    physics::ConvexHullCollider hull(points);
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    for (int i = 0; i < 64; ++i) ExpectSupportsAtLeast(hull, points, Rng().NextUnitVector3());
}

TEST_F(ConvexHullTest, SupportPointFollowsTheWorldRotation)
{
    physics::ConvexHullCollider hull(CubePoints());
    const math::Quaternion turn =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    hull.Update(math::Vector3::ZERO, turn);

    // 回した後の +X 方向の支持点は、回す前の «+X へ回ってくる角»。
    EXPECT_NEAR(math::Vector3::Dot(hull.SupportPoint(math::Vector3::RIGHT), math::Vector3::RIGHT),
                1.0f, testkit::kLooseTolerance);
}

TEST_F(ConvexHullTest, SupportPointFollowsTheWorldPosition)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.Update({ 10.0f, 0.0f, 0.0f }, math::Quaternion::Identity());

    EXPECT_VEC3_NEAR(hull.SupportPoint({ 1.0f, 1.0f, 1.0f }),
                     math::Vector3(11.0f, 1.0f, 1.0f), testkit::kTolerance);
}

TEST_F(ConvexHullTest, ScaleStretchesTheHull)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.UpdateWithScale(math::Vector3::ZERO, math::Quaternion::Identity(),
                         { 2.0f, 1.0f, 1.0f });

    EXPECT_VEC3_NEAR(hull.SupportPoint({ 1.0f, 1.0f, 1.0f }),
                     math::Vector3(2.0f, 1.0f, 1.0f), testkit::kTolerance);
}

// --- 境界 -------------------------------------------------------------------

TEST_F(ConvexHullTest, BoundsEncloseEveryHullVertex)
{
    physics::ConvexHullCollider hull(CubePoints(2.0f));
    hull.Update({ 1.0f, -2.0f, 3.0f },
                math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(30.0f)));

    const physics::AABB bounds = hull.GetAABB();
    for (const math::Vector3& v : hull.GetWorldVertices()) {
        EXPECT_GE(v.x, bounds.min.x - testkit::kLooseTolerance);
        EXPECT_GE(v.y, bounds.min.y - testkit::kLooseTolerance);
        EXPECT_GE(v.z, bounds.min.z - testkit::kLooseTolerance);
        EXPECT_LE(v.x, bounds.max.x + testkit::kLooseTolerance);
        EXPECT_LE(v.y, bounds.max.y + testkit::kLooseTolerance);
        EXPECT_LE(v.z, bounds.max.z + testkit::kLooseTolerance);
    }
}

TEST_F(ConvexHullTest, ReportsItsTypeAndAVolume)
{
    physics::ConvexHullCollider hull(CubePoints());
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(hull.GetType(), physics::ColliderType::CONVEX_HULL);
    // 凸包は厳密な体積を持たず外接箱で近似する契約 (Collider::ComputeVolume)。
    EXPECT_NEAR(hull.ComputeVolume(), 8.0f, testkit::kLooseTolerance);
}

// --- 縮退した入力 -----------------------------------------------------------

TEST_F(ConvexHullTest, SurvivesAnEmptyPointCloud)
{
    // コンポーネントを組み立てる途中で «まだ点が無い» 状態は普通に通る。
    physics::ConvexHullCollider hull(std::vector<math::Vector3>{});
    hull.Update({ 5.0f, 0.0f, 0.0f }, math::Quaternion::Identity());

    EXPECT_TRUE(hull.GetWorldVertices().empty());
    EXPECT_VEC3_NEAR(hull.SupportPoint(math::Vector3::RIGHT), math::Vector3(5.0f, 0.0f, 0.0f),
                     testkit::kTolerance);
}

TEST_F(ConvexHullTest, SurvivesPointsThatAllLieOnOnePlane)
{
    // 板ポリのメッシュから作るとこうなる。体積が無くても落ちないこと。
    physics::ConvexHullCollider hull(std::vector<math::Vector3>{
        { -1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, -1.0f },
        { -1.0f, 0.0f,  1.0f }, { 1.0f, 0.0f,  1.0f },
    });
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_NEAR(math::Vector3::Dot(hull.SupportPoint(math::Vector3::RIGHT), math::Vector3::RIGHT),
                1.0f, testkit::kLooseTolerance);
}

TEST_F(ConvexHullTest, SurvivesPointsOnASingleLine)
{
    physics::ConvexHullCollider hull(std::vector<math::Vector3>{
        { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
    });
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_NEAR(math::Vector3::Dot(hull.SupportPoint(math::Vector3::UP), math::Vector3::UP),
                1.0f, testkit::kLooseTolerance);
}

// 4 点以下は «そのまま包み» の近道を通るので、Quickhull 本体は一度も走らない。
// 退化した点群が壊すのは初期四面体を組む側なので、そこへ届く点数で当てる。
TEST_F(ConvexHullTest, SurvivesManyPointsOnASingleLine)
{
    std::vector<math::Vector3> line;
    for (int i = 0; i < 32; ++i)
        line.push_back({ static_cast<float>(i) * 0.1f - 1.5f, 0.0f, 0.0f });

    physics::ConvexHullCollider hull(line);
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    // 面は張れない。それでも «その向きで一番遠い点» は答えられなければならない。
    EXPECT_TRUE(hull.GetFaces().empty());
    for (int i = 0; i < 32; ++i)
        ExpectSupportsAtLeast(hull, line, Rng().NextUnitVector3());
}

TEST_F(ConvexHullTest, SurvivesManyPointsOnOnePlane)
{
    std::vector<math::Vector3> plane;
    for (int i = 0; i < 6; ++i)
        for (int k = 0; k < 6; ++k)
            plane.push_back({ static_cast<float>(i) * 0.4f - 1.0f, 0.0f,
                              static_cast<float>(k) * 0.4f - 1.0f });

    physics::ConvexHullCollider hull(plane);
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_TRUE(hull.GetFaces().empty());
    for (int i = 0; i < 32; ++i)
        ExpectSupportsAtLeast(hull, plane, Rng().NextUnitVector3());
}

TEST_F(ConvexHullTest, SurvivesManyCopiesOfTheSamePoint)
{
    const std::vector<math::Vector3> cloud(32, math::Vector3(0.7f, -0.3f, 0.2f));

    physics::ConvexHullCollider hull(cloud);
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_VEC3_NEAR(hull.SupportPoint(math::Vector3::RIGHT),
                     math::Vector3(0.7f, -0.3f, 0.2f), testkit::kTolerance);
}

TEST_F(ConvexHullTest, CapsTheVertexCount)
{
    // サポート点探索は頂点数に比例する。上限を外すと、細かいメッシュを 1 つ
    // 割り当てただけで NarrowPhase が跳ねる。
    std::vector<math::Vector3> cloud;
    for (int i = 0; i < 500; ++i) cloud.push_back(Rng().NextUnitVector3());

    physics::ConvexHullCollider hull(std::move(cloud));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_LE(static_cast<int>(hull.GetWorldVertices().size()),
              physics::ConvexHullCollider::MAX_HULL_VERTS);
}

TEST_F(ConvexHullTest, KeepsTheOutermostPointsWhenTheCloudExceedsTheCap)
{
    // WHY 外周の点を «後ろ» に置くか: 上限を入力の先頭から掛ける実装だと、この 8 点が
    //     丸ごと落ちて «実際の形より小さいコライダー» が黙って出来上がる。頂点バッファの
    //     並び順は形とは無関係なので、実データのメッシュでも普通に起きる。
    //     上の CapsTheVertexCount は «球面上の 500 点» なので、先頭 64 点でもだいたい球に
    //     なってしまい、この不具合を素通しする。包含関係まで見ないと固定できない。
    std::vector<math::Vector3> cloud;
    for (int i = 0; i < 120; ++i) cloud.push_back(Rng().NextUnitVector3() * 0.1f);
    for (const math::Vector3& corner : CubePoints(5.0f)) cloud.push_back(corner);
    ASSERT_GT(static_cast<int>(cloud.size()), physics::ConvexHullCollider::MAX_HULL_VERTS);

    physics::ConvexHullCollider hull(cloud);
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const math::Vector3 dirs[] = {
        math::Vector3::RIGHT,    -math::Vector3::RIGHT,
        math::Vector3::UP,       -math::Vector3::UP,
        math::Vector3::FORWARD,  -math::Vector3::FORWARD,
        math::Vector3( 1.0f,  1.0f,  1.0f).Normalized(),
        math::Vector3(-1.0f,  1.0f, -1.0f).Normalized(),
        math::Vector3( 1.0f, -1.0f,  1.0f).Normalized(),
    };
    for (const math::Vector3& dir : dirs)
        ExpectSupportsAtLeast(hull, cloud, dir);

    EXPECT_LE(static_cast<int>(hull.GetWorldVertices().size()),
              physics::ConvexHullCollider::MAX_HULL_VERTS);
}

TEST_F(ConvexHullTest, FacesReferenceRealVerticesWhenTheCloudExceedsTheCap)
{
    // WHY 上限に掛かる大きさで見るか: 頂点だけ切り詰めて面のインデックスを放置すると
    //     ここが範囲外になる。World.cpp の RayConvexHull は範囲チェックなしで
    //     verts[face[0]] を引くので、そのまま範囲外読み取りになる。
    std::vector<math::Vector3> cloud;
    for (int i = 0; i < 500; ++i) cloud.push_back(Rng().NextUnitVector3());

    physics::ConvexHullCollider hull(std::move(cloud));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const std::size_t vertexCount = hull.GetWorldVertices().size();
    ASSERT_FALSE(hull.GetFaces().empty());

    for (const std::array<std::uint32_t, 3>& face : hull.GetFaces()) {
        EXPECT_LT(face[0], vertexCount);
        EXPECT_LT(face[1], vertexCount);
        EXPECT_LT(face[2], vertexCount);
    }
}

TEST_F(ConvexHullTest, OrientsTetrahedronFacesOutwardForEitherWinding)
{
    // WHY 2 通り試すか: 4 点だけの経路は入力の並び順をそのまま面にしていた。
    //     どちら手の四面体かで法線が 4 枚とも内向きになるので、片方の順序しか
    //     試さないと «たまたま通る» ことがある。
    const std::vector<math::Vector3> base = {
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    const std::vector<math::Vector3> mirrored = {
        { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };

    auto expectOutward = [](const std::vector<math::Vector3>& points) {
        physics::ConvexHullCollider hull(points);
        hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

        const std::vector<math::Vector3>& verts = hull.GetWorldVertices();
        ASSERT_EQ(verts.size(), 4u);
        ASSERT_EQ(hull.GetFaces().size(), 4u);

        const math::Vector3 centroid = (verts[0] + verts[1] + verts[2] + verts[3]) * 0.25f;
        for (const std::array<std::uint32_t, 3>& face : hull.GetFaces()) {
            const math::Vector3 n = math::Vector3::Cross(verts[face[1]] - verts[face[0]],
                                                         verts[face[2]] - verts[face[0]]);
            // 外向きなら、面の平面から見て centroid は負の側にある。
            EXPECT_LT(math::Vector3::Dot(n, centroid - verts[face[0]]), 0.0f);
        }
    };

    expectOutward(base);
    expectOutward(mirrored);
}

} // namespace fbzz::tests
