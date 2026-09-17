/// @file    GJKTests.cpp
/// @brief   GJK の交差判定と、EPA へ渡す単体 (Simplex) の受け渡し契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// GJK は «当たっているのに当たっていないと言う» 形で壊れると、すり抜けとして現れる。
/// 半径の和ちょうど付近を挟んで両側を確かめることと、交差時に必ず四面体を残すこと
/// (EPA は 4 頂点でないと即座に無効を返す) を特に固定する。
#include <TestKit/TestKit.hpp>

#include <TestKit/Physics/SupportShapes.hpp>

#include <Physics/GJK.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::tests {

namespace {

physics::GJKResult Intersect(const testkit::SupportSphere& a, const testkit::SupportSphere& b)
{
    return physics::GJK_Intersect(&a, testkit::kSphereSupport, &b, testkit::kSphereSupport);
}

physics::GJKResult Intersect(const testkit::SupportBox& a, const testkit::SupportBox& b)
{
    return physics::GJK_Intersect(&a, testkit::kBoxSupport, &b, testkit::kBoxSupport);
}

physics::GJKResult Distance(const testkit::SupportBox& a, const testkit::SupportBox& b)
{
    return physics::GJK_Distance(&a, testkit::kBoxSupport, &b, testkit::kBoxSupport);
}

/// 軸整合の箱どうしの隙間は閉じた式で出る。軸ごとのはみ出し量を並べたベクトルの長さ。
/// 重なっている軸は 0 になるので、面・辺・角のどの配置でも同じ 1 本の式で書ける。
float AnalyticGap(const testkit::SupportBox& a, const testkit::SupportBox& b)
{
    const math::Vector3 delta = b.center - a.center;
    const math::Vector3 sum   = a.halfExtents + b.halfExtents;
    const math::Vector3 gap{ std::max(0.0f, std::abs(delta.x) - sum.x),
                             std::max(0.0f, std::abs(delta.y) - sum.y),
                             std::max(0.0f, std::abs(delta.z) - sum.z) };
    return gap.Length();
}

} // namespace

class GJKTest : public testkit::Fixture {};

/// @name 球どうし

TEST_F(GJKTest, ReportsIntersectionForDeeplyOverlappingSpheres)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.5f, 0.0f, 0.0f), 1.0f};

    EXPECT_TRUE(Intersect(a, b).intersects);
}

TEST_F(GJKTest, ReportsNoIntersectionForClearlySeparatedSpheres)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(10.0f, 0.0f, 0.0f), 1.0f};

    EXPECT_FALSE(Intersect(a, b).intersects);
}

TEST_F(GJKTest, SeparatesJustInsideAndJustOutsideTheSumOfRadii)
{
    /// @note 判定の境目は «中心間距離 == 半径の和»。両側 1mm を挟んで挙動が切り替わることを見る。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere justInside{math::Vector3(2.0f - 0.001f, 0.0f, 0.0f), 1.0f};
    const testkit::SupportSphere justOutside{math::Vector3(2.0f + 0.001f, 0.0f, 0.0f), 1.0f};

    EXPECT_TRUE(Intersect(a, justInside).intersects);
    EXPECT_FALSE(Intersect(a, justOutside).intersects);
}

TEST_F(GJKTest, ReportsIntersectionForCoincidentSpheres)
{
    /// @note 中心が完全に一致すると Minkowski 差の初期方向がゼロになる縮退経路を踏む。
    const testkit::SupportSphere a{math::Vector3(3.0f, -2.0f, 1.0f), 1.0f};
    const testkit::SupportSphere b = a;

    EXPECT_TRUE(Intersect(a, b).intersects);
}

/// @name 箱どうし

TEST_F(GJKTest, ReportsIntersectionForOverlappingBoxes)
{
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(1.5f, 0.0f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};

    EXPECT_TRUE(Intersect(a, b).intersects);
}

TEST_F(GJKTest, ReportsNoIntersectionWhenBoxesAreApartOnASingleAxis)
{
    /// @note 分離軸が 1 本でもあれば非交差。X と Z は重なっているが Y だけ離れている配置。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(0.0f, 2.5f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};

    EXPECT_FALSE(Intersect(a, b).intersects);
}

TEST_F(GJKTest, ReportsIntersectionWhenOnlyCornersOverlap)
{
    /// @note 面ではなく角どうしがわずかに食い込む配置。単体が三角形止まりになりやすい。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(1.9f, 1.9f, 1.9f), math::Vector3(1.0f, 1.0f, 1.0f)};

    EXPECT_TRUE(Intersect(a, b).intersects);
}

/// @name 異なる形状の組み合わせ

TEST_F(GJKTest, DetectsIntersectionBetweenABoxAndASphere)
{
    const testkit::SupportBox    box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportSphere sphere{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::GJKResult result =
        physics::GJK_Intersect(&box, testkit::kBoxSupport, &sphere, testkit::kSphereSupport);

    EXPECT_TRUE(result.intersects);
}

TEST_F(GJKTest, DetectsAPointInsideABox)
{
    const testkit::SupportBox   box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportPoint inside{math::Vector3(0.5f, 0.5f, 0.5f)};

    const physics::GJKResult result =
        physics::GJK_Intersect(&box, testkit::kBoxSupport, &inside, testkit::kPointSupport);

    EXPECT_TRUE(result.intersects);
}

TEST_F(GJKTest, DetectsAPointOutsideABox)
{
    const testkit::SupportBox   box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportPoint outside{math::Vector3(5.0f, 0.0f, 0.0f)};

    const physics::GJKResult result =
        physics::GJK_Intersect(&box, testkit::kBoxSupport, &outside, testkit::kPointSupport);

    EXPECT_FALSE(result.intersects);
}

/// @name EPA への受け渡し

TEST_F(GJKTest, LeavesAFullTetrahedronWhenShapesIntersect)
{
    /// @note EPA_GetContactInfo は size < 4 の単体を受け取ると即座に valid=false を返す。
    ///       つまり «交差時は必ず四面体» が GJK 側の契約になる。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.5f, 0.2f, -0.3f), 1.0f};

    const physics::GJKResult result = Intersect(a, b);

    ASSERT_TRUE(result.intersects);
    EXPECT_EQ(result.simplex.size, 4);
}

TEST_F(GJKTest, LeavesAFullTetrahedronForShallowBoxOverlap)
{
    /// @note 浅い面接触は単体が三角形で止まりやすく、四面体への拡張が効いているかが出る。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(0.0f, 1.99f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};

    const physics::GJKResult result = Intersect(a, b);

    ASSERT_TRUE(result.intersects);
    EXPECT_EQ(result.simplex.size, 4);
}

TEST_F(GJKTest, SimplexVerticesReconstructTheMinkowskiDifference)
{
    /// @note point == suppA - suppB が崩れると、EPA が復元する接触点が形状の外に出る。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.7f, 0.4f, 0.1f), 1.0f};

    const physics::GJKResult result = Intersect(a, b);

    ASSERT_TRUE(result.intersects);
    for (int i = 0; i < result.simplex.size; ++i) {
        const physics::Simplex::Vertex& v = result.simplex.verts[i];
        EXPECT_VEC3_NEAR(v.point, v.suppA - v.suppB, testkit::kTolerance) << "頂点 " << i;
    }
}

/// @name 対称性

TEST_F(GJKTest, GivesTheSameAnswerRegardlessOfArgumentOrder)
{
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 offset = Rng().NextVector3(-3.0f, 3.0f);

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{offset, 1.0f};

        EXPECT_EQ(Intersect(a, b).intersects, Intersect(b, a).intersects)
            << "offset = " << ::testing::PrintToString(offset);
    }
}

TEST_F(GJKTest, AgreesWithTheAnalyticAnswerForRandomSpherePairs)
{
    /// @note 球どうしなら «中心間距離 < 半径の和» が正解。境界ぎわは判定が割れるので、
    ///       判定の «外側» に十分な余白を取った組だけを見る。
    for (int i = 0; i < 128; ++i) {
        const math::Vector3 offset = Rng().NextVector3(-4.0f, 4.0f);
        const float         sum    = 2.0f;
        const float         gap    = offset.Length() - sum;
        if (std::fabs(gap) < 0.01f) continue;

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{offset, 1.0f};

        EXPECT_EQ(Intersect(a, b).intersects, gap < 0.0f)
            << "offset = " << ::testing::PrintToString(offset) << " gap = " << gap;
    }
}

/// @name 隙間と最近傍点 (GJK_Distance)

class GJKDistanceTest : public testkit::Fixture {};

TEST_F(GJKDistanceTest, ReportsTheGapBetweenSeparatedSpheres)
{
    /// @note 中心間 5m、半径 1m どうし。隙間はちょうど 3m。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(5.0f, 0.0f, 0.0f), 1.0f};

    const physics::GJKResult result = physics::GJK_Distance(
        &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, 3.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.closestA, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.closestB, math::Vector3(4.0f, 0.0f, 0.0f), testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, ClosestPointsLieOnTheSurfaceOfEachSphere)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(3.0f, 2.0f, -1.5f), 0.75f};

    const physics::GJKResult result = physics::GJK_Distance(
        &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR((result.closestA - a.center).Length(), a.radius, testkit::kLooseTolerance);
    EXPECT_NEAR((result.closestB - b.center).Length(), b.radius, testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, ClosestPointsAreSeparatedByTheReportedDistance)
{
    /// @note distance と最近傍点が別々に計算されて食い違う、が一番ありがちな壊れ方。
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 direction = Rng().NextUnitVector3();
        const float         span      = Rng().NextFloat(2.5f, 12.0f);

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{direction * span, 1.0f};

        const physics::GJKResult result = physics::GJK_Distance(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

        ASSERT_FALSE(result.intersects);
        EXPECT_NEAR((result.closestA - result.closestB).Length(), result.distance,
                    testkit::kLooseTolerance);
    }
}

TEST_F(GJKDistanceTest, DistanceMatchesTheAnalyticGapForRandomSpherePairs)
{
    for (int i = 0; i < 128; ++i) {
        const math::Vector3 direction = Rng().NextUnitVector3();
        const float         span      = Rng().NextFloat(2.2f, 15.0f);

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{direction * span, 1.0f};

        const physics::GJKResult result = physics::GJK_Distance(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

        ASSERT_FALSE(result.intersects);
        EXPECT_NEAR(result.distance, span - 2.0f, testkit::kLooseTolerance) << "span = " << span;
    }
}

TEST_F(GJKDistanceTest, ReportsTheGapBetweenSeparatedBoxes)
{
    /// @note 面どうしが向かい合う配置。最近傍«点»は面全体で一意に決まらないが、
    ///       隙間と、法線方向 (X) の座標は一意に決まる。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(5.0f, 0.0f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};

    const physics::GJKResult result =
        physics::GJK_Distance(&a, testkit::kBoxSupport, &b, testkit::kBoxSupport);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, 3.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(result.closestA.x, 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(result.closestB.x, 4.0f, testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, ReportsTheGapFromABoxFaceToAPoint)
{
    const testkit::SupportBox   box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportPoint point{math::Vector3(3.0f, 0.0f, 0.0f)};

    const physics::GJKResult result =
        physics::GJK_Distance(&box, testkit::kBoxSupport, &point, testkit::kPointSupport);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, 2.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.closestA, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.closestB, point.position, testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, ReportsTheGapFromABoxCornerToAPoint)
{
    /// @note 対角線上の点。最近傍は面ではなく «角» — 箱の支持点そのものなので、
    ///       単体は 1 頂点のまま収束する。三角形まで育つ配置は下の «面の内側» が受け持つ。
    const testkit::SupportBox   box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportPoint point{math::Vector3(3.0f, 3.0f, 3.0f)};

    const physics::GJKResult result =
        physics::GJK_Distance(&box, testkit::kBoxSupport, &point, testkit::kPointSupport);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, std::sqrt(12.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.closestA, math::Vector3(1.0f, 1.0f, 1.0f), testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, GivesTheSameGapRegardlessOfArgumentOrder)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(4.0f, 1.0f, -2.0f), 1.5f};

    const physics::GJKResult forward = physics::GJK_Distance(
        &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);
    const physics::GJKResult swapped = physics::GJK_Distance(
        &b, testkit::kSphereSupport, &a, testkit::kSphereSupport);

    /// @note 曲面上の «点» は距離より一桁粗い精度でしか決まらない。半径 R の球で距離が e ずれると
    ///       接点は接線方向に sqrt(2*R*e) 動くため、距離の許容 1e-4 に対し点は 0.014 程度まで動きうる。
    constexpr float kSurfacePointTolerance = 0.02f;

    ASSERT_FALSE(forward.intersects);
    ASSERT_FALSE(swapped.intersects);
    EXPECT_NEAR(forward.distance, swapped.distance, testkit::kLooseTolerance);
    /// @note 引数を入れ替えたら «A 上の点» と «B 上の点» も入れ替わる。
    EXPECT_VEC3_NEAR(swapped.closestA, forward.closestB, kSurfacePointTolerance);
    EXPECT_VEC3_NEAR(swapped.closestB, forward.closestA, kSurfacePointTolerance);
}

TEST_F(GJKDistanceTest, ReportsIntersectionWithoutMeasuringTheGap)
{
    /// @note 重なっている場合、隙間は «無い»。貫通量は EPA の仕事なので 0 のまま返す。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.5f, 0.0f, 0.0f), 1.0f};

    const physics::GJKResult result = physics::GJK_Distance(
        &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

    EXPECT_TRUE(result.intersects);
    EXPECT_NEAR(result.distance, 0.0f, testkit::kTolerance);
}

TEST_F(GJKDistanceTest, AgreesWithGJKIntersectOnWhetherShapesTouch)
{
    /// @note 2 つの入口が «当たっている / いない» で食い違うと、当たり判定と距離クエリで
    ///       別々の世界が見えることになる。境界ぎわを避けて突き合わせる。
    for (int i = 0; i < 128; ++i) {
        const math::Vector3 offset = Rng().NextVector3(-4.0f, 4.0f);
        const float         gap    = offset.Length() - 2.0f;
        if (std::fabs(gap) < 0.02f) continue;

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{offset, 1.0f};

        const bool byIntersect = physics::GJK_Intersect(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport).intersects;
        const bool byDistance = physics::GJK_Distance(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport).intersects;

        EXPECT_EQ(byIntersect, byDistance)
            << "offset = " << ::testing::PrintToString(offset) << " gap = " << gap;
    }
}

TEST_F(GJKDistanceTest, DistanceGrowsMonotonicallyAsShapesSeparate)
{
    /// @note 少しずつ離していったときに距離が単調に増えること。飛びがあれば、
    ///       単体の削り方か収束判定のどちらかが配置によって崩れている。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};

    float previous = -1.0f;
    for (int step = 0; step < 40; ++step) {
        const float span = 2.5f + static_cast<float>(step) * 0.25f;

        const testkit::SupportSphere b{math::Vector3(span * 0.6f, span * 0.8f, 0.0f), 1.0f};
        const physics::GJKResult     result = physics::GJK_Distance(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);

        ASSERT_FALSE(result.intersects) << "span = " << span;
        EXPECT_GT(result.distance, previous) << "span = " << span;
        previous = result.distance;
    }
}

/// @name 最近傍が «頂点そのもの» ではない配置
/// 箱の支持関数は必ず 8 隅のどれかを返す。最近傍点が隅と一致する配置では、単体は
/// 1 頂点のまま答えに届いてしまう。最近傍が面や辺の «内側» に落ちる配置でだけ、
/// GJK は複数の隅を集めて重心座標で内分する経路へ入る。
/// 既存の面どうしのテストは Y も Z も揃っているせいで、たまたま最初の支持点が
/// 答えそのものになる。ここでは軸をずらして、その近道を塞ぐ。

TEST_F(GJKDistanceTest, ReportsTheGapWhenTheClosestPointFallsInsideAFace)
{
    /// @note X 方向へ離し、Y と Z を «隅では釣り合わない» 量だけずらす。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(5.0f, 0.3f, 0.4f), math::Vector3(1.0f, 1.0f, 1.0f)};

    const physics::GJKResult result = Distance(a, b);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, AnalyticGap(a, b), testkit::kLooseTolerance);
    /// @note 隙間は X 方向だけ。ずらした Y / Z は «面の中で» 吸収され、距離には出ない。
    EXPECT_NEAR(result.closestA.x, 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(result.closestB.x, 4.0f, testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, ReportsTheGapWhenTheClosestFeatureIsAnEdge)
{
    /// @note 2 軸で離し、残る 1 軸だけ重なりを残す。最近傍は «辺の内側の 1 点»。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(4.0f, 4.0f, 0.3f), math::Vector3(1.0f, 1.0f, 1.0f)};

    const physics::GJKResult result = Distance(a, b);

    ASSERT_FALSE(result.intersects);
    EXPECT_NEAR(result.distance, AnalyticGap(a, b), testkit::kLooseTolerance);
}

TEST_F(GJKDistanceTest, AgreesWithTheAnalyticGapForRandomBoxPairs)
{
    /// @note 面・辺・角のどれが最近傍になるかを配置任せにして、式と突き合わせる。
    ///       単体が 1 頂点で止まる配置と、複数頂点を内分する配置の両方がここを通る。
    int checked = 0;
    for (int i = 0; i < 200; ++i) {
        const testkit::SupportBox a{Rng().NextVector3(-1.0f, 1.0f),
                                    Rng().NextVector3(0.3f, 1.5f)};
        const testkit::SupportBox b{Rng().NextVector3(-6.0f, 6.0f),
                                    Rng().NextVector3(0.3f, 1.5f)};

        /// @note 重なりぎわは EPA の領分。GJK_Distance は隙間を測ることだけを受け持つ。
        const float expected = AnalyticGap(a, b);
        if (expected < 0.05f) continue;

        const physics::GJKResult result = Distance(a, b);

        ASSERT_FALSE(result.intersects)
            << "a = " << ::testing::PrintToString(a.center)
            << " b = " << ::testing::PrintToString(b.center);
        EXPECT_NEAR(result.distance, expected, testkit::kLooseTolerance)
            << "a = " << ::testing::PrintToString(a.center)
            << " b = " << ::testing::PrintToString(b.center);
        ++checked;
    }

    /// @note 全部が «重なりぎわ» で弾かれると、何も確かめないまま緑になる。
    EXPECT_GT(checked, 50);
}

} // namespace fbzz::tests
