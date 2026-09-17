/// @file    FluidVolumeTests.cpp
/// @brief   FluidVolume の «どこまでが流体か» と «沈み率から出る力» の式を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @brief 表面は平らな callback で与える。波を巻き込まずに法則だけを試せるのが、表面を
/// @brief std::function で受ける形にした理由そのもの。密度ベースの法則へ置き換えるまでの間、
/// @brief ここが «等価球 4 点» の式の回帰の網になる。
/// @see Docs/design/buoyancy.md
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/FluidVolume.hpp>
#include <Physics/RigidBody.hpp>
#include <cmath>
#include <utility>

namespace fbzz::tests {
namespace {

/// @brief 力は m_force に溜まるだけで外から読めない。dt = 1 で 1 回積分すれば速度の増分が
/// @brief «加えられた力 / 質量» になるので、効果の検証はこの形で行う。
constexpr float kUnitDt = 1.0f;

constexpr float kRadius     = 1.0f;   ///< @brief 等価球の半径 [m]
constexpr float kMass       = 2.0f;   ///< @brief 質量 [kg]
constexpr float kBuoyancy   = 6.0f;   ///< @brief 完全に沈んだときの上向き加速度 [m/s^2]
constexpr float kDepthLimit = 10.0f;  ///< @brief 浮力が届く表面からの深さ [m]
constexpr float kHalfExtent = 100.0f; ///< @brief 矩形の半幅 [m]

physics::RigidBody MakeBody(const math::Vector3& position)
{
    physics::RigidBody body;
    body.SetMass(kMass);
    body.SetPosition(position);
    return body;
}

/// @brief y = 0 の平らな表面を持つ流体。半径表には body 1 つだけを載せる。
physics::FluidVolumeDesc FlatFluid(const physics::RigidBody& body)
{
    physics::BodyVolumeMap volumes;
    volumes[&body] = 4.0f / 3.0f * math::PI * kRadius * kRadius * kRadius;

    physics::FluidVolumeDesc desc;
    desc.surfaceHeight = [](float, float, float) { return 0.0f; };
    desc.center     = math::Vector3::ZERO;
    desc.halfX      = kHalfExtent;
    desc.halfZ      = kHalfExtent;
    desc.buoyancy   = kBuoyancy;
    desc.drag       = 0.0f;
    desc.depthLimit = kDepthLimit;
    desc.radii      = physics::MakeBodyRadii(volumes);
    return desc;
}

} // namespace

class FluidVolumeTest : public testkit::Fixture {};

TEST_F(FluidVolumeTest, HeightBandSkipsOnlyUnambiguousQueries)
{
    auto body = MakeBody({ 0.0f, -5.0f, 0.0f });
    auto desc = FlatFluid(body);
    int calls = 0;
    desc.surfaceHeight = [&calls](float, float, float) { ++calls; return 0.25f; };
    desc.surfaceHeightBound = 0.5f;
    physics::FluidVolume volume(desc);
    EXPECT_FALSE(volume.Contains({ 0.0f, 2.0f, 0.0f }));
    EXPECT_FALSE(volume.Contains({ 0.0f, -11.0f, 0.0f }));
    EXPECT_TRUE(volume.Contains(body.GetPosition()));
    volume.Apply(body, kUnitDt);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(volume.Contains({ 0.0f, 1.1f, 0.0f }));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(volume.Contains({ 0.0f, -10.0f, 0.0f }));
    EXPECT_EQ(calls, 2);
}

TEST_F(FluidVolumeTest, UnknownHeightBoundKeepsTheExactCallback)
{
    auto body = MakeBody({ 0.0f, 80.0f, 0.0f });
    auto desc = FlatFluid(body);
    desc.surfaceHeight = [](float, float, float) { return 85.0f; };
    physics::FluidVolume volume(desc);
    EXPECT_TRUE(volume.Contains(body.GetPosition()));
}

/// @name 揚力

TEST_F(FluidVolumeTest, LiftsAFullySubmergedBodyByTheConfiguredAcceleration)
{
    physics::RigidBody body = MakeBody(math::Vector3(0.0f, -5.0f, 0.0f));
    physics::FluidVolume volume(FlatFluid(body));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note 4 点とも沈み率 1 なので、合計の揚力は buoyancy * mass。
    ///       質量倍で入るため、加速度は質量に依らず buoyancy と一致する。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, kBuoyancy, 0.0f), testkit::kTolerance);
    /// @note 水平 4 点の沈み率が等しければトルクは打ち消し合う。
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(FluidVolumeTest, AppliesATorqueThatRestoresTheHorizontalForATiltedBody)
{
    constexpr float kTiltDeg = 30.0f;
    physics::RigidBody body = MakeBody(math::Vector3::ZERO);
    body.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3(0.0f, 0.0f, 1.0f),
                                                     math::ToRad(kTiltDeg)));
    physics::FluidVolume volume(FlatFluid(body));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note +Z 側と -Z 側の腕は傾けても高さが変わらず打ち消し合う。残るのは ±X の差だけで、
    ///       4 点の和をまとめると torque_z = -buoyancy * mass * 0.25 * arm^2 * sin t * cos t。
    ///       コライダー未設定の剛体の慣性は等方 (1/m) なので、角速度は torque / mass。
    const float arm = kRadius * 0.6f;
    const float sinTilt = std::sin(math::ToRad(kTiltDeg));
    const float cosTilt = std::cos(math::ToRad(kTiltDeg));
    const float torqueZ = -kBuoyancy * kMass * 0.25f * arm * arm * sinTilt * cosTilt;

    EXPECT_VEC3_NEAR(body.GetAngularVelocity(),
                     math::Vector3(0.0f, 0.0f, torqueZ / kMass), testkit::kTolerance);
    /// @note +X 側が浮き上がった傾きなので、戻す向き (Z 負) でなければならない。
    EXPECT_LT(body.GetAngularVelocity().z, 0.0f);
}

/// @name 流れの抵抗

TEST_F(FluidVolumeTest, DragCarriesARestingBodyAlongTheFlow)
{
    constexpr float kDrag = 0.5f;
    const math::Vector3 flow(3.0f, 0.0f, 0.0f);

    physics::RigidBody body = MakeBody(math::Vector3(0.0f, -5.0f, 0.0f));
    physics::FluidVolumeDesc desc = FlatFluid(body);
    desc.buoyancy = 0.0f;
    desc.drag = kDrag;
    desc.flowVelocity = [flow](const math::Vector3&) { return flow; };
    physics::FluidVolume volume(std::move(desc));

    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note 抵抗は «流体に対する» 速度に掛かる。静止した体には流れがそのまま推進力になる。
    ///       F = (flow - v) * drag * mass * submersion、submersion は完全に沈んで 1。
    EXPECT_VEC3_NEAR(body.GetVelocity(), flow * kDrag * kUnitDt, testkit::kTolerance);
}

/// @name 内外判定

TEST_F(FluidVolumeTest, ContainsSpansFromOneBodyRadiusAboveTheSurfaceToTheDepthLimit)
{
    physics::RigidBody body = MakeBody(math::Vector3::ZERO);
    physics::FluidVolume volume(FlatFluid(body));

    /// @note 重心が表面より上でも、体の下側が浸かっていれば浮力は掛かる。
    EXPECT_TRUE(volume.Contains(math::Vector3(0.0f, kRadius * 0.5f, 0.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(0.0f, kRadius * 1.5f, 0.0f)));

    /// @note 表面は厚みを持たない板なので、下限が無いと真下の洞窟まで浮力が届く。
    EXPECT_TRUE(volume.Contains(math::Vector3(0.0f, -kDepthLimit + 0.5f, 0.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(0.0f, -kDepthLimit - 0.5f, 0.0f)));
}

TEST_F(FluidVolumeTest, ContainsIsFalseOutsideTheRectangle)
{
    physics::RigidBody body = MakeBody(math::Vector3::ZERO);
    physics::FluidVolume volume(FlatFluid(body));

    EXPECT_TRUE(volume.Contains(math::Vector3(kHalfExtent - 1.0f, 0.0f, kHalfExtent - 1.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(kHalfExtent + 1.0f, 0.0f, 0.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(0.0f, 0.0f, kHalfExtent + 1.0f)));
}

} // namespace fbzz::tests
