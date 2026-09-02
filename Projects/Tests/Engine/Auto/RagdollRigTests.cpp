/// @file    RagdollRigTests.cpp
/// @brief   骨の並び ↔ 剛体の往復と、プロファイルの割り当てを自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// スケルトンもシーンも用意せず、骨の «ワールド姿勢の配列» だけで確かめる。
/// 一番大事なのは «捕獲して書き戻したら元の姿勢に戻る» こと ─ ここが崩れていると、
/// ラグドールを起動した瞬間にキャラクターの形が変わる。
#include <gtest/gtest.h>

#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::tests {

namespace {

/// 原点から +Y へ 1m 刻みで伸びる、まっすぐな 4 本の骨。
/// 3 本が剛体を持ち (最後の骨は葉なので持たない)、関節は 2 本。
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

scene::RagdollProfile SoftProfile()
{
    scene::RagdollProfile profile;
    profile.fallback.radius  = 0.1f;
    profile.fallback.density = 1000.0f;
    profile.fallback.limits.enabled = false;
    profile.fallback.drive.enabled  = false;
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

} // namespace

TEST(RagdollRigTest, BuildsOneBodyPerBoneThatHasAChild)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());

    EXPECT_EQ(rig.GetBodyCount(), 3);    // 葉は剛体を持たない
    EXPECT_EQ(rig.GetJointCount(), 2);   // 根の剛体はワールドへ繋がない
    EXPECT_GE(rig.BodyIndexOfBone(0), 0);
    EXPECT_EQ(rig.BodyIndexOfBone(3), -1);
}

// 一番大事な契約。物理を 1 ステップも回さずに書き戻したら、元の姿勢と一致すること。
TEST(RagdollRigTest, CaptureThenWriteReproducesThePose)
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

// 組んだ時と違う姿勢を捕獲しても往復すること。ラグドールは «歩いている途中» で起動する。
TEST(RagdollRigTest, CaptureReproducesAPoseDifferentFromTheBuildPose)
{
    const auto bones = StraightChain(4);

    scene::RagdollRig rig;
    rig.Build(bones, SoftProfile());

    // 途中で折れ曲がった姿勢を作る。親の回転を子の位置へ積んで «関節が曲がった» 形にする。
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

// 剛体を持たない葉の骨も、親に付いて動くこと。
TEST(RagdollRigTest, LeafBoneFollowsItsParentBody)
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

    // 落ちたので葉も元の位置には居ない。
    EXPECT_GT((positions[3] - bones[3].position).Length(), 0.05f);
    // それでも親との距離は保たれている (骨は伸びない)。
    EXPECT_NEAR((positions[3] - positions[2]).Length(),
                (bones[3].position - bones[2].position).Length(), 0.05f);
}

TEST(RagdollRigTest, JointsKeepTheChainConnectedWhileFalling)
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

// Active の要。ドライブの目標を今の骨から取り直すと、無負荷の釣り合い点がその姿勢になる。
TEST(RagdollRigTest, DriveHoldsTheAnimationPoseUnderGravity)
{
    const auto bones = StraightChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.drive.enabled    = true;
    profile.fallback.drive.compliance = 1.0e-6f;
    profile.fallback.drive.damping    = 30.0f;

    scene::RagdollRig rig;
    rig.Build(bones, profile);
    rig.Capture(bones);
    rig.UpdateDriveTargets(bones);
    rig.Solver().SetGravity({ 0.0f, -9.81f, 0.0f });

    for (int i = 0; i < 120; ++i) {
        rig.UpdateDriveTargets(bones);
        rig.Step(1.0f / 60.0f);
    }

    // 根の剛体は何にも繋がっていないので落ちる。ここで見たいのは «形が保たれるか» で、
    // 関節から先が垂れていないこと ＝ 姿勢のずれが小さいこと。
    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    const math::Vector3 drift = positions[0] - bones[0].position;
    for (std::size_t i = 1; i < bones.size(); ++i) {
        const math::Vector3 expected = bones[i].position + drift;
        EXPECT_LT((positions[i] - expected).Length(), 0.05f) << "bone " << i << " sagged";
    }
}

TEST(RagdollRigTest, ProfileResolvesByBoneNameSubstring)
{
    scene::RagdollProfile profile;
    profile.fallback.radius = 0.5f;

    scene::RagdollProfile::Rule knee;
    knee.pattern          = "knee";
    knee.settings.radius  = 0.1f;
    profile.rules.push_back(knee);

    EXPECT_FLOAT_EQ(profile.Resolve("Leg_Knee_L").radius, 0.1f);
    EXPECT_FLOAT_EQ(profile.Resolve("LEG_KNEE_R").radius, 0.1f);   // 大文字小文字を無視
    EXPECT_FLOAT_EQ(profile.Resolve("Spine_01").radius, 0.5f);     // 当たらなければ fallback
}

TEST(RagdollRigTest, MechProfileGivesTheKneeAOneWayHinge)
{
    const scene::RagdollProfile profile = scene::RagdollProfile::Mech();
    const scene::RagdollBoneSettings& knee = profile.Resolve("Leg_Knee_FR");

    EXPECT_TRUE(knee.limits.enabled);
    // 片方向にしか曲がらない ＝ 下限が 0 で上限が正。
    EXPECT_FLOAT_EQ(knee.limits.swingMinZ, 0.0f);
    EXPECT_GT(knee.limits.swingMaxZ, 1.0f);
    // サーボなのでトルクに上限がある。
    EXPECT_GT(knee.drive.maxTorque, 0.0f);
}

TEST(RagdollRigTest, CenterOfMassSitsInsideTheChain)
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

} // namespace fbzz::tests
