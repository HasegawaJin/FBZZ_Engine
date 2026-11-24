/// @file    CCDTests.cpp
/// @brief   高速移動物体の TOI (球×球 / 球×平面) と、CCD を適用する速度のしきい値を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// CCD が壊れた形は 2 つ。«すり抜ける» と «手前で止まる»。前者は弾が敵を通過する、
/// 後者は何も無い所で当たる ── どちらも «たまに起きる» ので、実機で再現させにくい。
/// 解析解が出る配置だけを使い、TOI を数値として固定する。
#include <TestKit/TestKit.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Collision/NarrowPhase/CCDSolver.hpp>
#include <Physics/Dynamics/RigidBody.hpp>

namespace fbzz::tests {

class CCDTest : public testkit::Fixture {};

/// @name 球 × 球

/// @brief Finds the time of impact for an approaching sphere.
TEST_F(CCDTest, FindsTheTimeOfImpactForAnApproachingSphere)
{
    /// @note 1 フレームで 10 進む球。中心間 10、半径の和 2 なので 8 進んだところで接触する。
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.toi, 0.8f, testkit::kLooseTolerance);
}

/// @brief Reports the normal pointing from the target to the mover.
TEST_F(CCDTest, ReportsTheNormalPointingFromTheTargetToTheMover)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    ASSERT_TRUE(result.hit);
    EXPECT_VEC3_NEAR(result.normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(result.contactPoint, math::Vector3(-1.0f, 0.0f, 0.0f),
                     testkit::kLooseTolerance);
}

/// @brief Misses a sphere that the path passes beside.
TEST_F(CCDTest, MissesASphereThatThePathPassesBeside)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 5.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    EXPECT_FALSE(result.hit);
}

/// @brief Misses when the frame is too short to reach the target.
TEST_F(CCDTest, MissesWhenTheFrameIsTooShortToReachTheTarget)
{
    /// @note 同じ速度でも dt が短ければ届かない。TOI が 1 を超える解は採らない。
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.01f);

    EXPECT_FALSE(result.hit);
}

/// @brief Misses when the mover is standing still.
TEST_F(CCDTest, MissesWhenTheMoverIsStandingStill)
{
    /// @note 動いていなければ通り抜けようが無い。ここで解こうとすると 0 除算になる。
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, math::Vector3::ZERO,
        math::Vector3::ZERO, 1.0f, 0.1f);

    EXPECT_FALSE(result.hit);
}

/// @brief Misses when the mover is travelling away.
TEST_F(CCDTest, MissesWhenTheMoverIsTravellingAway)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { -100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    EXPECT_FALSE(result.hit);
}

/// @brief Time of impact shrinks as the mover gets faster.
TEST_F(CCDTest, TimeOfImpactShrinksAsTheMoverGetsFaster)
{
    const physics::CCDResult slow = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);
    const physics::CCDResult fast = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 200.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    ASSERT_TRUE(slow.hit);
    ASSERT_TRUE(fast.hit);
    EXPECT_LT(fast.toi, slow.toi);
}

/// @brief Larger radii make contact happen earlier.
TEST_F(CCDTest, LargerRadiiMakeContactHappenEarlier)
{
    const physics::CCDResult thin = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 1.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);
    const physics::CCDResult fat = physics::CCDSolver::SweptSphereSphere(
        { -10.0f, 0.0f, 0.0f }, 3.0f, { 100.0f, 0.0f, 0.0f },
        math::Vector3::ZERO, 1.0f, 0.1f);

    ASSERT_TRUE(thin.hit);
    ASSERT_TRUE(fat.hit);
    EXPECT_LT(fat.toi, thin.toi);
}

/// @name 球 × 平面

/// @brief Finds the time of impact against a ground plane.
TEST_F(CCDTest, FindsTheTimeOfImpactAgainstAGroundPlane)
{
    /// @note 高さ 10 から 1 フレームで 20 落ちる。半径 1 なので高さ 1 で接触 = 9 落ちた時点。
    const physics::CCDResult result = physics::CCDSolver::SweptSpherePlane(
        { 0.0f, 10.0f, 0.0f }, 1.0f, { 0.0f, -200.0f, 0.0f },
        math::Vector3::UP, 0.0f, 0.1f);

    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.toi, 0.45f, testkit::kLooseTolerance);
}

/// @brief Contact with a plane sits one radius below the centre.
TEST_F(CCDTest, ContactWithAPlaneSitsOneRadiusBelowTheCentre)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSpherePlane(
        { 0.0f, 10.0f, 0.0f }, 1.0f, { 0.0f, -200.0f, 0.0f },
        math::Vector3::UP, 0.0f, 0.1f);

    ASSERT_TRUE(result.hit);
    EXPECT_VEC3_NEAR(result.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(result.contactPoint, math::Vector3::ZERO, testkit::kLooseTolerance);
}

/// @brief Misses a plane it is moving away from.
TEST_F(CCDTest, MissesAPlaneItIsMovingAwayFrom)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSpherePlane(
        { 0.0f, 10.0f, 0.0f }, 1.0f, { 0.0f, 200.0f, 0.0f },
        math::Vector3::UP, 0.0f, 0.1f);

    EXPECT_FALSE(result.hit);
}

/// @brief Misses a plane it cannot reach this frame.
TEST_F(CCDTest, MissesAPlaneItCannotReachThisFrame)
{
    const physics::CCDResult result = physics::CCDSolver::SweptSpherePlane(
        { 0.0f, 10.0f, 0.0f }, 1.0f, { 0.0f, -10.0f, 0.0f },
        math::Vector3::UP, 0.0f, 0.1f);

    EXPECT_FALSE(result.hit);
}

/// @brief Honours the plane offset.
TEST_F(CCDTest, HonoursThePlaneOffset)
{
    /// @note 高さ 5 に持ち上げた床。同じ落下でも接触は早くなる。
    const physics::CCDResult raised = physics::CCDSolver::SweptSpherePlane(
        { 0.0f, 10.0f, 0.0f }, 1.0f, { 0.0f, -200.0f, 0.0f },
        math::Vector3::UP, 5.0f, 0.1f);

    ASSERT_TRUE(raised.hit);
    EXPECT_NEAR(raised.toi, 0.2f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(raised.contactPoint, math::Vector3(0.0f, 5.0f, 0.0f),
                     testkit::kLooseTolerance);
}

/// @name 適用するかの判定

/// @brief Needs CCD only when a frame moves more than half the radius.
TEST_F(CCDTest, NeedsCCDOnlyWhenAFrameMovesMoreThanHalfTheRadius)
{
    /// @note しきい値は «1 フレームの移動量 > 半径 * 0.5»。ここを緩めると穴が開き、
    ///       締めすぎると全部の弾が CCD 経路へ入って重くなる。
    physics::RigidBody body;
    /// @note dt=1/60 で 0.0667 進む
    body.SetVelocity({ 0.0f, 0.0f, 4.0f });

    EXPECT_TRUE(physics::CCDSolver::NeedsCCD(body, 0.1f, testkit::kFixedDeltaTime));
    EXPECT_FALSE(physics::CCDSolver::NeedsCCD(body, 1.0f, testkit::kFixedDeltaTime));
}

/// @brief Does not need CCD while standing still.
TEST_F(CCDTest, DoesNotNeedCCDWhileStandingStill)
{
    physics::RigidBody body;

    EXPECT_FALSE(physics::CCDSolver::NeedsCCD(body, 0.5f, testkit::kFixedDeltaTime));
}

/// @brief Needs CCD is independent of the direction of travel.
TEST_F(CCDTest, NeedsCCDIsIndependentOfTheDirectionOfTravel)
{
    physics::RigidBody forward;
    physics::RigidBody backward;
    forward.SetVelocity({ 0.0f, 0.0f, 50.0f });
    backward.SetVelocity({ 0.0f, 0.0f, -50.0f });

    EXPECT_EQ(physics::CCDSolver::NeedsCCD(forward, 0.5f, testkit::kFixedDeltaTime),
              physics::CCDSolver::NeedsCCD(backward, 0.5f, testkit::kFixedDeltaTime));
}

} // namespace fbzz::tests
