/// @file    ContactResolveTests.cpp
/// @brief   接触の解決 (法線インパルス・反発・摩擦・貫通補正) が材質の値どおりに効くことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// PhysicsMaterial の合成規則そのものは別途テストしてあるが、«合成した値が実際に
/// インパルスへ効いているか» は誰も見ていなかった。反発 0 のはずが跳ねる、摩擦を
/// 上げても滑る、といった «材質を変えても手応えが変わらない» はここでしか捕まらない。
#include <TestKit/TestKit.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Solver/ContactPoint.hpp>
#include <Physics/Dynamics/PhysicsMaterial.hpp>
#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include <Physics/Dynamics/RigidBody.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

physics::PhysicsMaterial MakeMaterial(float restitution, float friction)
{
    physics::PhysicsMaterial material;
    material.restitution     = restitution;
    material.staticFriction  = friction;
    material.dynamicFriction = friction;
    return material;
}

} /// @note namespace

/// 床 (静的) の上に落ちてきた剛体 1 個、という最小の接触。
/// normal は «B から A へ» なので、床を B、落ちてきた側を A に置く。
class ContactResolveTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        falling.SetMass(1.0f);
        falling.SetPosition({ 0.0f, 1.0f, 0.0f });
        ground.m_isStatic = true;
        ground.SetMass(1.0f);
        ground.SetPosition(math::Vector3::ZERO);
    }

    /// 貫通 depth の接触を 1 つ作る。材質は呼び出し側が決める。
    physics::ContactPoint MakeContact(const physics::PhysicsMaterial& material,
                                      float                           depth = 0.0f)
    {
        physics::ContactPoint contact;
        contact.point     = math::Vector3::ZERO;
        contact.normal    = math::Vector3::UP;
        contact.depth     = depth;
        contact.bodyA     = &falling;
        contact.bodyB     = &ground;
        contact.materialA = &material;
        contact.materialB = &material;
        return contact;
    }

    void Resolve(physics::ContactPoint contact)
    {
        std::vector<physics::ContactPoint> contacts{ contact };
        solver.Resolve(contacts);
    }

    physics::PhysicsSolver solver;
    physics::RigidBody     falling;
    physics::RigidBody     ground;
};

/// @name 法線方向

/// @brief Stops a body that is moving into the surface.
TEST_F(ContactResolveTest, StopsABodyThatIsMovingIntoTheSurface)
{
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);
    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });

    Resolve(MakeContact(material));

    // 反発 0 なら «めり込む向きの速度» だけが消える。
    EXPECT_NEAR(falling.GetVelocity().y, 0.0f, testkit::kLooseTolerance);
}

/// @brief Does not touch a body that is already separating.
TEST_F(ContactResolveTest, DoesNotTouchABodyThatIsAlreadySeparating)
{
    // 離れていく接触にインパルスを入れると、跳ね上がった直後にもう一度蹴られる。
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);
    falling.SetVelocity({ 0.0f, 5.0f, 0.0f });

    Resolve(MakeContact(material));

    EXPECT_NEAR(falling.GetVelocity().y, 5.0f, testkit::kLooseTolerance);
}

/// @brief Does not drag the body sideways without friction.
TEST_F(ContactResolveTest, DoesNotDragTheBodySidewaysWithoutFriction)
{
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);
    falling.SetVelocity({ 3.0f, -5.0f, 0.0f });

    Resolve(MakeContact(material));

    EXPECT_NEAR(falling.GetVelocity().x, 3.0f, testkit::kLooseTolerance);
}

/// @brief Leaves a static body where it is.
TEST_F(ContactResolveTest, LeavesAStaticBodyWhereItIs)
{
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);
    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });

    Resolve(MakeContact(material));

    EXPECT_VEC3_NEAR(ground.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(ground.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @name 反発

/// @brief Bounce height follows the restitution.
TEST_F(ContactResolveTest, BounceHeightFollowsTheRestitution)
{
    // 材質の値がそのまま «跳ね返る速度の比» になる。
    const physics::PhysicsMaterial bouncy = MakeMaterial(0.8f, 0.0f);
    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });

    Resolve(MakeContact(bouncy));

    EXPECT_NEAR(falling.GetVelocity().y, 4.0f, 0.5f);
}

/// @brief Checks bouncier materials come back faster.
TEST_F(ContactResolveTest, BouncierMaterialsComeBackFaster)
{
    const physics::PhysicsMaterial dull   = MakeMaterial(0.1f, 0.0f);
    const physics::PhysicsMaterial rubber = MakeMaterial(0.9f, 0.0f);

    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });
    Resolve(MakeContact(dull));
    const float dullSpeed = falling.GetVelocity().y;

    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });
    Resolve(MakeContact(rubber));

    EXPECT_GT(falling.GetVelocity().y, dullSpeed);
}

/// @brief Takes the less bouncy of the two materials.
TEST_F(ContactResolveTest, TakesTheLessBouncyOfTheTwoMaterials)
{
    // 反発の既定の合成規則は Minimum。ゴム玉を粘土の床へ落としても跳ねない。
    const physics::PhysicsMaterial rubber = MakeMaterial(0.9f, 0.0f);
    const physics::PhysicsMaterial clay   = MakeMaterial(0.0f, 0.0f);

    physics::ContactPoint contact = MakeContact(rubber);
    contact.materialB = &clay;
    falling.SetVelocity({ 0.0f, -5.0f, 0.0f });

    Resolve(contact);

    EXPECT_NEAR(falling.GetVelocity().y, 0.0f, testkit::kLooseTolerance);
}

/// @name 摩擦

/// @brief Friction slows the slide along the surface.
TEST_F(ContactResolveTest, FrictionSlowsTheSlideAlongTheSurface)
{
    const physics::PhysicsMaterial grippy = MakeMaterial(0.0f, 1.0f);
    falling.SetVelocity({ 3.0f, -5.0f, 0.0f });

    Resolve(MakeContact(grippy));

    EXPECT_LT(falling.GetVelocity().x, 3.0f);
    // 押し返して逆走させない
    EXPECT_GE(falling.GetVelocity().x, 0.0f);
}

/// @brief Grippier materials slow the slide more.
TEST_F(ContactResolveTest, GrippierMaterialsSlowTheSlideMore)
{
    const physics::PhysicsMaterial ice    = MakeMaterial(0.0f, 0.02f);
    const physics::PhysicsMaterial rubber = MakeMaterial(0.0f, 1.0f);

    falling.SetVelocity({ 3.0f, -5.0f, 0.0f });
    Resolve(MakeContact(ice));
    const float onIce = falling.GetVelocity().x;

    falling.SetVelocity({ 3.0f, -5.0f, 0.0f });
    Resolve(MakeContact(rubber));

    EXPECT_LT(falling.GetVelocity().x, onIce);
}

/// @brief Friction acts on every tangential direction.
TEST_F(ContactResolveTest, FrictionActsOnEveryTangentialDirection)
{
    // 接線は 2 軸ある。片方しか解いていないと、斜めに滑ったときだけ止まらない。
    const physics::PhysicsMaterial grippy = MakeMaterial(0.0f, 1.0f);
    falling.SetVelocity({ 3.0f, -5.0f, 3.0f });

    Resolve(MakeContact(grippy));

    EXPECT_LT(falling.GetVelocity().x, 3.0f);
    EXPECT_LT(falling.GetVelocity().z, 3.0f);
}

/// @name トリガー

/// @brief Trigger contacts leave velocity and position unchanged.
TEST_F(ContactResolveTest, TriggerContactsLeaveVelocityAndPositionUnchanged)
{
    // トリガーは «通知するだけ»。速度も位置も動かしてはいけない。
    const physics::PhysicsMaterial material = MakeMaterial(0.5f, 1.0f);
    physics::ContactPoint contact = MakeContact(material, 0.5f);
    contact.isTrigger = true;
    falling.SetVelocity({ 3.0f, -5.0f, 0.0f });

    Resolve(contact);

    EXPECT_VEC3_NEAR(falling.GetVelocity(), math::Vector3(3.0f, -5.0f, 0.0f),
                     testkit::kTolerance);
    EXPECT_VEC3_NEAR(falling.GetPosition(), math::Vector3(0.0f, 1.0f, 0.0f),
                     testkit::kTolerance);
}

/// @name 貫通の補正

/// @brief Pushes a deeply overlapping body out along the normal.
TEST_F(ContactResolveTest, PushesADeeplyOverlappingBodyOutAlongTheNormal)
{
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);

    Resolve(MakeContact(material, 0.5f));

    EXPECT_GT(falling.GetPosition().y, 1.0f);
}

/// @brief Leaves a shallow overlap alone.
TEST_F(ContactResolveTest, LeavesAShallowOverlapAlone)
{
    // 許容量 (slop) 以下の重なりまで押し返すと、接地したものが小刻みに震える。
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);

    Resolve(MakeContact(material, 0.005f));

    EXPECT_NEAR(falling.GetPosition().y, 1.0f, testkit::kTolerance);
}

/// @brief Deeper overlaps are pushed further.
TEST_F(ContactResolveTest, DeeperOverlapsArePushedFurther)
{
    const physics::PhysicsMaterial material = MakeMaterial(0.0f, 0.0f);

    Resolve(MakeContact(material, 0.2f));
    const float shallowLift = falling.GetPosition().y - 1.0f;

    falling.SetPosition({ 0.0f, 1.0f, 0.0f });
    Resolve(MakeContact(material, 1.0f));

    EXPECT_GT(falling.GetPosition().y - 1.0f, shallowLift);
}

/// @brief Resolving nothing is harmless.
TEST_F(ContactResolveTest, ResolvingNothingIsHarmless)
{
    std::vector<physics::ContactPoint> empty;

    solver.Resolve(empty);

    SUCCEED();
}

} /// @note namespace fbzz::tests
