/// @file    RigidBodySerializerTests.cpp
/// @brief   剛体の TOML 保存・復元が往復し、欠けたキーと壊れた入力に耐えることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 「保存して読んだら同じ」はシーンで最も事故が多く、かつ最も自動化しやすい契約。
/// 項目を足したときに書き忘れると、保存はできるのに開くと既定値へ戻る ──
/// エディターで作業した内容が «静かに» 消える形で出る。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/RigidBodySerializer.hpp>

#include <string>

namespace fbzz::tests {
namespace {

/// 既定値と全部違う剛体。«書き忘れたキー» を既定値との一致で炙り出す。
physics::RigidBody MakeDistinctBody()
{
    physics::RigidBody body;
    body.SetMass(7.5f);
    body.SetPosition({ 1.0f, -2.0f, 3.0f });
    body.SetVelocity({ 4.0f, 5.0f, -6.0f });
    body.SetRotation(math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f)));
    body.SetAngularVelocity({ 0.5f, -1.5f, 2.5f });
    body.m_charge                = 3.0f;
    body.m_isGravitationalSource = true;
    body.m_gravitationalMass     = 12.0f;
    return body;
}

} // namespace

class RigidBodySerializerTest : public testkit::Fixture {};

/// @name 往復

TEST_F(RigidBodySerializerTest, RestoresTheStateItSaved)
{
    const physics::RigidBody original = MakeDistinctBody();

    physics::RigidBody restored;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(
        restored, physics::RigidBodySerializer::Serialize(original)));

    EXPECT_VEC3_NEAR(restored.GetPosition(), original.GetPosition(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored.GetVelocity(), original.GetVelocity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored.GetAngularVelocity(), original.GetAngularVelocity(),
                     testkit::kTolerance);
    EXPECT_QUAT_NEAR(restored.GetRotation(), original.GetRotation(), testkit::kTolerance);
}

TEST_F(RigidBodySerializerTest, RestoresTheSettingsItSaved)
{
    const physics::RigidBody original = MakeDistinctBody();

    physics::RigidBody restored;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(
        restored, physics::RigidBodySerializer::Serialize(original)));

    EXPECT_NEAR(restored.GetMass(), original.GetMass(), testkit::kTolerance);
    EXPECT_NEAR(restored.m_charge, original.m_charge, testkit::kTolerance);
    EXPECT_EQ(restored.m_isGravitationalSource, original.m_isGravitationalSource);
    EXPECT_NEAR(restored.m_gravitationalMass, original.m_gravitationalMass,
                testkit::kTolerance);
}

TEST_F(RigidBodySerializerTest, RestoresTheStaticFlag)
{
    physics::RigidBody original;
    original.m_isStatic = true;
    original.SetMass(3.0f);

    physics::RigidBody restored;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(
        restored, physics::RigidBodySerializer::Serialize(original)));

    EXPECT_TRUE(restored.IsStatic());
    EXPECT_NEAR(restored.GetInvMass(), 0.0f, testkit::kTolerance);
}

TEST_F(RigidBodySerializerTest, SavingTwiceProducesTheSameText)
{
    /// @note 出力が安定していないと、何も触っていないのに .scene の差分が出る。
    const physics::RigidBody original = MakeDistinctBody();

    EXPECT_EQ(physics::RigidBodySerializer::Serialize(original),
              physics::RigidBodySerializer::Serialize(original));
}

TEST_F(RigidBodySerializerTest, ASecondRoundTripChangesNothing)
{
    const std::string first = physics::RigidBodySerializer::Serialize(MakeDistinctBody());

    physics::RigidBody restored;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(restored, first));

    EXPECT_EQ(physics::RigidBodySerializer::Serialize(restored), first);
}

TEST_F(RigidBodySerializerTest, InverseMassFollowsTheRestoredMass)
{
    physics::RigidBody original;
    original.SetMass(4.0f);

    physics::RigidBody restored;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(
        restored, physics::RigidBodySerializer::Serialize(original)));

    /// @note 質量だけ書き戻して逆質量を計算し直さないと、重さを変えても押した手応えが変わらない。
    EXPECT_NEAR(restored.GetInvMass(), 0.25f, testkit::kTolerance);
}

/// @name 欠けた入力・壊れた入力

TEST_F(RigidBodySerializerTest, FillsMissingKeysWithDefaults)
{
    /// @note 古いシーンには新しいキーが無い。読めなくなるのではなく既定値で埋める。
    physics::RigidBody body;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(body, "mass = 2.0\n"));

    EXPECT_NEAR(body.GetMass(), 2.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_FALSE(body.IsStatic());
}

TEST_F(RigidBodySerializerTest, AcceptsAnEmptyDocument)
{
    physics::RigidBody body;

    EXPECT_TRUE(physics::RigidBodySerializer::Deserialize(body, ""));
    EXPECT_NEAR(body.GetMass(), 1.0f, testkit::kTolerance);
}

TEST_F(RigidBodySerializerTest, RejectsTextThatIsNotToml)
{
    /// @note 壊れたファイルは «読めなかった» と答える。既定値で上書きして
    ///       「開いたら全部リセットされていた」にしない。
    physics::RigidBody body;
    body.SetMass(5.0f);

    EXPECT_FALSE(physics::RigidBodySerializer::Deserialize(body, "{{{ not toml ]]]"));
}

TEST_F(RigidBodySerializerTest, KeepsTheRotationNormalised)
{
    /// @note 保存された成分は丸められている。読んだまま使うと回転行列がスケールを持つ。
    physics::RigidBody body;
    ASSERT_TRUE(physics::RigidBodySerializer::Deserialize(
        body, "rotation = [0.0, 0.0, 0.0, 4.0]\n"));

    EXPECT_NEAR(body.GetRotation().Length(), 1.0f, testkit::kLooseTolerance);
}

} // namespace fbzz::tests
