/// @file    RagdollRigTests.cpp
/// @brief   骨の並び ↔ 剛体の往復と、プロファイルの割り当てを自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
/// @note シーンを使わず、骨のワールド姿勢配列で捕獲と書き戻しの往復を検証する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <Physics/OBBCollider.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::tests {

class RagdollRigTest : public testkit::EngineFixture {};

namespace {

/// @brief 原点から +Y へ 1m 刻みで伸びる骨を作る。
/// @note 既定の 4 本では葉を除く 3 剛体と 2 関節になる。
std::vector<scene::RagdollBonePose> StraightChain(int boneCount = 4)
{
    std::vector<scene::RagdollBonePose> bones;
    for (int i = 0; i < boneCount; ++i) {
        scene::RagdollBonePose bone;
        bone.parent   = i - 1;
        bone.name     = "Bone" + std::to_string(i);
        bone.position = { 0.0f, static_cast<float>(i), 0.0f };
        bone.rotation = math::Quaternion::Identity();
        bones.push_back(bone);
    }
    return bones;
}

/// @brief 原点から +X へ 1m 刻みで伸びる骨を作る。
/// @note 縦の鎖と異なり、重力が関節を曲げるのでサーボの荷重支持を検証できる。
std::vector<scene::RagdollBonePose> HorizontalChain(int boneCount = 4)
{
    std::vector<scene::RagdollBonePose> bones;
    for (int i = 0; i < boneCount; ++i) {
        scene::RagdollBonePose bone;
        bone.parent   = i - 1;
        bone.name     = "Bone" + std::to_string(i);
        bone.position = { static_cast<float>(i), 0.0f, 0.0f };
        bone.rotation = math::Quaternion::Identity();
        bones.push_back(bone);
    }
    return bones;
}

scene::RagdollProfile SoftProfile()
{
    scene::RagdollProfile profile;
    /// @note 節の長さ 1m の鎖に対して半径 0.1m になる比。
    profile.fallback.radiusRatio = 0.1f;
    profile.fallback.radiusMin   = 0.1f;
    profile.fallback.density     = 1000.0f;
    profile.fallback.limits.enabled = false;
    profile.fallback.servo.enabled  = false;
    return profile;
}

float MaxPositionError(const std::vector<scene::RagdollBonePose>& expected,
                       const std::vector<math::Vector3>&          actual)
{
    float worst = 0.0f;
    for (std::size_t i = 0; i < expected.size(); ++i)
        worst = std::max(worst, (expected[i].position - actual[i]).Length());
    return worst;
}

float MaxRotationError(const std::vector<scene::RagdollBonePose>& expected,
                       const std::vector<math::Quaternion>&       actual)
{
    float worst = 0.0f;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const float dot = std::abs(math::Quaternion::Dot(expected[i].rotation, actual[i]));
        worst = std::max(worst, 2.0f * std::acos(std::min(dot, 1.0f)));
    }
    return worst;
}

}

TEST_F(RagdollRigTest, PlayerHumanoidProfileHandlesReportedBonesWithoutFallback)
{
    const auto profile = scene::RagdollProfile::Humanoid();
    const char* accessories[] = {
        "Mount_Back", "Ant_A", "Ant_B", "Grip_L", "Grip_R",
        "F1A_L", "F2A_L", "F3A_L", "ThA_L", "F1B_L", "F2B_L", "F3B_L", "ThB_L",
        "F1A_R", "F2A_R", "F3A_R", "ThA_R", "F1B_R", "F2B_R", "F3B_R", "ThB_R"
    };
    for (const char* name : accessories) EXPECT_TRUE(profile.IsExcluded(name)) << name;
    for (const char* name : { "Hand_L", "Hand_R" }) {
        bool matched = false;
        const auto& hand = profile.Resolve(name, &matched);
        EXPECT_TRUE(matched) << name;
        EXPECT_FALSE(profile.IsExcluded(name));
        EXPECT_TRUE(hand.limits.enabled);
        EXPECT_LT(hand.limits.swingMaxY, profile.fallback.limits.swingMaxY);
    }
    bool matched = true;
    (void)profile.Resolve("UnknownAppendage", &matched);
    EXPECT_FALSE(matched);
    EXPECT_FALSE(profile.IsExcluded("UnknownAppendage"));
    EXPECT_FALSE(profile.IsExcluded("ForeArm_L"));
    EXPECT_FALSE(profile.IsExcluded("Giant_Arm"));
    EXPECT_TRUE(profile.IsExcluded("ant_a"));
}

TEST_F(RagdollRigTest, BuildsOneBodyPerBoneThatHasAChild)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());

    /// @note 葉は剛体を持たない
    EXPECT_EQ(rig.GetBodyCount(), 3);
    /// @note 根の剛体はワールドへ繋がない
    EXPECT_EQ(rig.GetJointCount(), 2);
    EXPECT_GE(rig.BodyIndexOfBone(0), 0);
    EXPECT_EQ(rig.BodyIndexOfBone(3), -1);
}

/// @note 物理ステップなしの捕獲と書き戻しでは元の姿勢と一致すること。
TEST_F(RagdollRigTest, CaptureThenWriteReproducesThePose)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    EXPECT_LT(MaxPositionError(bones, positions), 1.0e-4f);
    EXPECT_LT(MaxRotationError(bones, rotations), 1.0e-3f);
}

/// @note 構築時と異なる途中の姿勢を捕獲しても往復できること。
TEST_F(RagdollRigTest, CaptureReproducesAPoseDifferentFromTheBuildPose)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());

    /// @note 途中で折れ曲がった姿勢を作る。親の回転を子の位置へ積んで «関節が曲がった» 形にする。
    auto bent = bones;
    const math::Quaternion bend = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 0.6f);
    for (std::size_t i = 2; i < bent.size(); ++i) {
        bent[i].rotation = (bend * bent[i].rotation).Normalized();
        bent[i].position = bent[1].position + bend * (bones[i].position - bones[1].position);
    }

    rig.Capture(bent);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bent, positions, rotations);

    EXPECT_LT(MaxPositionError(bent, positions), 1.0e-4f);
    EXPECT_LT(MaxRotationError(bent, rotations), 1.0e-3f);
}

/// @note 剛体を持たない葉の骨も親に追従すること。
TEST_F(RagdollRigTest, LeafBoneFollowsItsParentBody)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.Solver().SetGravity({ 0.0f, -9.81f, 0.0f });
    for (int i = 0; i < 30; ++i) rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    /// @note 落ちたので葉も元の位置には居ない。
    EXPECT_GT((positions[3] - bones[3].position).Length(), 0.05f);
    /// @note それでも親との距離は保たれている (骨は伸びない)。
    EXPECT_NEAR((positions[3] - positions[2]).Length(),
                (bones[3].position - bones[2].position).Length(), 0.05f);
}

TEST_F(RagdollRigTest, JointsKeepTheChainConnectedWhileFalling)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.Solver().SetGravity({ 0.0f, -9.81f, 0.0f });
    for (int i = 0; i < 120; ++i) rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    for (std::size_t i = 1; i < bones.size(); ++i) {
        const float restLength = (bones[i].position - bones[i - 1].position).Length();
        EXPECT_NEAR((positions[i] - positions[i - 1]).Length(), restLength, 0.02f)
            << "bone " << i << " stretched";
    }
}

/// @note 現在の骨からドライブ目標を取り直すと、無負荷の釣り合い点はその姿勢になる。
TEST_F(RagdollRigTest, DriveHoldsTheAnimationPoseUnderGravity)
{
    const auto bones = StraightChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.servo.enabled     = true;
    profile.fallback.servo.torqueScale = 20.0f;
    profile.fallback.servo.holdSag     = 0.02f;
    profile.fallback.servo.damping     = 30.0f;

    scene::RagdollRig rig;
    rig.Build(bones, profile);
    rig.Capture(bones);
    rig.UpdateDriveTargets(bones);
    rig.Solver().SetGravity({ 0.0f, -9.81f, 0.0f });

    for (int i = 0; i < 120; ++i) {
        rig.UpdateDriveTargets(bones);
        rig.Step(1.0f / 60.0f);
    }

    /// @note 根は自由落下するため、根の移動を除いた姿勢のずれで関節の垂れを検証する。
    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    const math::Vector3 drift = positions[0] - bones[0].position;
    for (std::size_t i = 1; i < bones.size(); ++i) {
        const math::Vector3 expected = bones[i].position + drift;
        EXPECT_LT((positions[i] - expected).Length(), 0.05f) << "bone " << i << " sagged";
    }
}

/// @note 接触の簡易平面でも床を抜けないこと。
TEST_F(RagdollRigTest, GroundPlaneStopsTheFallingChain)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetGravity({ 0.0f, -9.81f, 0.0f });
    rig.SetGround(-2.0f, 0.9f);
    for (int i = 0; i < 240; ++i) rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    /// @note カプセルの端 ＋ 半径がちょうど骨の位置なので、骨が床より下へ出たら抜けている。
    for (std::size_t i = 0; i < positions.size(); ++i)
        EXPECT_GT(positions[i].y, -2.15f) << "bone " << i << " fell through the ground";
}

/// @note 同じ荷重で低いトルク上限の関節ほど垂れ、出力が飽和すること。
TEST_F(RagdollRigTest, LowerTorqueLimitSagsMoreAndSaturates)
{
    const auto bones = HorizontalChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.servo.enabled     = true;
    /// @note 自重の 3 倍まで支えられるサーボ。倍率なので、鎖の重さが変わっても意味が変わらない。
    profile.fallback.servo.torqueScale = 3.0f;
    profile.fallback.servo.holdSag     = 0.02f;
    profile.fallback.servo.damping     = 20.0f;

    /// @note 一様な自由落下では関節に荷重が掛からないため、根を固定してトルク上限を検証する。
    const auto sagWith = [&](float driveScale, int& outSaturated) {
        scene::RagdollRig rig;
        rig.Build(bones, profile);
        rig.Capture(bones);
        rig.GetBody(0)->m_isStatic = true;
        rig.SetDrive(true, driveScale, 1.0f, 1.0f);
        rig.SetGravity({ 0.0f, -9.81f, 0.0f });

        /// @note 垂れ切るとトルク不要になるため、最終フレームではなく飽和履歴を検証する。
        outSaturated = 0;
        for (int i = 0; i < 120; ++i) {
            rig.UpdateDriveTargets(bones);
            rig.Step(1.0f / 60.0f);
            outSaturated = std::max(outSaturated, rig.CountSaturatedJoints());
        }

        std::vector<math::Vector3>    positions;
        std::vector<math::Quaternion> rotations;
        rig.WritePose(bones, positions, rotations);

        float worst = 0.0f;
        for (std::size_t i = 1; i < bones.size(); ++i)
            worst = std::max(worst, (positions[i] - bones[i].position).Length());
        return worst;
    };

    int   strongSaturated = 0;
    int   weakSaturated   = 0;
    const float strongSag = sagWith(1.0f,  strongSaturated);
    const float weakSag   = sagWith(0.01f, weakSaturated);

    /// @note 出力が足りていれば形は保たれる
    EXPECT_LT(strongSag, 0.10f);
    /// @note 足りなければ垂れる
    EXPECT_GT(weakSag, strongSag * 3.0f);
    EXPECT_EQ(strongSaturated, 0);
    /// @note 上限に張り付いた関節が居る ＝ 力負け
    EXPECT_GT(weakSaturated, 0);
}

TEST_F(RagdollRigTest, ImpulsePushesEveryBody)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetGravity(math::Vector3::ZERO);
    rig.ApplyImpulse(math::Vector3::ZERO, { 5.0f, 0.0f, 0.0f }, 0.0f);
    rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    /// @note 5 m/s で 1/60 秒 ＝ 8cm 強。全身が一律に動くので鎖は伸びない。
    for (std::size_t i = 0; i < positions.size(); ++i)
        EXPECT_NEAR(positions[i].x - bones[i].position.x, 5.0f / 60.0f, 0.02f)
            << "bone " << i;
}

/// @note 簡易平面ではなく、World コライダーと NarrowPhase の接触で落下を止めること。
TEST_F(RagdollRigTest, WorldColliderStopsTheFall)
{
    const auto bones = StraightChain(4);

    /// @note 剛体のない静的コライダーは World が更新しないため、天板 y = 0 の姿勢を明示する。
    physics::OBBCollider floor{ { 20.0f, 1.0f, 20.0f } };
    floor.Update({ 0.0f, -1.0f, 0.0f }, math::Quaternion::Identity());
    const physics::PhysicsMaterial material;

    physics::World world;
    world.BeginSceneSync();
    world.SyncCollider({}, physics::ColliderInstance{
        &floor, nullptr, &material, math::Vector3::ZERO, false, 0 });
    world.EndSceneSync();

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetGravity({ 0.0f, -9.81f, 0.0f });
    /// @note 抜け止めの平面を無効化し、世界の接触だけで受け止める
    rig.DisableGround();

    for (int i = 0; i < 240; ++i) {
        rig.RefreshContacts(&world);
        rig.Step(1.0f / 60.0f);
        rig.ApplyContactReactions();
    }

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    EXPECT_GT(rig.GetContactCount(), 0) << "no contact was generated against the world";
    for (std::size_t i = 0; i < positions.size(); ++i)
        EXPECT_GT(positions[i].y, -0.2f) << "bone " << i << " fell through the world collider";
}

/// @note World が積分する剛体を substep で動かさず、フレーム末の反作用の力積で動かすこと。
TEST_F(RagdollRigTest, FallingRagdollKicksADynamicBody)
{
    const auto bones = StraightChain(4);

    physics::RigidBody debris;
    debris.SetPosition({ 0.0f, -0.6f, 0.0f });
    debris.SetMass(5.0f);
    /// @note World::Step を回さないので自分では落ちない
    debris.m_useGravity = false;

    physics::OBBCollider box{ { 1.0f, 0.5f, 1.0f } };
    box.Update(debris.GetPosition(), debris.GetRotation());
    const physics::PhysicsMaterial material;

    physics::World world;
    world.BeginSceneSync();
    world.SyncBody({}, &debris);
    world.SyncCollider({}, physics::ColliderInstance{
        &box, &debris, &material, math::Vector3::ZERO, false, 0 });
    world.EndSceneSync();

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetGravity({ 0.0f, -9.81f, 0.0f });
    rig.DisableGround();

    for (int i = 0; i < 60; ++i) {
        rig.RefreshContacts(&world);
        rig.Step(1.0f / 60.0f);
        rig.ApplyContactReactions();
    }

    /// @note 上から乗られたので下向きに押されている。逆にラグドール側は乗り越えていない。
    EXPECT_LT(debris.GetVelocity().y, 0.0f) << "the dynamic body was never pushed";
}

/// @note 関節で隣接する骨は重なるため、接触の押し返しによる振動を避けて自己衝突から除外する。
TEST_F(RagdollRigTest, ConnectedBonesDoNotSelfCollide)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);

    scene::RagdollContactSettings settings;
    settings.world   = false;
    settings.dynamic = false;
    settings.self    = true;
    rig.SetContactSettings(settings);

    rig.RefreshContacts(nullptr);
    EXPECT_EQ(rig.GetContactCount(), 0);
}

/// @note トルクを自重支持量の倍率で持ち、骨格の大きさが変わってもプロファイルを共用できること。
TEST_F(RagdollRigTest, TorqueFollowsTheSkeletonScale)
{
    const auto relativeSag = [](float scale) {
        auto bones = HorizontalChain(4);
        for (scene::RagdollBonePose& bone : bones) bone.position = bone.position * scale;

        scene::RagdollProfile profile = SoftProfile();
        profile.fallback.servo.enabled     = true;
        profile.fallback.servo.torqueScale = 2.0f;
        profile.fallback.servo.holdSag     = 0.05f;
        profile.fallback.servo.damping     = 20.0f;

        scene::RagdollRig rig;
        rig.Build(bones, profile);
        rig.Capture(bones);
        /// @note 片持ち梁 (自由落下する鎖は垂れない)
        rig.GetBody(0)->m_isStatic = true;
        rig.SetGravity({ 0.0f, -9.81f, 0.0f });

        for (int i = 0; i < 120; ++i) {
            rig.UpdateDriveTargets(bones);
            rig.Step(1.0f / 60.0f);
        }

        std::vector<math::Vector3>    positions;
        std::vector<math::Quaternion> rotations;
        rig.WritePose(bones, positions, rotations);

        float worst = 0.0f;
        for (std::size_t i = 1; i < bones.size(); ++i)
            worst = std::max(worst, (positions[i] - bones[i].position).Length());
        /// @note 大きさで割れば «形の崩れ» そのものになる
        return worst / scale;
    };

    const float small = relativeSag(1.0f);
    const float large = relativeSag(4.0f);
    EXPECT_NEAR(large, small, 0.05f) << "small=" << small << " large=" << large;
}

/// @note 関節は相対姿勢だけを拘束するため、常時アクティブでは根の固定が全身の落下を防ぐ。
TEST_F(RagdollRigTest, RootAnchorHoldsTheWholeBodyUpUnderGravity)
{
    const auto bones = StraightChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.servo.enabled     = true;
    profile.fallback.servo.torqueScale = 5.0f;
    profile.fallback.servo.holdSag     = 0.03f;
    profile.fallback.servo.damping     = 20.0f;

    scene::RagdollRig rig;
    rig.Build(bones, profile);
    rig.Capture(bones);
    rig.SetGravity({ 0.0f, -9.81f, 0.0f });
    /// @note Active。繋ぎ止めもここで入る
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);
    rig.SetRootAnchor(5.0f, 0.02f, 0.02f);

    for (int i = 0; i < 180; ++i) {
        rig.UpdateDriveTargets(bones);
        rig.Step(1.0f / 60.0f);
    }

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    /// @note 3 秒経っても «クリップそのもの» から離れないこと。
    for (std::size_t i = 0; i < bones.size(); ++i)
        EXPECT_LT((positions[i] - bones[i].position).Length(), 0.10f)
            << "bone " << i << " drifted from the animation pose";
}

/// @note 脱力時に根の固定も外れ、胴だけが宙に留まらないこと。
TEST_F(RagdollRigTest, PassiveReleasesTheRootAnchor)
{
    const auto bones = StraightChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.servo.enabled = true;

    scene::RagdollRig rig;
    rig.Build(bones, profile);
    rig.Capture(bones);
    rig.SetGravity({ 0.0f, -9.81f, 0.0f });
    rig.SetRootAnchor(5.0f, 0.02f, 0.02f);
    /// @note Passive
    rig.SetDrive(false, 1.0f, 1.0f, 1.0f);

    for (int i = 0; i < 60; ++i) rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    /// @note 1 秒の自由落下は約 4.9m。繋ぎ止めが残っていればここまで落ちない。
    EXPECT_LT(positions[0].y, bones[0].position.y - 1.0f);
}

/// @note クリップの要求角を学習して可動域を広げると、サーボが目標へ到達できること。
TEST_F(RagdollRigTest, LearningWidensLimitsToFitTheClip)
{
    const auto bones = StraightChain(4);

    /// @note わざと «ほとんど曲がらない» 可動域にしておく。
    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.limits.enabled   = true;
    profile.fallback.limits.twistMin  = -0.01f;
    profile.fallback.limits.twistMax  = 0.01f;
    profile.fallback.limits.swingMinY = -0.01f;
    profile.fallback.limits.swingMaxY = 0.01f;
    profile.fallback.limits.swingMinZ = -0.01f;
    profile.fallback.limits.swingMaxZ = 0.01f;
    profile.fallback.servo.enabled    = true;

    /// @note 子の位置と骨の回転を共に曲げ、捕獲時にソケット拘束が戻す不正な姿勢を作らない。
    auto bent = bones;
    const math::Quaternion bend = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 0.6f);
    for (std::size_t i = 1; i < bent.size(); ++i) {
        bent[i].rotation = (bend * bones[i].rotation).Normalized();
        bent[i].position = bones[1].position + bend * (bones[i].position - bones[1].position);
    }

    scene::RagdollRig rig;
    /// @note 可動域の基準はバインドポーズ (まっすぐ)
    rig.Build(bones, profile);
    rig.Capture(bent);
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);
    rig.SetLimitLearning(true, 0.09f);
    /// @note ここで «クリップが要求する角» を測る
    rig.UpdateDriveTargets(bent);

    rig.SetGravity(math::Vector3::ZERO);
    rig.Step(1.0f / 60.0f);

    /// @note 0.6 rad 曲げた関節が、0.01 rad の可動域に閉じ込められていないこと。
    EXPECT_EQ(rig.CountLimitedJoints(), 0);
}

TEST_F(RagdollRigTest, ProfileResolvesByBoneNameSubstring)
{
    scene::RagdollProfile profile;
    profile.fallback.radiusRatio = 0.5f;

    scene::RagdollProfile::Rule knee;
    knee.pattern              = "knee";
    knee.settings.radiusRatio = 0.1f;
    profile.rules.push_back(knee);

    EXPECT_FLOAT_EQ(profile.Resolve("Leg_Knee_L").radiusRatio, 0.1f);
    /// @note 大文字小文字を無視
    EXPECT_FLOAT_EQ(profile.Resolve("LEG_KNEE_R").radiusRatio, 0.1f);
    /// @note 当たらなければ fallback
    EXPECT_FLOAT_EQ(profile.Resolve("Spine_01").radiusRatio, 0.5f);
}

/// @note Resolve は未一致でも fallback を返すため、Mech が Boss_01 の胴と全脚の各部位に一致することを検証する。
TEST_F(RagdollRigTest, MechProfileCoversTheBossRig)
{
    const scene::RagdollProfile profile = scene::RagdollProfile::Mech();

    for (const char* bone : { "Body", "Core", "Muzzle",
                              "Yaw_FR", "Thigh_FR", "Shin_FR", "Hock_FR", "Foot_FR",
                              "Yaw_BL", "Thigh_BL", "Shin_BL", "Hock_BL", "Foot_BL" }) {
        bool matched = false;
        EXPECT_NE(&profile.Resolve(bone, &matched), &profile.fallback);
        EXPECT_TRUE(matched) << bone << " falls through to the profile fallback";
    }

    /// @note 原点の «入れ物» は枝だけ辿る。剛体を作ると胴から原点へ 4.5m の棒ができる。
    EXPECT_TRUE(profile.IsBodyless("Root"));
    EXPECT_TRUE(profile.IsBodyless("Boss_Armature"));
    EXPECT_FALSE(profile.IsBodyless("Body"));

    /// @note 足指と踵は枝ごと落とす。拾うと脚 1 本あたり剛体が 8 個増える。
    EXPECT_TRUE(profile.IsExcluded("Toe1A_FR"));
    EXPECT_TRUE(profile.IsExcluded("HeelB_BL"));
    EXPECT_FALSE(profile.IsExcluded("Foot_FR"));

    /// @note 胴は «塊»。脚より太く、桁違いに粘る。
    const scene::RagdollBoneSettings& torso = profile.Resolve("Body");
    const scene::RagdollBoneSettings& thigh = profile.Resolve("Thigh_FR");
    EXPECT_GT(torso.radiusRatio, thigh.radiusRatio);
    EXPECT_GT(torso.servo.torqueScale, thigh.servo.torqueScale);

    /// @note 飛節は膝と同じ蝶番で、**逆向き**に曲がる。
    const scene::RagdollBoneSettings& knee = profile.Resolve("Shin_FR");
    const scene::RagdollBoneSettings& hock = profile.Resolve("Hock_FR");
    EXPECT_TRUE(hock.limits.enabled);
    EXPECT_LT(hock.limits.swingMinZ, -1.0f);
    EXPECT_FLOAT_EQ(hock.limits.swingMaxZ, 0.0f);
    EXPECT_GT(knee.limits.swingMaxZ, 1.0f);
    EXPECT_FLOAT_EQ(knee.limits.swingMinZ, 0.0f);
}

/// @note 一致しない骨は黙って fallback にせず、名前を記録すること。
TEST_F(RagdollRigTest, UnmatchedBonesAreNamed)
{
    auto bones = StraightChain(4);
    /// @note 当たる
    bones[0].name = "Thigh_FR";
    /// @note 当たらない
    bones[1].name = "Tentacle";
    /// @note 当たる
    bones[2].name = "Shin_FR";

    scene::RagdollRig rig;
    rig.Build(bones, scene::RagdollProfile::Mech());

    /// @note 葉 (bones[3]) は剛体を持たないので設定を引かれず、数にも入らない。
    ASSERT_EQ(rig.UnmatchedBones().size(), 1u);
    EXPECT_EQ(rig.UnmatchedBones()[0], "Tentacle");
}

TEST_F(RagdollRigTest, MechProfileGivesTheKneeAOneWayHinge)
{
    const scene::RagdollProfile profile = scene::RagdollProfile::Mech();
    const scene::RagdollBoneSettings& knee = profile.Resolve("Leg_Knee_FR");

    EXPECT_TRUE(knee.limits.enabled);
    /// @note 片方向にしか曲がらない ＝ 下限が 0 で上限が正。
    EXPECT_FLOAT_EQ(knee.limits.swingMinZ, 0.0f);
    EXPECT_GT(knee.limits.swingMaxZ, 1.0f);
    /// @note サーボなのでトルクに上限がある (実値は骨格の質量から Build が埋める)。
    EXPECT_TRUE(knee.servo.enabled);
    EXPECT_GT(knee.servo.torqueScale, 0.0f);
}

TEST_F(RagdollRigTest, CenterOfMassSitsInsideTheChain)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);

    const math::Vector3 com = rig.CenterOfMass();
    EXPECT_GT(com.y, 0.0f);
    EXPECT_LT(com.y, 3.0f);
    EXPECT_NEAR(com.x, 0.0f, 1.0e-3f);
}

TEST_F(RagdollRigTest, ZeroRootStrengthDoesNotBecomeAnUnlimitedAnchor)
{
    const auto bones = StraightChain(2);
    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.UpdateDriveTargets(bones);
    rig.DisableGround();
    rig.SetRootAnchor(0.0f, 0.04f, 0.05f);
    rig.SetDrive(true, 0.0f, 1.0f, 1.0f);
    for (int i = 0; i < 60; ++i) rig.Step(1.0f / 60.0f);
    ASSERT_NE(rig.BodyOfBone(0), nullptr);
    EXPECT_LT(rig.BodyOfBone(0)->GetPosition().y, -2.0f);
}

TEST_F(RagdollRigTest, ZeroGravityDoesNotTurnComputedZeroForceIntoAnUnlimitedAnchor)
{
    const auto bones = StraightChain(2);
    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.UpdateDriveTargets(bones);
    rig.DisableGround();
    rig.SetGravity(math::Vector3::ZERO);
    rig.SetDrag(0.0f, 0.0f);
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);
    rig.ApplyImpulse(math::Vector3::ZERO, {1.0f, 0.0f, 0.0f}, 0.0f);
    for (int i = 0; i < 60; ++i) rig.Step(1.0f / 60.0f);
    ASSERT_NE(rig.BodyOfBone(0), nullptr);
    EXPECT_NEAR(rig.BodyOfBone(0)->GetPosition().x, 1.0f, 0.001f);
}

TEST_F(RagdollRigTest, LinearAndAngularInputsHaveTheSameMeaningInEitherGuardMode)
{
    for (const bool guarded : {false, true}) {
        const auto bones = StraightChain(2);
        scene::RagdollRig rig;
        rig.Build(bones, SoftProfile());
        rig.Capture(bones);
        rig.SetStandingGuard(guarded, 0.12f, 0.25f);
        rig.ApplyImpulse(math::Vector3::ZERO, math::Vector3::RIGHT, 0.0f);
        EXPECT_VEC3_NEAR(rig.BodyOfBone(0)->GetVelocity(), math::Vector3::RIGHT, 1.0e-6f);
        EXPECT_VEC3_NEAR(rig.BodyOfBone(0)->GetAngularVelocity(), math::Vector3::ZERO, 1.0e-6f);
        const math::Vector3 angular{0.0f, 0.0f, 12.0f};
        rig.ApplyAngularVelocity(math::Vector3::ZERO, angular, 0.0f);
        EXPECT_VEC3_NEAR(rig.BodyOfBone(0)->GetAngularVelocity(), angular, 1.0e-6f);
        EXPECT_VEC3_NEAR(rig.BodyOfBone(0)->GetVelocity(), math::Vector3::RIGHT, 1.0e-6f);
    }
}

TEST_F(RagdollRigTest, ExplicitAngularInputRotatesTheTorsoWithoutMovingItsRoot)
{
    const auto bones = StraightChain(2);
    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetGravity(math::Vector3::ZERO);
    rig.DisableGround();
    rig.SetDrive(false, 0.0f, 1.0f, 1.0f);
    rig.SetStandingGuard(true, 0.12f, 0.25f);
    rig.ApplyImpulse(math::Vector3::ZERO, math::Vector3::RIGHT, 0.0f);
    rig.ApplyAngularVelocity(math::Vector3::ZERO, {0.0f, 0.0f, -2.0f}, 0.0f);
    rig.Step(1.0f / 60.0f);
    std::vector<math::Vector3> positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);
    EXPECT_VEC3_NEAR(positions[0], bones[0].position, 0.0001f);
    EXPECT_GT((positions[1] - bones[1].position).Length(), 0.005f);
    EXPECT_LE((positions[1] - bones[1].position).Length(), 0.1201f);
}

TEST_F(RagdollRigTest, StandingRootMotionDoesNotCreateASpuriousImpulse)
{
    auto bones = StraightChain(4);
    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.SetStandingGuard(true, 0.12f, 0.25f);
    rig.UpdateDriveTargets(bones);
    for (auto& bone : bones) bone.position.x += 10.0f;
    rig.UpdateDriveTargets(bones);
    std::vector<math::Vector3> positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);
    EXPECT_VEC3_NEAR(positions[0], bones[0].position, 0.0001f);
    EXPECT_VEC3_NEAR(rig.BodyOfBone(0)->GetVelocity(), math::Vector3::ZERO, 0.0001f);
}

TEST_F(RagdollRigTest, StandingProjectionPreservesLengthsAndPhysicalBodyCenters)
{
    const auto bones = StraightChain(4);
    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());
    rig.Capture(bones);
    rig.UpdateDriveTargets(bones);
    rig.DisableGround();
    rig.SetDrive(false, 0.0f, 1.0f, 1.0f);
    rig.SetStandingGuard(true, 0.12f, 0.25f);
    std::vector<math::Vector3> positions;
    std::vector<math::Quaternion> rotations;
    float largestResponse = 0.0f;
    for (int frame = 0; frame < 240; ++frame) {
        if (frame % 12 == 0) {
            rig.GetBody(1)->SetAngularVelocity({4.0f, 0.0f, 0.0f});
            rig.ApplyImpulse({0.0f, 2.0f, 0.0f}, {3.0f, -5.0f, 2.0f}, 2.0f);
        }
        rig.Step(1.0f / 120.0f);
        rig.WritePose(bones, positions, rotations);
        EXPECT_VEC3_NEAR(positions[0], bones[0].position, 0.0001f);
        for (std::size_t i = 1; i < positions.size(); ++i) {
            const float response = (positions[i] - bones[i].position).Length();
            largestResponse = std::max(largestResponse, response);
            EXPECT_LE(response, 0.1201f);
            EXPECT_NEAR((positions[i] - positions[i - 1]).Length(), 1.0f, 0.0001f);
            EXPECT_VEC3_NEAR(rig.BodyOfBone(static_cast<int>(i - 1))->GetPosition(),
                             (positions[i - 1] + positions[i]) * 0.5f, 0.0001f);
        }
    }
    EXPECT_GT(largestResponse, 0.0001f);
}

}
