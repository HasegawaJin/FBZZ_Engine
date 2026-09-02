/// @file    XPBDJointTests.cpp
/// @brief   ボールソケット・可動域・角度ドライブ・トルク上限の契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// どのテストも «親を静止させ、子の骨を +X へ 1m 伸ばし、重力で下へ引く» 形にしてある。
/// 関節フレームの X は骨の向き、Z 軸まわりの回転が «下へ垂れる» に対応する。
/// 目視で確かめにくい «制限で止まる» «力負けして垂れる» を、角度の数字で固定する。
#include <gtest/gtest.h>

#include <Physics/XPBDJoint.hpp>
#include <Physics/XPBDSolver.hpp>

#include <cmath>
#include <memory>

namespace fbzz::tests {

namespace {

constexpr float kGravity  = -9.81f;
constexpr float kBoneSpan = 1.0f;   // 関節から子の重心までの距離 [m]

/// 親 (静止) と子 (+X へ 1m) を 1 本の関節で繋いだ試験台。
struct JointRig {
    physics::XPBDSolver                  solver;
    std::unique_ptr<physics::RigidBody>  parent = std::make_unique<physics::RigidBody>();
    std::unique_ptr<physics::RigidBody>  child  = std::make_unique<physics::RigidBody>();
    physics::XPBDJoint*                  joint  = nullptr;

    JointRig()
    {
        parent->SetMass(1.0f);
        parent->m_isStatic = true;
        parent->SetPosition(math::Vector3::ZERO);

        child->SetMass(1.0f);
        child->SetPosition({ kBoneSpan, 0.0f, 0.0f });

        solver.SetGravity({ 0.0f, kGravity, 0.0f });
        solver.AddBody(parent.get());
        solver.AddBody(child.get());

        auto owned = std::make_unique<physics::XPBDJoint>(parent.get(), child.get());
        joint = owned.get();
        // 関節はワールド原点、フレームは «X が骨の向き» ＝ 無回転でよい。
        joint->Build(math::Vector3::ZERO, math::Quaternion::Identity());
        solver.AddConstraint(std::move(owned));
    }

    ~JointRig() { solver.ClearBodies(); }

    void Simulate(int frames, float dt = 1.0f / 60.0f)
    {
        for (int i = 0; i < frames; ++i) solver.Step(dt);
    }

    /// 減衰を入れて «振り子が振れ続ける» のを止める。静止位置を測るテストで使う。
    void Damp(float amount = 2.0f)
    {
        child->m_linearDrag  = amount;
        child->m_angularDrag = amount;
    }

    /// 骨を水平から radians だけ垂らした姿勢に置く。関節は原点に付いたまま。
    ///
    /// WHY 釣り合いの «答え» から始めるテストを用意するか: 落ちきるまでの時間は減衰と
    ///     慣性で決まり、テストが «収束待ちの秒数» に依存する。釣り合いの角度に置いて
    ///     «動かないこと» を見れば、時間に依らず釣り合いの位置だけを固定できる。
    void SetDroop(float radians)
    {
        child->SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, -radians));
        child->SetPosition({ kBoneSpan * std::cos(radians), -kBoneSpan * std::sin(radians), 0.0f });
    }

    /// 骨が水平からどれだけ垂れたか [rad]。0 で水平、+ で下向き。
    [[nodiscard]] float DroopAngle() const
    {
        const math::Vector3 bone = child->GetPosition();
        return -std::atan2(bone.y, std::sqrt(bone.x * bone.x + bone.z * bone.z));
    }

    /// 関節が離れていないか [m]。
    [[nodiscard]] float SocketError() const
    {
        const math::Vector3 anchor =
            child->GetPosition() + child->GetRotation() * math::Vector3{ -kBoneSpan, 0.0f, 0.0f };
        return anchor.Length();
    }
};

} // namespace

TEST(XPBDJointTest, SocketKeepsBodiesConnected)
{
    JointRig rig;
    rig.Simulate(180);
    EXPECT_LT(rig.SocketError(), 1.0e-3f);
}

// 脱力した関節は真下までぶら下がる。制限もドライブも無い状態の基準。
TEST(XPBDJointTest, LimplessJointHangsStraightDown)
{
    JointRig rig;
    rig.Damp();
    rig.Simulate(600);

    EXPECT_GT(rig.DroopAngle(), 1.4f);   // ほぼ π/2
    EXPECT_LT(rig.SocketError(), 1.0e-3f);
}

// 可動域は «そこで止まる» こと。膝が逆に折れないのはこの拘束。
TEST(XPBDJointTest, SwingLimitStopsTheBone)
{
    JointRig rig;
    auto& limits = rig.joint->Limits();
    limits.enabled   = true;
    limits.swingMinZ = -0.30f;   // 下へ垂れるのは Z 軸まわりの負回転
    limits.swingMaxZ = 0.30f;
    limits.swingMinY = -0.05f;
    limits.swingMaxY = 0.05f;
    limits.twistMin  = -0.05f;
    limits.twistMax  = 0.05f;

    rig.Damp();

    rig.Simulate(300);

    EXPECT_NEAR(rig.DroopAngle(), 0.30f, 0.02f);
    EXPECT_LT(rig.SocketError(), 1.0e-3f);
}

// 非対称な可動域。«片方向にしか曲がらない» が書けることの確認。
TEST(XPBDJointTest, AsymmetricSwingLimitIsRespected)
{
    JointRig rig;
    auto& limits = rig.joint->Limits();
    limits.enabled   = true;
    limits.swingMinZ = 0.0f;     // 下へは 1 度も曲がらない
    limits.swingMaxZ = 1.20f;    // 上へは大きく曲がる
    limits.swingMinY = -0.05f;
    limits.swingMaxY = 0.05f;
    limits.twistMin  = -0.05f;
    limits.twistMax  = 0.05f;

    rig.Damp();

    rig.Simulate(300);

    EXPECT_NEAR(rig.DroopAngle(), 0.0f, 0.02f);
}

// ドライブは目標姿勢を保つ。定常たわみは compliance × トルクで決まる。
TEST(XPBDJointTest, DriveHoldsTheTargetAgainstGravity)
{
    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = true;
    drive.compliance = 1.0e-5f;
    drive.damping    = 20.0f;

    rig.Simulate(180);

    EXPECT_LT(std::abs(rig.DroopAngle()), 0.02f);
    EXPECT_FALSE(rig.joint->IsDriveSaturated());
}

// M2 の核心その 1。支え切れずに «明らかに» 垂れること。どこまで垂れるかは下のテストが見る。
TEST(XPBDJointTest, TorqueLimitMakesTheDriveGiveWay)
{
    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = true;
    drive.compliance = 1.0e-5f;
    drive.damping    = 20.0f;
    // 水平で支えるのに要るトルクは m·g·L = 9.81 N·m。その 1/5 しか出せない。
    drive.maxTorque  = 2.0f;

    rig.Damp();
    rig.Simulate(300);

    EXPECT_GT(rig.DroopAngle(), 0.60f);
    EXPECT_TRUE(rig.joint->IsDriveSaturated());
}

// M2 の核心その 2。垂れるほど腕の «てこ» が短くなり、必要トルクが m·g·L·cos(θ) へ落ちる。
// cos(θ) = τ_max/(m·g·L) を満たす角度で釣り合うこと ─ 上限の «値» が効いている証拠。
TEST(XPBDJointTest, TorqueLimitBalancesAtTheAnalyticAngle)
{
    const float balance = std::acos(2.0f / 9.81f);

    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = true;
    drive.compliance = 1.0e-5f;
    drive.damping    = 20.0f;
    drive.maxTorque  = 2.0f;

    rig.Damp();
    rig.SetDroop(balance);
    rig.Simulate(240);

    EXPECT_NEAR(rig.DroopAngle(), balance, 0.10f);
    EXPECT_LT(rig.SocketError(), 1.0e-3f);
}

// 上の «動かない» が偶然でないこと。釣り合いより上に置けば落ち、下に置けば持ち上がる。
TEST(XPBDJointTest, SaturatedDriveMovesTowardTheBalanceFromBothSides)
{
    const float balance = std::acos(2.0f / 9.81f);

    const auto settle = [balance](float startDroop) {
        JointRig rig;
        auto& drive = rig.joint->Drive();
        drive.enabled    = true;
        drive.compliance = 1.0e-5f;
        drive.damping    = 20.0f;
        drive.maxTorque  = 2.0f;

        rig.Damp();
        rig.SetDroop(startDroop);
        rig.Simulate(300);
        return rig.DroopAngle();
    };

    // 釣り合いより «上» に置くと、2 N·m では支え切れずに落ちる。
    EXPECT_GT(settle(balance - 0.40f), balance - 0.40f);
    // «下» に置くと、必要トルクが 2 N·m を下回るので余った分で持ち上がる。
    // 真下 (π/2) を越えると重力トルクの符号が変わるので、それより手前に置く。
    EXPECT_LT(settle(balance + 0.15f), balance + 0.15f);
}

// 上限を十分に上げれば同じ設定で支え切る。上の結果がトルク上限由来だと固定する。
TEST(XPBDJointTest, HighTorqueLimitStillHolds)
{
    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = true;
    drive.compliance = 1.0e-5f;
    drive.damping    = 20.0f;
    drive.maxTorque  = 200.0f;

    rig.Simulate(180);

    EXPECT_LT(std::abs(rig.DroopAngle()), 0.05f);
}

// ドライブを切れば脱力する。Passive / Active の切り替えはこのフラグだけ。
TEST(XPBDJointTest, DisabledDriveFallsLikeLimpJoint)
{
    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = false;
    drive.compliance = 1.0e-5f;

    rig.Damp();
    rig.Simulate(600);

    EXPECT_GT(rig.DroopAngle(), 1.4f);
}

// ドライブの目標を動かすと骨が付いてくる。Active の «クリップを追う» 経路。
TEST(XPBDJointTest, DriveFollowsAMovingTarget)
{
    JointRig rig;
    auto& drive = rig.joint->Drive();
    drive.enabled    = true;
    drive.compliance = 1.0e-5f;
    drive.damping    = 20.0f;

    // 関節フレーム Z 軸まわりに +0.5 rad ＝ 骨を上へ持ち上げる目標。
    drive.target = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 0.5f);
    rig.Simulate(180);

    EXPECT_NEAR(rig.DroopAngle(), -0.5f, 0.05f);
}

TEST(XPBDJointTest, TwistLimitStopsRotationAboutTheBone)
{
    JointRig rig;
    auto& limits = rig.joint->Limits();
    limits.enabled   = true;
    limits.twistMin  = -0.20f;
    limits.twistMax  = 0.20f;
    limits.swingMinY = -2.0f;
    limits.swingMaxY = 2.0f;
    limits.swingMinZ = -2.0f;
    limits.swingMaxZ = 2.0f;

    // 骨の軸まわりに回し続ける。制限が効かなければ 1 回転してしまう。
    rig.Damp();
    rig.child->SetAngularVelocity({ 6.0f, 0.0f, 0.0f });
    rig.Simulate(120);

    EXPECT_LT(std::abs(rig.joint->GetTwistAngle()), 0.25f);
}

} // namespace fbzz::tests
