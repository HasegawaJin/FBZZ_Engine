/// @file    XPBDPoseAnchorTests.cpp
/// @brief   剛体をアニメーションの姿勢へ引き止める拘束の有効化・強さ・飽和を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 関節は «隣の骨との相対» しか拘束しない。根を繋ぎ止める 1 本が抜けると、
/// 形を保ったまま体ごと床下へ沈む ── 数秒の転倒では見えず、立っている間だけ出る。
/// «押されたぶんだけ動いて戻る» ことも含めて、力の上限の意味をここで固定する。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/XPBDPoseAnchor.hpp>
#include <Physics/XPBDSolver.hpp>

#include <memory>

namespace fbzz::tests {
namespace {

constexpr float kGravity = -9.81f;

void Simulate(physics::XPBDSolver& solver, int frames)
{
    for (int i = 0; i < frames; ++i) solver.Step(testkit::kFixedDeltaTime);
}

} // namespace

class XPBDPoseAnchorTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_body.SetMass(1.0f);
        m_body.SetPosition({ 0.0f, 5.0f, 0.0f });
        solver.SetGravity({ 0.0f, kGravity, 0.0f });
        solver.AddBody(&m_body);
    }

    void TearDown() override { solver.ClearBodies(); }

    /// 目標を今の姿勢に置いた繋ぎ止めを 1 本足し、その参照を返す。
    physics::XPBDPoseAnchor& AddAnchor(float maxForce = 1000.0f, float maxTorque = 1000.0f)
    {
        auto anchor = std::make_unique<physics::XPBDPoseAnchor>(&m_body);
        anchor->SetTarget(m_body.GetPosition(), m_body.GetRotation());
        anchor->SetStrength(maxForce, maxTorque, 0.1f, 0.1f);
        anchor->SetEnabled(true);
        physics::XPBDPoseAnchor& reference = *anchor;
        solver.AddConstraint(std::move(anchor));
        return reference;
    }

    physics::XPBDSolver solver;
    physics::RigidBody  m_body;
};

// --- 有効・無効 -------------------------------------------------------------

TEST_F(XPBDPoseAnchorTest, HoldsTheBodyAtItsTargetUnderGravity)
{
    AddAnchor();

    Simulate(solver, 120);

    EXPECT_NEAR(m_body.GetPosition().y, 5.0f, 0.05f);
}

TEST_F(XPBDPoseAnchorTest, LetsTheBodyFallWhenDisabled)
{
    // 脱力して倒れる間は切る。切れていないと «力が抜けたのに胴だけ宙に留まる»。
    physics::XPBDPoseAnchor& anchor = AddAnchor();
    anchor.SetEnabled(false);

    Simulate(solver, 60);

    EXPECT_LT(m_body.GetPosition().y, 1.0f);
}

TEST_F(XPBDPoseAnchorTest, StartsDisabledUntilItIsTurnedOn)
{
    // 既定は «繋ぎ止めない»。組んだ瞬間に勝手に吊られていると、
    // 倒れるはずのラグドールが浮いたままになる。
    auto anchor = std::make_unique<physics::XPBDPoseAnchor>(&m_body);
    anchor->SetTarget(m_body.GetPosition(), m_body.GetRotation());
    anchor->SetStrength(1000.0f, 1000.0f, 0.1f, 0.1f);
    EXPECT_FALSE(anchor->IsEnabled());
    solver.AddConstraint(std::move(anchor));

    Simulate(solver, 60);

    EXPECT_LT(m_body.GetPosition().y, 1.0f);
}

// --- 目標の移動 -------------------------------------------------------------

TEST_F(XPBDPoseAnchorTest, FollowsATargetThatMoves)
{
    // Active なラグドールは毎フレーム目標をクリップへ乗せ替える。付いて回ること。
    physics::XPBDPoseAnchor& anchor = AddAnchor();

    anchor.SetTarget({ 3.0f, 5.0f, 0.0f }, m_body.GetRotation());
    Simulate(solver, 120);

    EXPECT_NEAR(m_body.GetPosition().x, 3.0f, 0.1f);
}

TEST_F(XPBDPoseAnchorTest, PullsTheRotationTowardsTheTarget)
{
    physics::XPBDPoseAnchor& anchor = AddAnchor();
    const math::Quaternion turned =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));

    anchor.SetTarget(m_body.GetPosition(), turned);
    Simulate(solver, 120);

    EXPECT_QUAT_NEAR(m_body.GetRotation(), turned, 0.05f);
}

// --- 強さと飽和 -------------------------------------------------------------

TEST_F(XPBDPoseAnchorTest, SagsBelowTheTargetWhenTheForceLimitIsTooLow)
{
    // 上限を «自重より弱く» すると支え切れない。押されたぶんだけ沈む挙動の芯。
    AddAnchor(1.0f, 1000.0f);   // 自重は約 9.8 N

    Simulate(solver, 120);

    EXPECT_LT(m_body.GetPosition().y, 5.0f);
}

TEST_F(XPBDPoseAnchorTest, ReportsSaturationWhenItCannotHold)
{
    physics::XPBDPoseAnchor& weak = AddAnchor(1.0f, 1000.0f);

    Simulate(solver, 120);

    EXPECT_TRUE(weak.IsSaturated());
}

TEST_F(XPBDPoseAnchorTest, DoesNotReportSaturationWhileItHolds)
{
    physics::XPBDPoseAnchor& strong = AddAnchor(1000.0f, 1000.0f);

    Simulate(solver, 120);

    EXPECT_FALSE(strong.IsSaturated());
}

TEST_F(XPBDPoseAnchorTest, ReportsHowFarItIsFromTheTarget)
{
    // 「どれだけ引き止め切れていないか」の物差し。0 なら目標どおり。
    physics::XPBDPoseAnchor& strong = AddAnchor();

    Simulate(solver, 120);
    const float held = strong.GetOffset();

    strong.SetTarget({ 0.0f, 20.0f, 0.0f }, m_body.GetRotation());
    Simulate(solver, 1);

    EXPECT_LT(held, 0.1f);
    EXPECT_GT(strong.GetOffset(), 1.0f);
}

TEST_F(XPBDPoseAnchorTest, TreatsANonPositiveLimitAsUnlimited)
{
    // 0 以下は «上限なし»。«0 = 力を出さない» と読み替えると、
    // 上限を設定していない繋ぎ止めが全部効かなくなる。
    AddAnchor(0.0f, 0.0f);

    Simulate(solver, 120);

    EXPECT_NEAR(m_body.GetPosition().y, 5.0f, 0.05f);
}

TEST_F(XPBDPoseAnchorTest, FiniteZeroLinearForceDoesNotDisableAngularSupport)
{
    auto& anchor = AddAnchor();
    anchor.SetFiniteStrength(0.0f, 1000.0f, 0.1f, 0.1f);
    const auto turned = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.5f);
    anchor.SetTarget(m_body.GetPosition(), turned);
    Simulate(solver, 120);
    EXPECT_LT(m_body.GetPosition().y, 1.0f);
    EXPECT_QUAT_NEAR(m_body.GetRotation(), turned, 0.05f);
}

TEST_F(XPBDPoseAnchorTest, FiniteZeroTorqueDoesNotLockRotationOrDisableLinearSupport)
{
    auto& anchor = AddAnchor();
    anchor.SetFiniteStrength(1000.0f, 0.0f, 0.1f, 0.1f);
    const auto turned = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.5f);
    m_body.SetRotation(turned);
    Simulate(solver, 120);
    EXPECT_NEAR(m_body.GetPosition().y, 5.0f, 0.05f);
    EXPECT_QUAT_NEAR(m_body.GetRotation(), turned, 0.05f);
}

} // namespace fbzz::tests
