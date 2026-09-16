/// @file    EPATests.cpp
/// @brief   EPA が返す衝突法線・貫通深度・接触点が解析解と一致することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 法線の «向き» と深度の «量» は、間違っていても «めり込みが直る» ため気づきにくい。
/// 逆向きなら押し込み、深すぎれば弾かれる ── どちらも「物理がなんか変」で終わってしまう。
/// ここでは球と箱という解析解が出せる形状だけを使い、数値として固定する。
#include <TestKit/TestKit.hpp>

#include <TestKit/Physics/SupportShapes.hpp>

#include <Physics/EPA.hpp>
#include <Physics/GJK.hpp>

namespace fbzz::tests {

namespace {

/// GJK -> EPA を通し、交差していることを前提に接触情報を返す。
physics::EPAResult ContactOf(const void* a, physics::SupportFn supportA,
                             const void* b, physics::SupportFn supportB)
{
    const physics::GJKResult gjk = physics::GJK_Intersect(a, supportA, b, supportB);
    if (!gjk.intersects) return {};
    return physics::EPA_GetContactInfo(a, supportA, b, supportB, gjk.simplex);
}

physics::EPAResult ContactOf(const testkit::SupportSphere& a, const testkit::SupportSphere& b)
{
    return ContactOf(&a, testkit::kSphereSupport, &b, testkit::kSphereSupport);
}

physics::EPAResult ContactOf(const testkit::SupportBox& a, const testkit::SupportBox& b)
{
    return ContactOf(&a, testkit::kBoxSupport, &b, testkit::kBoxSupport);
}

} // namespace

class EPATest : public testkit::Fixture {};

// --- 法線の向き -------------------------------------------------------------

TEST_F(EPATest, NormalPointsFromBTowardAForSpheresOverlappingAlongX)
{
    // 法線は «B を A から引き離す» 向きではなく «B から A へ押し戻す» 向き。
    // ContactPoint::normal と同じ規約で、逆に取ると床に乗った物を床へ押し込む。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_VEC3_NEAR(contact.normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
}

TEST_F(EPATest, NormalFlipsWhenTheOperandsAreSwapped)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::EPAResult forward = ContactOf(a, b);
    const physics::EPAResult swapped = ContactOf(b, a);

    ASSERT_TRUE(forward.valid);
    ASSERT_TRUE(swapped.valid);
    EXPECT_VEC3_NEAR(swapped.normal, -forward.normal, testkit::kLooseTolerance);
}

TEST_F(EPATest, NormalIsAUnitVector)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.9f, 0.6f, -0.4f), 1.0f};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_UNIT_LENGTH(contact.normal, testkit::kLooseTolerance);
}

// --- 貫通深度 ---------------------------------------------------------------

TEST_F(EPATest, DepthMatchesTheOverlapOfTwoSpheres)
{
    // 球どうしの貫通量は «半径の和 - 中心間距離» で解析的に決まる。
    constexpr float kDistances[] = {0.5f, 1.0f, 1.5f, 1.9f};

    for (const float distance : kDistances) {
        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{math::Vector3(distance, 0.0f, 0.0f), 1.0f};

        const physics::EPAResult contact = ContactOf(a, b);

        ASSERT_TRUE(contact.valid) << "distance = " << distance;
        EXPECT_NEAR(contact.depth, 2.0f - distance, testkit::kLooseTolerance)
            << "distance = " << distance;
    }
}

TEST_F(EPATest, DepthIsAlwaysPositiveForIntersectingShapes)
{
    for (int i = 0; i < 32; ++i) {
        const math::Vector3 offset = Rng().NextUnitVector3() * 1.2f;

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{offset, 1.0f};

        const physics::EPAResult contact = ContactOf(a, b);

        ASSERT_TRUE(contact.valid) << "offset = " << ::testing::PrintToString(offset);
        EXPECT_GT(contact.depth, 0.0f);
    }
}

TEST_F(EPATest, ChoosesTheShallowestAxisForOverlappingBoxes)
{
    // X と Z は全面重なり、Y だけ 0.2 の重なり。最小移動量は Y 方向。
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
    const testkit::SupportBox b{math::Vector3(0.0f, 1.8f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_VEC3_NEAR(contact.normal, -math::Vector3::UP, testkit::kLooseTolerance);
    EXPECT_NEAR(contact.depth, 0.2f, testkit::kLooseTolerance);
}

TEST_F(EPATest, ChoosesTheShallowestAxisWhenTheOverlapIsOnZ)
{
    const testkit::SupportBox a{math::Vector3::ZERO, math::Vector3(2.0f, 2.0f, 2.0f)};
    const testkit::SupportBox b{math::Vector3(0.0f, 0.0f, 3.5f), math::Vector3(2.0f, 2.0f, 2.0f)};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_VEC3_NEAR(contact.normal, -math::Vector3::FORWARD, testkit::kLooseTolerance);
    EXPECT_NEAR(contact.depth, 0.5f, testkit::kLooseTolerance);
}

// --- 接触点 -----------------------------------------------------------------

TEST_F(EPATest, ContactPointsLieOnTheSurfaceOfEachSphere)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_NEAR((contact.contactA - a.center).Length(), a.radius, testkit::kLooseTolerance);
    EXPECT_NEAR((contact.contactB - b.center).Length(), b.radius, testkit::kLooseTolerance);
}

TEST_F(EPATest, ContactPointsAreSeparatedByTheDepthAlongTheNormal)
{
    // contactA は A の表面上で B 側へ最も入り込んだ点、contactB はその逆。
    // 法線は B→A 向きなので、contactB から contactA へのベクトルを法線に射影すると
    // そのまま貫通量になる。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_NEAR(math::Vector3::Dot(contact.contactB - contact.contactA, contact.normal),
                contact.depth, testkit::kLooseTolerance);
}

TEST_F(EPATest, ContactPointOnTheSphereFacesTheOtherShape)
{
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(1.5f, 0.0f, 0.0f), 1.0f};

    const physics::EPAResult contact = ContactOf(a, b);

    ASSERT_TRUE(contact.valid);
    EXPECT_VEC3_NEAR(contact.contactA, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(contact.contactB, math::Vector3(0.5f, 0.0f, 0.0f), testkit::kLooseTolerance);
}

// --- 入力の契約 -------------------------------------------------------------

TEST_F(EPATest, IsInvalidWhenTheSimplexIsNotATetrahedron)
{
    // GJK が四面体まで拡張できなかった場合、EPA はポリトープの初期面を作れない。
    // ここで無効を返さずに進むと、初期化されていない面を参照することになる。
    const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
    const testkit::SupportSphere b{math::Vector3(0.5f, 0.0f, 0.0f), 1.0f};

    physics::Simplex tooFewVertices;
    tooFewVertices.size = 2;

    const physics::EPAResult contact = physics::EPA_GetContactInfo(
        &a, testkit::kSphereSupport, &b, testkit::kSphereSupport, tooFewVertices);

    EXPECT_FALSE(contact.valid);
}

TEST_F(EPATest, ProducesAValidContactForEveryIntersectingRandomPair)
{
    // «交差していると GJK が言ったのに EPA が接触を作れない» は、
    // 呼び出し側が押し戻しを取りこぼす形で表に出る。組み合わせを振って潰す。
    for (int i = 0; i < 64; ++i) {
        const math::Vector3 direction = Rng().NextUnitVector3();
        const float         distance  = Rng().NextFloat(0.1f, 1.9f);

        const testkit::SupportSphere a{math::Vector3::ZERO, 1.0f};
        const testkit::SupportSphere b{direction * distance, 1.0f};

        const physics::GJKResult gjk = physics::GJK_Intersect(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport);
        ASSERT_TRUE(gjk.intersects);

        const physics::EPAResult contact = physics::EPA_GetContactInfo(
            &a, testkit::kSphereSupport, &b, testkit::kSphereSupport, gjk.simplex);

        EXPECT_TRUE(contact.valid)
            << "direction = " << ::testing::PrintToString(direction) << " distance = " << distance;
    }
}

} // namespace fbzz::tests
