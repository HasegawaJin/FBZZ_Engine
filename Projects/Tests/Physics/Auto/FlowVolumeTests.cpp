/// @file    FlowVolumeTests.cpp
/// @brief   FlowVolume の «誰が流れを受けるか» と結合式 F = k*m*(v_flow - v) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// 流速は固定値を返す callback で与える。FlowField も風も持ち込まずに結合だけを試せるのが、
/// 流速を std::function で受ける形にした理由そのもの。
/// @see Docs/design/flow-field.md §2 / §9-6
#include <TestKit/TestKit.hpp>

#include <Math/Vector3.hpp>
#include <Physics/FlowDrag.hpp>
#include <Physics/FlowVolume.hpp>
#include <Physics/RigidBody.hpp>
#include <utility>

namespace fbzz::tests {
namespace {

/// 力は m_force に溜まるだけで外から読めない。dt = 1 で 1 回積分すれば速度の増分が
/// «加えられた力 / 質量» になるので、効果の検証はこの形で行う。
constexpr float kUnitDt = 1.0f;

constexpr float kMass     = 2.0f;  ///< 質量 [kg]
constexpr float kCoupling = 0.5f;  ///< 結合係数 [1/s]

const math::Vector3 kFlow{ 3.0f, 0.0f, -1.0f }; ///< 一様流 [m/s]

physics::RigidBody MakeBody(float coupling)
{
    physics::RigidBody body;
    body.SetMass(kMass);
    body.SetFlowCoupling(coupling);
    return body;
}

/// どこでも同じ速度を返す流れ。
physics::FlowVolumeDesc UniformFlow(const math::Vector3& flow)
{
    physics::FlowVolumeDesc desc;
    desc.flowVelocity = [flow](const math::Vector3&) { return flow; };
    return desc;
}

} // namespace

class FlowVolumeTest : public testkit::Fixture {};

/// @name 誰が受けるか

TEST_F(FlowVolumeTest, LeavesABodyWithoutCouplingUntouched)
{
    physics::RigidBody body = MakeBody(0.0f);
    physics::FlowVolume volume(UniformFlow(kFlow));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note 既定は «流れを受けない»。宣言しない限り、置いてある物は風で動かない。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(FlowVolumeTest, LeavesAStaticBodyUntouched)
{
    physics::RigidBody body = MakeBody(kCoupling);
    body.m_isStatic = true;
    physics::FlowVolume volume(UniformFlow(kFlow));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(FlowVolumeTest, ContainsIsTrueEverywhere)
{
    physics::FlowVolume volume(UniformFlow(kFlow));

    /// @note 半径も減衰も場の側 (SampleFlow) が持つ。この Volume は器なので内外を判定しない。
    EXPECT_TRUE(volume.Contains(math::Vector3::ZERO));
    EXPECT_TRUE(volume.Contains(math::Vector3(1.0e4f, -1.0e4f, 1.0e4f)));
}

/// @name 結合

TEST_F(FlowVolumeTest, AcceleratesARestingBodyAlongTheFlow)
{
    physics::RigidBody body = MakeBody(kCoupling);
    physics::FlowVolume volume(UniformFlow(kFlow));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note F = k*m*(v_flow - 0) なので、加速度は質量に依らず k*v_flow。
    EXPECT_VEC3_NEAR(body.GetVelocity(), kFlow * (kCoupling * kUnitDt), testkit::kTolerance);
}

TEST_F(FlowVolumeTest, ConvergesToTheFlowVelocity)
{
    constexpr float kDt    = 0.02f;  ///< 固定ステップ [s]
    constexpr int   kSteps = 2000;   ///< 40 秒ぶん。残差は (1 - k*dt)^n = 0.99^2000 ~ 2e-9
    /// 許容: 1 ステップぶんの残差より十分大きく、収束を «ほぼ一致» と呼べる幅。
    constexpr float kConvergedTolerance = 1.0e-3f;

    physics::RigidBody body = MakeBody(kCoupling);
    physics::FlowVolume volume(UniformFlow(kFlow));

    for (int i = 0; i < kSteps; ++i) {
        volume.Apply(body, kDt);
        body.Integrate(kDt);
    }

    /// @note 流れとの速度差にだけ力が掛かるので、止まる先は v_flow ちょうど。
    EXPECT_VEC3_NEAR(body.GetVelocity(), kFlow, kConvergedTolerance);
}

TEST_F(FlowVolumeTest, DoesNotPushABodyAlreadyMovingWithTheFlow)
{
    physics::RigidBody body = MakeBody(kCoupling);
    body.SetVelocity(kFlow);
    physics::FlowVolume volume(UniformFlow(kFlow));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), kFlow, testkit::kTolerance);
}

/// @name 共有している式

TEST_F(FlowVolumeTest, SharesTheCouplingFormulaWithFluidVolume)
{
    const math::Vector3 velocity{ 1.0f, 2.0f, 3.0f };

    /// @note FluidVolume が «沈み率で重み付けした結合» として呼ぶのと同じ関数。
    ///       完全に沈んだ (submersion = 1) ときは FlowVolume とまったく同じ力になる。
    const math::Vector3 expected = (kFlow - velocity) * (kCoupling * kMass);
    EXPECT_VEC3_NEAR(physics::FlowDragForce(kCoupling, kMass, kFlow, velocity),
                     expected, testkit::kTolerance);

    /// @note 沈み率は結合係数側に掛ける。FluidVolumeTests の
    ///       DragCarriesARestingBodyAlongTheFlow が見ている «静止した体 + submersion 1» の式。
    EXPECT_VEC3_NEAR(physics::FlowDragForce(kCoupling * 1.0f, kMass, kFlow, math::Vector3::ZERO),
                     kFlow * (kCoupling * kMass), testkit::kTolerance);
}

} // namespace fbzz::tests
