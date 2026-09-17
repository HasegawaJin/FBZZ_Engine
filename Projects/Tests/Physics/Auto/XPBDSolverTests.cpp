/// @file    XPBDSolverTests.cpp
/// @brief   XPBD ソルバの substep 積分と位置拘束の契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// M1 の受け入れ条件をそのままテストにしている。とくに «substep 数を変えても静止位置が
/// 変わらない» は、compliance で拘束を書いた狙い (定常誤差が刻みに依らない) が
/// 効いているかの直接の確認になる。ここが崩れると、以降の関節もドライブも
/// «刻みを変えると硬さが変わる» ものになる。
#include <TestKit/TestKit.hpp>

#include <Physics/XPBDPlaneContact.hpp>
#include <Physics/XPBDSolver.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace fbzz::tests {

class XPBDSolverTest : public testkit::Fixture {};

namespace {

constexpr float kGravity = -9.81f;

/// 1 秒ぶんを 60 フレームに割って回す。dt はゲーム側の固定刻みに合わせてある。
void Simulate(physics::XPBDSolver& solver, int frames, float dt = 1.0f / 60.0f)
{
    for (int i = 0; i < frames; ++i) solver.Step(dt);
}

std::unique_ptr<physics::RigidBody> MakeBody(const math::Vector3& position, float mass = 1.0f)
{
    auto body = std::make_unique<physics::RigidBody>();
    body->SetMass(mass);
    body->SetPosition(position);
    return body;
}

} // namespace

TEST_F(XPBDSolverTest, FreeFallMatchesAnalyticDrop)
{
    physics::XPBDSolver solver;
    solver.SetGravity({ 0.0f, kGravity, 0.0f });

    auto body = MakeBody({ 0.0f, 10.0f, 0.0f });
    solver.AddBody(body.get());

    Simulate(solver, 60);

    /// @note 半陰的オイラーは解析解より 0.5·g·h·t だけ余分に落ちる。substep を細かくすると
    ///       その差は縮む ─ ここでは «おおよそ自由落下している» ことだけ確認する。
    const float expected = 10.0f + 0.5f * kGravity * 1.0f;
    EXPECT_NEAR(body->GetPosition().y, expected, 0.15f);

    solver.ClearBodies();
}

TEST_F(XPBDSolverTest, PlaneContactStopsTheBody)
{
    physics::XPBDSolver solver;
    solver.SetGravity({ 0.0f, kGravity, 0.0f });

    auto body = MakeBody({ 0.0f, 3.0f, 0.0f });
    solver.AddBody(body.get());
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3::ZERO, 0.5f, math::Vector3::UP, 0.0f));

    Simulate(solver, 180);

    EXPECT_NEAR(body->GetPosition().y, 0.5f, 1.0e-3f);
    /// @note 位置から速度を出し直しているので、静止していれば速度も 0 になる。
    EXPECT_LT(body->GetVelocity().Length(), 1.0e-2f);

    solver.ClearBodies();
}

/// M1 の核心。刻みを変えても釣り合いの位置が動かないこと。
TEST_F(XPBDSolverTest, RestingHeightIsIndependentOfSubstepCount)
{
    const auto restingHeight = [](int substeps) {
        physics::XPBDSolver solver;
        solver.SetGravity({ 0.0f, kGravity, 0.0f });
        solver.SetSubsteps(substeps);

        auto body = MakeBody({ 0.0f, 2.0f, 0.0f });
        solver.AddBody(body.get());
        solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
            body.get(), math::Vector3::ZERO, 0.25f, math::Vector3::UP, 0.0f));

        Simulate(solver, 180);
        const float y = body->GetPosition().y;
        solver.ClearBodies();
        return y;
    };

    const float coarse = restingHeight(2);
    const float fine   = restingHeight(32);
    EXPECT_NEAR(coarse, fine, 1.0e-4f);
}

TEST_F(XPBDSolverTest, OffCenterContactGeneratesRotation)
{
    physics::XPBDSolver solver;
    solver.SetGravity({ 0.0f, kGravity, 0.0f });

    auto body = MakeBody({ 0.0f, 1.0f, 0.0f });
    solver.AddBody(body.get());
    /// @note 重心から外れた 1 点だけで支える。トルクが立たなければ姿勢は変わらない。
    solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
        body.get(), math::Vector3{ 0.6f, 0.0f, 0.0f }, 0.1f, math::Vector3::UP, 0.0f));

    Simulate(solver, 30);

    const math::Quaternion rotation = body->GetRotation();
    const float angle = 2.0f * std::acos(std::min(std::abs(rotation.w), 1.0f));
    EXPECT_GT(angle, 1.0e-3f);

    solver.ClearBodies();
}

TEST_F(XPBDSolverTest, StaticBodyIsNotIntegrated)
{
    physics::XPBDSolver solver;
    solver.SetGravity({ 0.0f, kGravity, 0.0f });

    auto body = MakeBody({ 0.0f, 5.0f, 0.0f });
    body->m_isStatic = true;
    solver.AddBody(body.get());

    Simulate(solver, 60);

    EXPECT_FLOAT_EQ(body->GetPosition().y, 5.0f);
    solver.ClearBodies();
}

TEST_F(XPBDSolverTest, RestoresSleepSettingWhenRemoved)
{
    physics::XPBDSolver solver;
    auto body = MakeBody({ 0.0f, 0.0f, 0.0f });
    ASSERT_TRUE(body->m_allowSleeping);

    solver.AddBody(body.get());
    EXPECT_FALSE(body->m_allowSleeping);

    solver.RemoveBody(body.get());
    EXPECT_TRUE(body->m_allowSleeping);
    EXPECT_EQ(solver.GetBodyCount(), 0);
}

TEST_F(XPBDSolverTest, IgnoresDuplicateBodyRegistration)
{
    physics::XPBDSolver solver;
    auto body = MakeBody({ 0.0f, 0.0f, 0.0f });

    solver.AddBody(body.get());
    solver.AddBody(body.get());
    EXPECT_EQ(solver.GetBodyCount(), 1);

    solver.ClearBodies();
}

TEST_F(XPBDSolverTest, FinalProjectionIsIncludedInDerivedVelocity)
{
    physics::XPBDSolver solver;
    solver.SetGravity(math::Vector3::ZERO);
    solver.SetSubsteps(4);
    auto body = MakeBody(math::Vector3::ZERO);
    body->SetVelocity({10.0f, 0.0f, 0.0f});
    solver.AddBody(body.get());
    solver.Step(1.0f / 60.0f, [](void* context) {
        auto* projected = static_cast<physics::RigidBody*>(context);
        projected->SetPosition(math::Vector3::ZERO);
    }, body.get());
    EXPECT_VEC3_NEAR(body->GetPosition(), math::Vector3::ZERO, 1.0e-6f);
    EXPECT_VEC3_NEAR(body->GetVelocity(), math::Vector3::ZERO, 1.0e-6f);
    solver.ClearBodies();
}

} // namespace fbzz::tests
