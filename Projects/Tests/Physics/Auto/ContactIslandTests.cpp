/// @file    ContactIslandTests.cpp
/// @brief   複数接触の解決 — 繋がった剛体を 1 つの島にまとめて解くこと、解く価値のない接触を外すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// 接触が 1 つのときは «その 1 本を解く» だけで済むが、積み上がった箱やラグドールでは
/// 接触が鎖のように繋がる。繋がった接触を別々に解くと、下から順に押し戻した結果が
/// 上へ伝わらず、山が沈み込んでから跳ね上がる。逆に、静止した物同士や Trigger まで
/// 解きにいくと、動かないものへ毎フレーム反復を回すだけになる。
///
/// 1 接触あたりの材質の効き方は `ContactResolveTests` が見る。ここは «どの接触を、
/// どの単位で解くか» だけを見る。
#include <TestKit/TestKit.hpp>

#include <Math/Vector3.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/RigidBody.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

/// upper が lower へ落ちてくる接触。normal は «B から A へ» なので UP 固定でよい。
/// 接触点は 2 体の重心を結ぶ線上に置き、角速度の項が入らないようにする。
physics::ContactPoint Touching(physics::RigidBody& upper, physics::RigidBody& lower)
{
    physics::ContactPoint contact;
    contact.point  = (upper.GetPosition() + lower.GetPosition()) * 0.5f;
    contact.normal = math::Vector3::UP;
    contact.depth  = 0.0f;
    contact.bodyA  = &upper;
    contact.bodyB  = &lower;
    return contact;
}

} // namespace

/// 上から A・B・C・D が 1 m 刻みで積まれ、上ほど速く落ちている。
/// 隣り合う組はすべて «近づいている» ので、解けば必ずインパルスが立つ。
class ContactIslandTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        physics::RigidBody* bodies[]   = { &a, &b, &c, &d };
        const float         heights[]  = { 3.0f, 2.0f, 1.0f, 0.0f };
        const float         falling[]  = { -3.0f, -2.0f, -1.0f, 0.0f };
        for (int i = 0; i < 4; ++i) {
            bodies[i]->SetMass(1.0f);
            bodies[i]->SetPosition({ 0.0f, heights[i], 0.0f });
            bodies[i]->SetVelocity({ 0.0f, falling[i], 0.0f });
        }
    }

    physics::PhysicsSolver solver;
    physics::RigidBody     a;
    physics::RigidBody     b;
    physics::RigidBody     c;
    physics::RigidBody     d;
};

// --- 島の構築 ---------------------------------------------------------------

TEST_F(ContactIslandTest, ResolvesEveryContactOfAConnectedStack)
{
    std::vector<physics::ContactPoint> contacts{ Touching(a, b), Touching(b, c), Touching(c, d) };

    solver.Resolve(contacts);

    // 1 本でも取りこぼすと、その段だけ押し戻されずに沈む。
    for (const physics::ContactPoint& contact : contacts)
        EXPECT_GT(contact.cachedNormalImpulse, 0.0f);
}

TEST_F(ContactIslandTest, MergesTwoIslandsWhenALaterContactBridgesThem)
{
    // A-B と C-D を別々に見つけたあとで、B-C が両者を繋ぐ。この «後から合流する»
    // 順序で島の付け替えを間違えると、片方の島が空のまま捨てられて接触が丸ごと消える。
    std::vector<physics::ContactPoint> contacts{ Touching(a, b), Touching(c, d), Touching(b, c) };

    solver.Resolve(contacts);

    for (const physics::ContactPoint& contact : contacts)
        EXPECT_GT(contact.cachedNormalImpulse, 0.0f);
}

TEST_F(ContactIslandTest, ResolvesIndependentIslandsSeparately)
{
    // 繋がっていない 2 組。合流させずにそれぞれ解けることを見る。
    std::vector<physics::ContactPoint> contacts{ Touching(a, b), Touching(c, d) };

    solver.Resolve(contacts);

    EXPECT_GT(contacts[0].cachedNormalImpulse, 0.0f);
    EXPECT_GT(contacts[1].cachedNormalImpulse, 0.0f);
}

// --- 解かない接触 -----------------------------------------------------------

TEST_F(ContactIslandTest, LeavesTriggerContactsUnresolved)
{
    std::vector<physics::ContactPoint> contacts{ Touching(a, b), Touching(c, d) };
    contacts[0].isTrigger = true;
    const math::Vector3 velocityBefore = a.GetVelocity();

    solver.Resolve(contacts);

    // Trigger は «通り抜けられる当たり»。押し返した時点で Trigger ではなくなる。
    EXPECT_FLOAT_EQ(contacts[0].cachedNormalImpulse, 0.0f);
    EXPECT_VEC3_NEAR(a.GetVelocity(), velocityBefore, testkit::kTolerance);
    EXPECT_GT(contacts[1].cachedNormalImpulse, 0.0f);
}

TEST_F(ContactIslandTest, SkipsContactsWhereNeitherSideCanMove)
{
    a.m_isStatic = true;
    a.SetMass(1.0f);
    b.Sleep();

    std::vector<physics::ContactPoint> contacts{ Touching(a, b), Touching(c, d) };

    solver.Resolve(contacts);

    // 静止した床と眠っている箱の接触を毎 substep 解いても状態は変わらない。
    // 積み上がったシーンではこの組が大半を占めるので、外せないと反復が空回りする。
    EXPECT_FLOAT_EQ(contacts[0].cachedNormalImpulse, 0.0f);
    EXPECT_GT(contacts[1].cachedNormalImpulse, 0.0f);
}

// --- 摩擦の基底 -------------------------------------------------------------

TEST_F(ContactIslandTest, BuildsAFrictionBasisEvenWhenTheNormalIsTheReferenceAxis)
{
    // 壁との接触。法線が基底の作成に使う既定軸と平行なので、外積が 0 に潰れる。
    // 予備の軸へ切り替えないと摩擦の 2 軸が NaN になり、接触点ごと壊れる。
    std::vector<physics::ContactPoint> contacts{ Touching(a, b) };
    contacts[0].normal = math::Vector3::RIGHT;

    solver.Resolve(contacts);

    EXPECT_UNIT_LENGTH(contacts[0].tangent[0], testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(contacts[0].tangent[1], testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(contacts[0].tangent[0], contacts[0].normal),
                0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(math::Vector3::Dot(contacts[0].tangent[1], contacts[0].normal),
                0.0f, testkit::kLooseTolerance);
}

TEST_F(ContactIslandTest, BuildsAFrictionBasisForAnOrdinaryNormal)
{
    std::vector<physics::ContactPoint> contacts{ Touching(a, b) };

    solver.Resolve(contacts);

    // 2 軸は互いにも直交していること。潰れていると摩擦が 1 方向にしか効かない。
    EXPECT_NEAR(math::Vector3::Dot(contacts[0].tangent[0], contacts[0].tangent[1]),
                0.0f, testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(contacts[0].tangent[0], testkit::kLooseTolerance);
}

} // namespace fbzz::tests
