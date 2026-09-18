/// @file    ColliderVolumeTests.cpp
/// @brief   Trigger コライダーを範囲として使う空間効果の «内外判定» と «効果の掛かり方» を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// Volume はゲーム側から見ると «そこへ入ると何かが起きる箱»。内外判定がずれれば効果が
/// 効かない場所ができ、力の入れ方を間違えれば «入った瞬間に吹き飛ぶ» か «何も起きない» に倒れる。
/// 特に、継続力が sleep を解除しないこと (ApplyForceNoWake) は World::Step の early-out が
/// 効くかどうかを直接決めるため、契約として固定する。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>

namespace fbzz::tests {
namespace {

/// 力は m_force に溜まるだけで外から読めない。dt = 1 / 質量 1 で 1 回積分すれば
/// 速度の増分がそのまま «加えられた力» になるので、効果の検証はこの形で行う。
constexpr float kUnitDt = 1.0f;

physics::RigidBody UnitBody(const math::Vector3& position)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.SetPosition(position);
    return body;
}

physics::VolumeSettings SettingsFor(physics::VolumeType type)
{
    physics::VolumeSettings settings;
    settings.type = type;
    return settings;
}

} // namespace

class ColliderVolumeTest : public testkit::Fixture {};

/// @name 内外判定

TEST_F(ColliderVolumeTest, ContainsIsFalseWithoutACollider)
{
    physics::ColliderVolume volume(nullptr, SettingsFor(physics::VolumeType::Gravity));

    /// @note 形状を失った Volume は «どこでも効く» ではなく «どこにも効かない» 側へ倒す。
    EXPECT_FALSE(volume.Contains(math::Vector3::ZERO));
}

TEST_F(ColliderVolumeTest, ContainsUsesTheSphereRadiusAroundItsWorldCenter)
{
    physics::SphereCollider sphere(2.0f);
    sphere.Update(math::Vector3(5.0f, 0.0f, 0.0f), math::Quaternion::Identity());
    physics::ColliderVolume volume(&sphere, SettingsFor(physics::VolumeType::Gravity));

    EXPECT_TRUE(volume.Contains(math::Vector3(6.0f, 0.0f, 0.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(7.5f, 0.0f, 0.0f)));
}

TEST_F(ColliderVolumeTest, ContainsTreatsTheAABBBoundaryAsInside)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::ColliderVolume volume(&box, SettingsFor(physics::VolumeType::Gravity));

    /// @note 面の上に立っている body を «外» にすると、境界で効果が点滅する。
    EXPECT_TRUE(volume.Contains(math::Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_FALSE(volume.Contains(math::Vector3(1.01f, 0.0f, 0.0f)));
}

TEST_F(ColliderVolumeTest, ContainsDelegatesToTheCylinderShape)
{
    physics::CylinderCollider cylinder(1.0f, 2.0f);
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::ColliderVolume volume(&cylinder, SettingsFor(physics::VolumeType::Gravity));

    EXPECT_TRUE(volume.Contains(math::Vector3(0.5f, 1.5f, 0.0f)));
    /// @note 高さは足りているが半径の外。円柱を外接箱で判定していれば取りこぼす点。
    EXPECT_FALSE(volume.Contains(math::Vector3(0.9f, 0.0f, 0.9f)));
}

TEST_F(ColliderVolumeTest, ContainsMeasuresDistanceToTheCapsuleSegment)
{
    physics::CapsuleCollider capsule(0.5f, 2.0f);
    capsule.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::ColliderVolume volume(&capsule, SettingsFor(physics::VolumeType::Gravity));

    /// @note 円柱部の側面内
    EXPECT_TRUE(volume.Contains(math::Vector3(0.4f, 1.0f, 0.0f)));
    /// @note 端の半球内
    EXPECT_TRUE(volume.Contains(math::Vector3(0.0f, 2.4f, 0.0f)));
    /// @note 端の半球の外
    EXPECT_FALSE(volume.Contains(math::Vector3(0.0f, 2.6f, 0.0f)));
}

/// @name 効果の適用

TEST_F(ColliderVolumeTest, GravityAppliesTheConfiguredAccelerationScaledByMass)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Gravity);
    settings.gravity = math::Vector3(0.0f, 4.0f, 0.0f);
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    body.SetMass(3.0f);
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note 力は質量倍で入るので、加速度は質量に依らず settings.gravity と一致する。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 4.0f, 0.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, GravityDoesNotWakeASleepingBody)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::ColliderVolume volume(&sphere, SettingsFor(physics::VolumeType::Gravity));

    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    body.Sleep();
    volume.Apply(body, kUnitDt);

    /// @note ここで起きてしまうと、Volume 内に静止した body が二度と Sleep できず
    ///       World::Step の early-out が永久に効かなくなる。
    EXPECT_TRUE(body.IsSleeping());
}

TEST_F(ColliderVolumeTest, ApplyIgnoresStaticBodies)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Explosion);
    settings.explosionImpulse = 50.0f;
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body;
    body.m_isStatic = true;
    body.SetMass(1.0f);
    body.SetPosition(math::Vector3(1.0f, 0.0f, 0.0f));
    volume.Apply(body, kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, VortexPushesTangentiallyAroundTheVerticalAxis)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Vortex);
    settings.swirlStrength  = 2.0f;
    settings.inwardStrength = 0.0f;
    settings.liftStrength   = 0.0f;
    physics::ColliderVolume volume(&sphere, settings);

    /// @note 中心の +X 側にいる body から見て中心方向は -X。接線は (-inward.z, 0, inward.x) = (0,0,-1)。
    physics::RigidBody body = UnitBody(math::Vector3(1.0f, 0.0f, 0.0f));
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 0.0f, -2.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, VortexLiftsWithoutASwirlDirectionAtTheCenter)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Vortex);
    settings.swirlStrength  = 5.0f;
    settings.inwardStrength = 5.0f;
    settings.liftStrength   = 3.0f;
    physics::ColliderVolume volume(&sphere, settings);

    /// @note 軸上では内向きベクトルが決まらない。ここで正規化を通すと NaN が全身へ広がるため、
    ///       水平成分を 0 に落として上昇分だけ残すことを固定する。
    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 3.0f, 0.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, ExplosionPushesOutwardAsAnImmediateImpulse)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Explosion);
    settings.explosionImpulse = 7.0f;
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body = UnitBody(math::Vector3(0.0f, 0.0f, 4.0f));
    volume.Apply(body, kUnitDt);

    /// @note インパルスなので積分を待たずに速度へ乗る。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 0.0f, 7.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, ExplosionPushesUpwardForABodyAtTheCenter)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Explosion);
    settings.explosionImpulse = 7.0f;
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    volume.Apply(body, kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 7.0f, 0.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, TimeDilationAppliesNoForce)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::TimeDilation);
    settings.timeScale = 0.25f;
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body = UnitBody(math::Vector3(1.0f, 0.0f, 0.0f));
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note 減速は World が dt をスケールして行う。Volume 側で力を足すと二重に効く。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, MagneticAppliesLorentzForceScaledByCharge)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Magnetic);
    settings.magneticField = math::Vector3(0.0f, 1.0f, 0.0f);
    physics::ColliderVolume volume(&sphere, settings);

    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    body.m_charge = 2.0f;
    body.SetVelocity(math::Vector3(1.0f, 0.0f, 0.0f));
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    /// @note F = q (v × B) = 2 * ((1,0,0) × (0,1,0)) = (0,0,2)
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(1.0f, 0.0f, 2.0f), testkit::kTolerance);
}

TEST_F(ColliderVolumeTest, MagneticAppliesNothingToAnUnchargedBody)
{
    physics::SphereCollider sphere(10.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    physics::ColliderVolume volume(&sphere, SettingsFor(physics::VolumeType::Magnetic));

    physics::RigidBody body = UnitBody(math::Vector3::ZERO);
    body.SetVelocity(math::Vector3(1.0f, 0.0f, 0.0f));
    volume.Apply(body, kUnitDt);
    body.Integrate(kUnitDt);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(1.0f, 0.0f, 0.0f), testkit::kTolerance);
}

/// @name World へ返す問い合わせ

TEST_F(ColliderVolumeTest, TimeScaleIsNeutralForEveryTypeExceptTimeDilation)
{
    physics::VolumeSettings dilation = SettingsFor(physics::VolumeType::TimeDilation);
    dilation.timeScale = 0.5f;
    physics::VolumeSettings gravity = SettingsFor(physics::VolumeType::Gravity);
    gravity.timeScale = 0.5f;

    EXPECT_FLOAT_EQ(physics::ColliderVolume(nullptr, dilation).GetTimeScale(), 0.5f);
    /// @note timeScale が設定されていても、型が違えば dt を触らせない。
    EXPECT_FLOAT_EQ(physics::ColliderVolume(nullptr, gravity).GetTimeScale(), 1.0f);
}

TEST_F(ColliderVolumeTest, OnlyTheGravityTypeOverridesWorldGravity)
{
    EXPECT_TRUE(physics::ColliderVolume(nullptr, SettingsFor(physics::VolumeType::Gravity))
                    .OverridesGravity());
    EXPECT_FALSE(physics::ColliderVolume(nullptr, SettingsFor(physics::VolumeType::Vortex))
                     .OverridesGravity());
}

TEST_F(ColliderVolumeTest, NeverExpiresForANegativeDuration)
{
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Gravity);
    settings.duration = -1.0f;
    physics::ColliderVolume volume(nullptr, settings);

    volume.Tick(1000.0f);

    EXPECT_FALSE(volume.IsExpired());
}

TEST_F(ColliderVolumeTest, ExpiresOnceTickedPastItsDuration)
{
    physics::VolumeSettings settings = SettingsFor(physics::VolumeType::Explosion);
    settings.duration = 0.1f;
    physics::ColliderVolume volume(nullptr, settings);

    volume.Tick(0.05f);
    EXPECT_FALSE(volume.IsExpired());

    volume.Tick(0.05f);
    EXPECT_TRUE(volume.IsExpired());
}

} // namespace fbzz::tests
