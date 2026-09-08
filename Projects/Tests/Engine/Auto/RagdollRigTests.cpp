/// @file    RagdollRigTests.cpp
/// @brief   骨の並び ↔ 剛体の往復と、プロファイルの割り当てを自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// スケルトンもシーンも用意せず、骨の «ワールド姿勢の配列» だけで確かめる。
/// 一番大事なのは «捕獲して書き戻したら元の姿勢に戻る» こと ─ ここが崩れていると、
/// ラグドールを起動した瞬間にキャラクターの形が変わる。
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

/// 原点から +X へ 1m 刻みで伸びる骨。**重力が関節を曲げる向きに掛かる**ので、
/// «サーボが荷重を支えられるか» を見るにはこちらを使う (縦の鎖では軸方向にしか
/// 引かれず、力負けが起きない)。
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
    // 節の長さ 1m の鎖に対して半径 0.1m になる比。
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

} // namespace

TEST_F(RagdollRigTest, BuildsOneBodyPerBoneThatHasAChild)
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

// 組んだ時と違う姿勢を捕獲しても往復すること。ラグドールは «歩いている途中» で起動する。
TEST_F(RagdollRigTest, CaptureReproducesAPoseDifferentFromTheBuildPose)
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

    // 落ちたので葉も元の位置には居ない。
    EXPECT_GT((positions[3] - bones[3].position).Length(), 0.05f);
    // それでも親との距離は保たれている (骨は伸びない)。
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

// Active の要。ドライブの目標を今の骨から取り直すと、無負荷の釣り合い点がその姿勢になる。
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

// 接地。M3 で本物の接触へ差し替わるまでの足場だが、«床を抜けない» は今から要る。
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

    // カプセルの端 ＋ 半径がちょうど骨の位置なので、骨が床より下へ出たら抜けている。
    for (std::size_t i = 0; i < positions.size(); ++i)
        EXPECT_GT(positions[i].y, -2.15f) << "bone " << i << " fell through the ground";
}

// 力負け。同じ荷重でもトルク上限が低い関節は目標を保てず、そのぶん垂れる。
// «サーボが力負けする» はロボット感の中心なので、数値として押さえておく。
TEST_F(RagdollRigTest, LowerTorqueLimitSagsMoreAndSaturates)
{
    const auto bones = HorizontalChain(4);

    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.servo.enabled     = true;
    // 自重の 3 倍まで支えられるサーボ。倍率なので、鎖の重さが変わっても意味が変わらない。
    profile.fallback.servo.torqueScale = 3.0f;
    profile.fallback.servo.holdSag     = 0.02f;
    profile.fallback.servo.damping     = 20.0f;

    // WHY 根を固定するか: 自由落下する鎖は «垂れない»。重力が一様なので全剛体が同じ
    //     加速度で落ち、関節に相対的なたわみが生まれず、サーボが 1 N·m も出さずに済む。
    //     トルクの上限を見たいなら、反力を受け取る固定点が要る ─ 片持ち梁にする。
    const auto sagWith = [&](float driveScale, int& outSaturated) {
        scene::RagdollRig rig;
        rig.Build(bones, profile);
        rig.Capture(bones);
        rig.GetBody(0)->m_isStatic = true;
        rig.SetDrive(true, driveScale, 1.0f, 1.0f);
        rig.SetGravity({ 0.0f, -9.81f, 0.0f });

        // 垂れ切って «軸方向にぶら下がる» 形に落ち着くとトルクが要らなくなるので、
        // 最後の 1 フレームではなく «一度でも張り付いたか» を見る。
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

    EXPECT_LT(strongSag, 0.10f);            // 出力が足りていれば形は保たれる
    EXPECT_GT(weakSag, strongSag * 3.0f);   // 足りなければ垂れる
    EXPECT_EQ(strongSaturated, 0);
    EXPECT_GT(weakSaturated, 0);            // 上限に張り付いた関節が居る ＝ 力負け
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

    // 5 m/s で 1/60 秒 ＝ 8cm 強。全身が一律に動くので鎖は伸びない。
    for (std::size_t i = 0; i < positions.size(); ++i)
        EXPECT_NEAR(positions[i].x - bones[i].position.x, 5.0f / 60.0f, 0.02f)
            << "bone " << i;
}

// M3。世界に置いた «床» で止まること。自前の平面ではなく、World のコライダーを
// 既存の NarrowPhase 越しに拾って解いている経路を通す。
TEST_F(RagdollRigTest, WorldColliderStopsTheFall)
{
    const auto bones = StraightChain(4);

    // 天板が y = 0 に来る箱を静的コライダーとして置く。静的コライダーは World が
    // Update しない (剛体を持たないため) ので、ここで一度だけワールドへ置く。
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
    rig.DisableGround();   // 抜け止めの平面を無効化し、世界の接触だけで受け止める

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

// M4。World が積分している剛体は substep の中では動かさず、受けた反作用を
// フレーム末に力積として返す。«瓦礫を蹴る» の実体。
TEST_F(RagdollRigTest, FallingRagdollKicksADynamicBody)
{
    const auto bones = StraightChain(4);

    physics::RigidBody debris;
    debris.SetPosition({ 0.0f, -0.6f, 0.0f });
    debris.SetMass(5.0f);
    debris.m_useGravity = false;   // World::Step を回さないので自分では落ちない

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

    // 上から乗られたので下向きに押されている。逆にラグドール側は乗り越えていない。
    EXPECT_LT(debris.GetVelocity().y, 0.0f) << "the dynamic body was never pushed";
}

// 関節で繋がった骨どうしは必ず重なる。当ててしまうと、関節が寄せた端から接触が
// 押し返して震え続ける ─ 真っ直ぐな鎖では自己衝突が 1 つも出ないのが正しい。
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

// この変更の芯。トルクを «自重を支えるのに要る量» への倍率で持つので、骨格の
// 大きさが変わっても «どれだけ垂れるか» が変わらないこと。
//
// WHY 要るか: 絶対値 [N·m] で持っていた頃、人型の想定で書いた 14,000 N·m を
//     脚だけで 7m ある Boss_01 に当てたら必要量の 1/3 しか出せず、脚が畳まれた。
//     骨格を差し替えるたびに数字を置き直すのでは «汎用のプロファイル» にならない。
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
        rig.GetBody(0)->m_isStatic = true;   // 片持ち梁 (自由落下する鎖は垂れない)
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
        return worst / scale;   // 大きさで割れば «形の崩れ» そのものになる
    };

    const float small = relativeSag(1.0f);
    const float large = relativeSag(4.0f);
    EXPECT_NEAR(large, small, 0.05f) << "small=" << small << " large=" << large;
}

// 常時アクティブの前提。関節は «隣の骨との相対» しか拘束しないので、根を繋ぎ止めないと
// サーボが形を保ったまま全体が重力で落ちていく。倒れる数秒だけなら見えないが、
// 立っている間ずっと走らせると胴が床下へ沈む。
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
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);      // Active。繋ぎ止めもここで入る
    rig.SetRootAnchor(5.0f, 0.02f, 0.02f);

    for (int i = 0; i < 180; ++i) {
        rig.UpdateDriveTargets(bones);
        rig.Step(1.0f / 60.0f);
    }

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    // 3 秒経っても «クリップそのもの» から離れないこと。
    for (std::size_t i = 0; i < bones.size(); ++i)
        EXPECT_LT((positions[i] - bones[i].position).Length(), 0.10f)
            << "bone " << i << " drifted from the animation pose";
}

// 脱力したら繋ぎ止めも外れること。残ると «力が抜けたのに胴だけ宙に留まる» になる。
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
    rig.SetDrive(false, 1.0f, 1.0f, 1.0f);     // Passive

    for (int i = 0; i < 60; ++i) rig.Step(1.0f / 60.0f);

    std::vector<math::Vector3>    positions;
    std::vector<math::Quaternion> rotations;
    rig.WritePose(bones, positions, rotations);

    // 1 秒の自由落下は約 4.9m。繋ぎ止めが残っていればここまで落ちない。
    EXPECT_LT(positions[0].y, bones[0].position.y - 1.0f);
}

// 可動域の自動学習。クリップが «曲げてよいことになっていない» 所まで曲げていると
// サーボが目標へ行けず絵が崩れる。要求された角を測って広げれば、崩れなくなる。
TEST_F(RagdollRigTest, LearningWidensLimitsToFitTheClip)
{
    const auto bones = StraightChain(4);

    // わざと «ほとんど曲がらない» 可動域にしておく。
    scene::RagdollProfile profile = SoftProfile();
    profile.fallback.limits.enabled   = true;
    profile.fallback.limits.twistMin  = -0.01f;
    profile.fallback.limits.twistMax  = 0.01f;
    profile.fallback.limits.swingMinY = -0.01f;
    profile.fallback.limits.swingMaxY = 0.01f;
    profile.fallback.limits.swingMinZ = -0.01f;
    profile.fallback.limits.swingMaxZ = 0.01f;
    profile.fallback.servo.enabled    = true;

    // 骨 1 で折れ曲がった «クリップ» を作る。
    //
    // 曲げる骨の回転も一緒に回すこと。剛体は «その骨から次の骨へ» の区間を代表し、
    // 向きは骨の rotation から作られる。回転を据え置いたまま子の位置だけ動かすと、
    // 剛体の向きと関節の位置が食い違った «骨格として有り得ない姿勢» になり、
    // 捕獲した瞬間にソケット拘束が 0.6m ぶん引き戻しにかかる。
    // それは «可動域が狭い» のではなく «姿勢が壊れている» ので、この試験の対象ではない。
    auto bent = bones;
    const math::Quaternion bend = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 0.6f);
    for (std::size_t i = 1; i < bent.size(); ++i) {
        bent[i].rotation = (bend * bones[i].rotation).Normalized();
        bent[i].position = bones[1].position + bend * (bones[i].position - bones[1].position);
    }

    scene::RagdollRig rig;
    rig.Build(bones, profile);          // 可動域の基準はバインドポーズ (まっすぐ)
    rig.Capture(bent);
    rig.SetDrive(true, 1.0f, 1.0f, 1.0f);
    rig.SetLimitLearning(true, 0.09f);
    rig.UpdateDriveTargets(bent);       // ここで «クリップが要求する角» を測る

    rig.SetGravity(math::Vector3::ZERO);
    rig.Step(1.0f / 60.0f);

    // 0.6 rad 曲げた関節が、0.01 rad の可動域に閉じ込められていないこと。
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
    EXPECT_FLOAT_EQ(profile.Resolve("LEG_KNEE_R").radiusRatio, 0.1f);  // 大文字小文字を無視
    EXPECT_FLOAT_EQ(profile.Resolve("Spine_01").radiusRatio, 0.5f);    // 当たらなければ fallback
}

// 回帰。Mech プロファイルは Boss_01 のリグ (README: Body + 脚 4 本 ×
// Thigh → Shin → Hock → Foot) の**全部位**に当たること。
//
// WHY 要るか: 当たらなくても settings は返るので、外していても «なんとなく柔らかい
//     ラグドール» にしかならない。実際に Body と Hock を取りこぼしていて、胴が
//     «塊» ではなく細い棒に、飛節が蝶番ではなく球関節になっていた。
TEST_F(RagdollRigTest, MechProfileCoversTheBossRig)
{
    const scene::RagdollProfile profile = scene::RagdollProfile::Mech();

    for (const char* bone : { "Body", "Core", "Muzzle",
                              "Yaw_FR", "Thigh_FR", "Shin_FR", "Hock_FR", "Foot_FR",
                              "Yaw_BL", "Thigh_BL", "Shin_BL", "Hock_BL", "Foot_BL" }) {
        bool matched = false;
        profile.Resolve(bone, &matched);
        EXPECT_TRUE(matched) << bone << " falls through to the profile fallback";
    }

    // 原点の «入れ物» は枝だけ辿る。剛体を作ると胴から原点へ 4.5m の棒ができる。
    EXPECT_TRUE(profile.IsBodyless("Root"));
    EXPECT_TRUE(profile.IsBodyless("Boss_Armature"));
    EXPECT_FALSE(profile.IsBodyless("Body"));

    // 足指と踵は枝ごと落とす。拾うと脚 1 本あたり剛体が 8 個増える。
    EXPECT_TRUE(profile.IsExcluded("Toe1A_FR"));
    EXPECT_TRUE(profile.IsExcluded("HeelB_BL"));
    EXPECT_FALSE(profile.IsExcluded("Foot_FR"));

    // 胴は «塊»。脚より太く、桁違いに粘る。
    const scene::RagdollBoneSettings& torso = profile.Resolve("Body");
    const scene::RagdollBoneSettings& thigh = profile.Resolve("Thigh_FR");
    EXPECT_GT(torso.radiusRatio, thigh.radiusRatio);
    EXPECT_GT(torso.servo.torqueScale, thigh.servo.torqueScale);

    // 飛節は膝と同じ蝶番で、**逆向き**に曲がる。
    const scene::RagdollBoneSettings& knee = profile.Resolve("Shin_FR");
    const scene::RagdollBoneSettings& hock = profile.Resolve("Hock_FR");
    EXPECT_TRUE(hock.limits.enabled);
    EXPECT_LT(hock.limits.swingMinZ, -1.0f);
    EXPECT_FLOAT_EQ(hock.limits.swingMaxZ, 0.0f);
    EXPECT_GT(knee.limits.swingMaxZ, 1.0f);
    EXPECT_FLOAT_EQ(knee.limits.swingMinZ, 0.0f);
}

// 当たらない骨は «黙って fallback» にせず、名前で残すこと。
TEST_F(RagdollRigTest, UnmatchedBonesAreNamed)
{
    auto bones = StraightChain(4);
    bones[0].name = "Thigh_FR";   // 当たる
    bones[1].name = "Tentacle";   // 当たらない
    bones[2].name = "Shin_FR";    // 当たる

    scene::RagdollRig rig;
    rig.Build(bones, scene::RagdollProfile::Mech());

    // 葉 (bones[3]) は剛体を持たないので設定を引かれず、数にも入らない。
    ASSERT_EQ(rig.UnmatchedBones().size(), 1u);
    EXPECT_EQ(rig.UnmatchedBones()[0], "Tentacle");
}

TEST_F(RagdollRigTest, MechProfileGivesTheKneeAOneWayHinge)
{
    const scene::RagdollProfile profile = scene::RagdollProfile::Mech();
    const scene::RagdollBoneSettings& knee = profile.Resolve("Leg_Knee_FR");

    EXPECT_TRUE(knee.limits.enabled);
    // 片方向にしか曲がらない ＝ 下限が 0 で上限が正。
    EXPECT_FLOAT_EQ(knee.limits.swingMinZ, 0.0f);
    EXPECT_GT(knee.limits.swingMaxZ, 1.0f);
    // サーボなのでトルクに上限がある (実値は骨格の質量から Build が埋める)。
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

} // namespace fbzz::tests
