/// @file    XPBDPlaneContactTests.cpp
/// @brief   平面接触の «半径ぶん浮いて止まる»・摩擦・面の移動・貫通量の報告を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ラグドールの足元はこの片側拘束で支えている。半径の扱いを間違えると床にめり込むか
/// 浮き、摩擦が効かないと着地した体がどこまでも滑る。どちらも «物理がなんか変» に
/// しか見えないので、静止位置と滑り量を数値として固定する。
#include <TestKit/TestKit.hpp>

#include <Math/Vector3.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/XPBDPlaneContact.hpp>
#include <Physics/XPBDSolver.hpp>

#include <memory>

namespace fbzz::tests {
namespace {

constexpr float kGravity = -9.81f;

void Simulate(physics::XPBDSolver& solver, int frames)
{
    for (int i = 0; i < frames; ++i) solver.Step(testkit::kFixedDeltaTime);
}

std::unique_ptr<physics::RigidBody> MakeBody(const math::Vector3& position)
{
    auto body = std::make_unique<physics::RigidBody>();
    body->SetMass(1.0f);
    body->SetPosition(position);
    return body;
}

} // namespace

class XPBDPlaneContactTest : public testkit::Fixture {
protected:
    void TearDown() override { solver.ClearBodies(); }

    physics::XPBDSolver solver;
};

/// @name 静止位置

TEST_F(XPBDPlaneContactTest, RestsOneRadiusAboveThePlane)
{
    /// @note 太さを持つ点として解く。半径を無視すると足首まで床へ埋まる。
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 3.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f));

    Simulate(solver, 120);

    EXPECT_NEAR(body->GetPosition().y, 0.5f, testkit::kLooseTolerance);
}

TEST_F(XPBDPlaneContactTest, HonoursThePlaneOffset)
{
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 5.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 2.0f));

    Simulate(solver, 120);

    EXPECT_NEAR(body->GetPosition().y, 2.5f, testkit::kLooseTolerance);
}

TEST_F(XPBDPlaneContactTest, OffsetsTheContactPointByTheLocalOffset)
{
    /// @note 接触点は重心ではなく «足の裏»。ローカル offset を無視すると体が半分沈む。
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 5.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3{ 0.0f, -1.0f, 0.0f }, 0.5f, math::Vector3::UP, 0.0f));

    Simulate(solver, 120);

    /// @note 重心の 1m 下が接触点。そこが半径 0.5 で止まるので重心は 1.5。
    EXPECT_NEAR(body->GetPosition().y, 1.5f, testkit::kLooseTolerance);
}

TEST_F(XPBDPlaneContactTest, DoesNotPullTheBodyDownFromAbove)
{
    /// @note 片側拘束。上に居るものを引き寄せてはいけない。
    auto body = MakeBody({ 0.0f, 10.0f, 0.0f });
    solver.SetGravity(math::Vector3::ZERO);
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f));

    Simulate(solver, 60);

    EXPECT_NEAR(body->GetPosition().y, 10.0f, testkit::kLooseTolerance);
}

/// @name 貫通量の報告

TEST_F(XPBDPlaneContactTest, ReportsNoPenetrationWhileInTheAir)
{
    auto body = MakeBody({ 0.0f, 10.0f, 0.0f });
    solver.SetGravity(math::Vector3::ZERO);
    solver.AddBody(body.get());
    auto contact = std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f);
    const physics::XPBDPlaneContact* probe = contact.get();
    solver.AddConstraint(std::move(contact));

    Simulate(solver, 10);

    EXPECT_NEAR(probe->GetPenetration(), 0.0f, testkit::kTolerance);
}

TEST_F(XPBDPlaneContactTest, ReportsThePenetrationItPushedBack)
{
    /// @note 接地しているかの判定にそのまま使える値であること。
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    /// @note 最初から半径ぶん埋まっている
    auto body = MakeBody({ 0.0f, 0.0f, 0.0f });
    solver.AddBody(body.get());
    auto contact = std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f);
    const physics::XPBDPlaneContact* probe = contact.get();
    solver.AddConstraint(std::move(contact));

    Simulate(solver, 1);

    EXPECT_GT(probe->GetPenetration(), 0.0f);
}

/// @name 摩擦

TEST_F(XPBDPlaneContactTest, SlidesForeverWithoutFriction)
{
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 0.5f, 0.0f });
    body->SetVelocity({ 4.0f, 0.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f));

    Simulate(solver, 60);

    /// @note 1 秒で 4m。氷の上なので目立って減速しない。
    EXPECT_GT(body->GetPosition().x, 3.0f);
}

TEST_F(XPBDPlaneContactTest, FrictionShortensTheSlide)
{
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 0.5f, 0.0f });
    body->SetVelocity({ 4.0f, 0.0f, 0.0f });
    solver.AddBody(body.get());
    auto contact = std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f);
    contact->SetFriction(1.0f);
    solver.AddConstraint(std::move(contact));

    Simulate(solver, 60);

    EXPECT_LT(body->GetPosition().x, 3.0f);
    /// @note 逆走はしない
    EXPECT_GT(body->GetPosition().x, 0.0f);
}

TEST_F(XPBDPlaneContactTest, FrictionDoesNotDragAStandingBodySideways)
{
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 0.5f, 0.0f });
    solver.AddBody(body.get());
    auto contact = std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f);
    contact->SetFriction(1.0f);
    solver.AddConstraint(std::move(contact));

    Simulate(solver, 60);

    EXPECT_NEAR(body->GetPosition().x, 0.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(body->GetPosition().z, 0.0f, testkit::kLooseTolerance);
}

/// @name 面を動かす

TEST_F(XPBDPlaneContactTest, FollowsThePlaneWhenItIsMoved)
{
    /// @note 歩いているキャラクターの足元へ «床を付いて回らせる» 経路。
    solver.SetGravity({ 0.0f, kGravity, 0.0f });
    auto body = MakeBody({ 0.0f, 3.0f, 0.0f });
    solver.AddBody(body.get());
    auto contact = std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f);
    physics::XPBDPlaneContact* movable = contact.get();
    solver.AddConstraint(std::move(contact));

    Simulate(solver, 120);
    movable->SetPlane(math::Vector3::UP, 4.0f);
    Simulate(solver, 60);

    EXPECT_NEAR(body->GetPosition().y, 4.5f, testkit::kLooseTolerance);
}

TEST_F(XPBDPlaneContactTest, SupportsANonVerticalPlane)
{
    /// @note 壁として使う。法線方向にだけ押し返し、面に沿っては滑る。
    solver.SetGravity(math::Vector3::ZERO);
    auto body = MakeBody({ -2.0f, 0.0f, 0.0f });
    body->SetVelocity({ -1.0f, 0.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::RIGHT, -3.0f));

    Simulate(solver, 120);

    EXPECT_NEAR(body->GetPosition().x, -2.5f, testkit::kLooseTolerance);
}

} // namespace fbzz::tests
