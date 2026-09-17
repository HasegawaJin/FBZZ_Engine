/// @file    ColliderDebugGeometryTests.cpp
/// @brief   コライダー可視化のワイヤーが、実際の衝突形状と同じ場所に出ることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// このワイヤーはエディターで «当たり判定がどこにあるか» を確かめる唯一の手段。ここがずれると、
/// 見えている線を信じてレベルを組むほど当たり判定が合わなくなる。線の本数だけでなく、
/// 端点が本当に形状の表面に乗っているかを不変条件として固定する。
///
/// メッシュと凸包は «重い形状で描画を殺さない» ことも契約に含む。上限を外すと、
/// 地形コライダーを表示した瞬間にエディターが止まる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

/// 生成側の分割数。テストが «丸いものは閉じた輪として出る» を数で表せるよう写している。
constexpr int kCircleSegments = 24;
constexpr int kArcSegments    = kCircleSegments / 2;
/// 三角メッシュ 1 形状あたりの線の上限。3 の倍数へ切り捨てた本数で頭打ちになる。
constexpr std::size_t kMaxMeshLines = 8192;
constexpr std::size_t kMeshLineCap  = kMaxMeshLines - (kMaxMeshLines % 3);

/// 12 本 = 箱の稜線。球やカプセルの «軸が潰れたときの代替表示» もこの本数になる。
constexpr std::size_t kBoxEdgeCount = 12;

std::vector<math::Vector3> Endpoints(const physics::ColliderDebugGeometry& geometry)
{
    std::vector<math::Vector3> points;
    points.reserve(geometry.lines.size() * 2);
    for (const physics::DebugLine& line : geometry.lines) {
        points.push_back(line.from);
        points.push_back(line.to);
    }
    return points;
}

float DistanceToSegment(const math::Vector3& point,
                        const math::Vector3& start,
                        const math::Vector3& end)
{
    const math::Vector3 segment = end - start;
    const float lenSq = segment.LengthSq();
    const float t = lenSq > 1e-8f
        ? std::clamp(math::Vector3::Dot(point - start, segment) / lenSq, 0.0f, 1.0f)
        : 0.0f;
    return (point - (start + segment * t)).Length();
}

/// 端点が箱の «角» であること。稜線の描画が面の途中から生えていないかを見る。
bool IsCornerOf(const math::Vector3& point, const physics::AABB& bounds)
{
    const auto OnEdge = [](float value, float low, float high) {
        return std::abs(value - low) < testkit::kTolerance ||
               std::abs(value - high) < testkit::kTolerance;
    };
    return OnEdge(point.x, bounds.min.x, bounds.max.x) &&
           OnEdge(point.y, bounds.min.y, bounds.max.y) &&
           OnEdge(point.z, bounds.min.z, bounds.max.z);
}

/// y = 0 に広がる cells x cells の格子メッシュ。三角形数は cells² * 2。
void BuildGrid(int cells,
               std::vector<math::Vector3>& positions,
               std::vector<std::uint32_t>& indices)
{
    const int side = cells + 1;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x)
            positions.push_back({ static_cast<float>(x), 0.0f, static_cast<float>(z) });

    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            const std::uint32_t base = static_cast<std::uint32_t>(z * side + x);
            const std::uint32_t next = base + static_cast<std::uint32_t>(side);
            indices.insert(indices.end(), { base, next, base + 1u });
            indices.insert(indices.end(), { base + 1u, next, next + 1u });
        }
    }
}

std::vector<math::Vector3> CubePoints(float half)
{
    return {
        { -half, -half, -half }, {  half, -half, -half },
        { -half,  half, -half }, {  half,  half, -half },
        { -half, -half,  half }, {  half, -half,  half },
        { -half,  half,  half }, {  half,  half,  half },
    };
}

} // namespace

class ColliderDebugGeometryTest : public testkit::Fixture {};

/// @name 箱

TEST_F(ColliderDebugGeometryTest, DrawsTwelveEdgesOnTheCornersOfAnAABB)
{
    physics::AABBCollider box(math::Vector3(1.0f, 2.0f, 3.0f));
    box.Update(math::Vector3(4.0f, 0.0f, 0.0f), math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(box);

    ASSERT_EQ(geometry.lines.size(), kBoxEdgeCount);
    for (const math::Vector3& point : Endpoints(geometry))
        EXPECT_TRUE(IsCornerOf(point, box.GetAABB()));
}

TEST_F(ColliderDebugGeometryTest, DrawsTheRotatedCornersOfAnOBB)
{
    physics::OBBCollider obb(math::Vector3(1.0f, 1.0f, 1.0f));
    obb.Update(math::Vector3::ZERO,
               math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.25f));

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(obb);

    ASSERT_EQ(geometry.lines.size(), kBoxEdgeCount);
    /// @note OBB を外接 AABB で描くと «回した箱» が軸整合のまま太って見え、
    ///       実際の当たり判定より広い場所を信じてしまう。角そのものを使うことを固定する。
    const std::array<math::Vector3, 8> corners = obb.GetCorners();
    for (const math::Vector3& point : Endpoints(geometry)) {
        const bool matchesACorner = std::any_of(corners.begin(), corners.end(),
            [&](const math::Vector3& corner) {
                return (point - corner).LengthSq() < testkit::kEpsilon;
            });
        EXPECT_TRUE(matchesACorner);
    }
}

/// @name 球

TEST_F(ColliderDebugGeometryTest, DrawsThreeClosedCirclesOnTheSphereSurface)
{
    physics::SphereCollider sphere(2.0f);
    const math::Vector3 center(1.0f, -2.0f, 3.0f);
    sphere.Update(center, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(sphere);

    EXPECT_EQ(geometry.lines.size(), static_cast<std::size_t>(3 * kCircleSegments));
    for (const math::Vector3& point : Endpoints(geometry))
        EXPECT_NEAR((point - center).Length(), sphere.m_radius, testkit::kTolerance);
}

TEST_F(ColliderDebugGeometryTest, DrawsTheThreeSphereCirclesOnOrthogonalPlanes)
{
    physics::SphereCollider sphere(1.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(sphere);

    /// @note 3 本とも同じ平面に出ると «球ではなく円» に見える。各軸に垂直な輪が 1 本ずつ要る。
    const auto CountInPlaneNormalTo = [&](int axis) {
        return std::count_if(geometry.lines.begin(), geometry.lines.end(),
            [&](const physics::DebugLine& line) {
                return std::abs((&line.from.x)[axis]) < testkit::kTolerance &&
                       std::abs((&line.to.x)[axis]) < testkit::kTolerance;
            });
    };

    EXPECT_EQ(CountInPlaneNormalTo(0), kCircleSegments);
    EXPECT_EQ(CountInPlaneNormalTo(1), kCircleSegments);
    EXPECT_EQ(CountInPlaneNormalTo(2), kCircleSegments);
}

/// @name カプセル

TEST_F(ColliderDebugGeometryTest, KeepsEveryCapsuleEndpointOnTheCapsuleSurface)
{
    physics::CapsuleCollider capsule(0.5f, 1.5f);
    capsule.Update(math::Vector3(0.0f, 2.0f, 0.0f),
                   math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::PI * 0.5f));

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(capsule);

    /// @note 2 本の輪 + 4 本の縦アーク + 4 本の側線。回転させても «中心線から半径ちょうど» が崩れない。
    EXPECT_EQ(geometry.lines.size(),
              static_cast<std::size_t>(2 * kCircleSegments + 4 * kArcSegments + 4));
    for (const math::Vector3& point : Endpoints(geometry)) {
        EXPECT_NEAR(DistanceToSegment(point, capsule.GetSegmentStart(), capsule.GetSegmentEnd()),
                    capsule.m_radius,
                    testkit::kLooseTolerance);
    }
}

TEST_F(ColliderDebugGeometryTest, FallsBackToABoxWhenTheCapsuleAxisIsDegenerate)
{
    physics::CapsuleCollider capsule(0.5f, 0.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    /// @note 中心線が 1 点に潰れると直交基底が作れない。正規化に突っ込むと NaN の線が出るため、
    ///       箱表示へ落とすことを契約にする。
    EXPECT_EQ(physics::BuildColliderDebugGeometry(capsule).lines.size(), kBoxEdgeCount);
}

/// @name 円柱

TEST_F(ColliderDebugGeometryTest, KeepsEveryCylinderEndpointOnTheCapRims)
{
    physics::CylinderCollider cylinder(0.75f, 2.0f);
    cylinder.Update(math::Vector3(3.0f, 0.0f, 0.0f), math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(cylinder);

    /// @note 2 本の輪 + 4 本の側線。カプセルと違い縁が鋭いので、端点はすべて上下の円周上に乗る。
    EXPECT_EQ(geometry.lines.size(), static_cast<std::size_t>(2 * kCircleSegments + 4));
    for (const math::Vector3& point : Endpoints(geometry)) {
        const math::Vector3 offset = point - cylinder.GetCenter();
        const float axial = math::Vector3::Dot(offset, cylinder.GetAxis());
        const float radial = (offset - cylinder.GetAxis() * axial).Length();

        EXPECT_NEAR(std::abs(axial), cylinder.m_halfHeight, testkit::kTolerance);
        EXPECT_NEAR(radial, cylinder.m_radius, testkit::kTolerance);
    }
}

TEST_F(ColliderDebugGeometryTest, FallsBackToABoxWhenTheCylinderAxisIsDegenerate)
{
    physics::CylinderCollider cylinder(0.75f, 0.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_EQ(physics::BuildColliderDebugGeometry(cylinder).lines.size(), kBoxEdgeCount);
}

/// @name 三角メッシュ / ハイトフィールド

TEST_F(ColliderDebugGeometryTest, DrawsThreeEdgesPerTriangleOfASmallMesh)
{
    std::vector<math::Vector3> positions;
    std::vector<std::uint32_t> indices;
    BuildGrid(1, positions, indices);
    physics::TriangleMeshCollider mesh(positions, indices);
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(mesh);

    ASSERT_EQ(mesh.GetBVH().triangles.size(), 2u);
    EXPECT_EQ(geometry.lines.size(), mesh.GetBVH().triangles.size() * 3);
}

TEST_F(ColliderDebugGeometryTest, DrawsNothingForAMeshWithoutTriangles)
{
    physics::TriangleMeshCollider mesh({}, {});
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_TRUE(physics::BuildColliderDebugGeometry(mesh).lines.empty());
}

TEST_F(ColliderDebugGeometryTest, SamplesALargeMeshDownToTheLineBudget)
{
    std::vector<math::Vector3> positions;
    std::vector<std::uint32_t> indices;
    /// @note 3200 三角形 = 9600 本ぶん。上限を超える。
    BuildGrid(40, positions, indices);
    physics::TriangleMeshCollider mesh(positions, indices);
    mesh.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(mesh);

    /// @note 全三角形を描くとエディターが止まる。間引いて上限ちょうどで頭打ちにする。
    ASSERT_GT(mesh.GetBVH().triangles.size() * 3, kMeshLineCap);
    EXPECT_EQ(geometry.lines.size(), kMeshLineCap);
}

TEST_F(ColliderDebugGeometryTest, DrawsThreeEdgesPerTriangleOfAHeightField)
{
    const std::vector<float> heights(9, 0.0f);
    physics::HeightFieldCollider field(heights, 3, 3, 1.0f, 1.0f);
    field.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(field);

    ASSERT_FALSE(field.GetBVH().triangles.empty());
    EXPECT_EQ(geometry.lines.size(), field.GetBVH().triangles.size() * 3);
}

/// @name 凸包

TEST_F(ColliderDebugGeometryTest, DrawsEachConvexHullEdgeOnlyOnce)
{
    physics::ConvexHullCollider hull(CubePoints(1.0f));
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(hull);

    /// @note 隣り合う面は稜線を共有する。素直に面ごとに 3 辺を出すと全稜線が二重に描かれ、
    ///       線の本数が形状の複雑さの 2 倍で増える。
    const std::vector<math::Vector3>& vertices = hull.GetWorldVertices();
    ASSERT_FALSE(geometry.lines.empty());

    const auto IndexOf = [&](const math::Vector3& point) {
        for (std::size_t i = 0; i < vertices.size(); ++i)
            if ((point - vertices[i]).LengthSq() < testkit::kEpsilon) return static_cast<int>(i);
        return -1;
    };

    std::set<std::pair<int, int>> seen;
    for (const physics::DebugLine& line : geometry.lines) {
        const int a = IndexOf(line.from);
        const int b = IndexOf(line.to);
        ASSERT_GE(a, 0);
        ASSERT_GE(b, 0);
        EXPECT_TRUE(seen.insert({ std::min(a, b), std::max(a, b) }).second);
    }
}

TEST_F(ColliderDebugGeometryTest, DrawsNothingForAHullWithoutVertices)
{
    physics::ConvexHullCollider hull({});
    hull.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    EXPECT_TRUE(physics::BuildColliderDebugGeometry(hull).lines.empty());
}

} // namespace fbzz::tests
