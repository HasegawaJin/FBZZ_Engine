/// @file    RagdollRig.hpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY Skeleton / GameObject を知らない形にするか:
///   ここがやるのは «骨の姿勢の配列» と «剛体の姿勢» の変換だけで、アセットの形式にも
///   シーングラフにも依存しない。切り離しておけば、スケルトンを用意せずに単体テストで
///   «捕獲して書き戻したら元の姿勢に戻るか» を確かめられる。Skeleton から配列を作るのは
///   RagdollSystem の仕事。
///
/// WHY 剛体を «骨と骨の間» に置くか:
///   骨の位置に置くと、剛体の中心が関節と重なって慣性が実際と食い違う (腕を振っても
///   反動が出ない)。骨 i から最初の子までを 1 本のカプセルにし、その中点を重心にすると、
///   長い骨ほど回りにくい ─ ロボットの «重い» はここから出る。
#pragma once

#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/XPBDJoint.hpp>
#include <Physics/XPBDSolver.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

/// 骨 1 本のワールド姿勢。**親は必ず子より前に並んでいること。**
struct RagdollBonePose {
    /// 同じ配列内の親の添字。-1 なら根。
    int              parent = -1;
    std::string      name;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

/// 剛体・コライダー・関節を所有し、ソルバへ載せる。
///
/// 内部で非所有ポインタ (ソルバ → 剛体、関節 → 剛体) が絡むので、コピーもムーブも禁止する。
/// **コンポーネントから持つときは `std::unique_ptr<RagdollRig>` で保持すること** ─
/// コンポーネントは配列の再確保で動くことがある。
class RagdollRig {
public:
    RagdollRig() = default;
    ~RagdollRig();

    RagdollRig(const RagdollRig&)            = delete;
    RagdollRig& operator=(const RagdollRig&) = delete;
    RagdollRig(RagdollRig&&)                 = delete;
    RagdollRig& operator=(RagdollRig&&)      = delete;

    /// 骨の並びから剛体と関節を組む。**この姿勢が «たわみ 0» の基準になる**ので、
    /// 可動域はここからの角度で測られる。バインドポーズ相当で呼ぶのが望ましい。
    void Build(const std::vector<RagdollBonePose>& bones, const RagdollProfile& profile);
    void Clear();

    /// 剛体を今の骨の姿勢へ置き直し、速度を 0 に戻す。物理へ渡す境目。
    void Capture(const std::vector<RagdollBonePose>& bones);

    /// Active: 各関節のドライブ目標を今の骨の姿勢から取り直す。
    /// 無負荷ならこの姿勢が釣り合い点になるので、絵はクリップそのままになる。
    void UpdateDriveTargets(const std::vector<RagdollBonePose>& bones);

    /// ドライブを一括で入切する。false で Passive (脱力)。
    void SetDriveEnabled(bool enabled);

    void Step(float dt) { m_solver.Step(dt); }

    /// 剛体の姿勢を骨のワールド姿勢へ戻す。
    /// 剛体を持たない骨 (葉など) は、fallback の «親からの相対» を保って埋める。
    void WritePose(const std::vector<RagdollBonePose>& fallback,
                   std::vector<math::Vector3>&         outPositions,
                   std::vector<math::Quaternion>&      outRotations) const;

    /// 与えられた姿勢から一番離れた剛体の距離 [m]。«どれだけ効いているか» の物差し。
    [[nodiscard]] float MeasureDeviation(const std::vector<RagdollBonePose>& bones) const;
    /// 全剛体の重心 (質量加重)。バランス判定と «どちらへ倒れるか» に使う。
    [[nodiscard]] math::Vector3 CenterOfMass() const;
    /// トルク上限に張り付いている関節の数。0 でない ＝ どこかが力負けしている。
    [[nodiscard]] int CountSaturatedJoints() const;

    [[nodiscard]] bool IsBuilt() const { return !m_bodies.empty(); }
    [[nodiscard]] int  GetBodyCount()  const { return static_cast<int>(m_bodies.size()); }
    [[nodiscard]] int  GetJointCount() const { return static_cast<int>(m_joints.size()); }
    /// 骨の添字 → 剛体の添字。-1 なら剛体を持たない骨。
    [[nodiscard]] int  BodyIndexOfBone(int boneIndex) const;

    [[nodiscard]] physics::XPBDSolver& Solver() { return m_solver; }

private:
    /// 剛体 1 個と、それが代表する骨との対応。
    struct BodyLink {
        int                                      boneIndex = -1;
        std::unique_ptr<physics::RigidBody>      body;
        std::unique_ptr<physics::CapsuleCollider> collider;
        /// 骨 = 剛体 × これ。捕獲と書き戻しはこの 2 つで往復する。
        math::Vector3    boneOffset = math::Vector3::ZERO;
        math::Quaternion boneRotation = math::Quaternion::Identity();
        float            mass = 1.0f;
    };

    /// 関節と、それが繋いでいる剛体の添字。所有は m_solver 側。
    struct JointLink {
        physics::XPBDJoint* joint      = nullptr;
        int                 parentBody = -1;
        int                 childBody  = -1;
    };

    [[nodiscard]] physics::RigidBody* BodyOfBone(int boneIndex) const;
    /// 骨 i の «剛体としての» ワールド姿勢を、与えられた骨の姿勢から作る。
    void BodyPoseFromBone(int bodyIndex, const RagdollBonePose& bone,
                          math::Vector3& outPosition, math::Quaternion& outRotation) const;

    std::vector<BodyLink>  m_bodies;
    std::vector<JointLink> m_joints;
    /// 骨の添字 → m_bodies の添字。-1 で剛体なし。
    std::vector<int>                 m_boneToBody;
    physics::XPBDSolver              m_solver;
};

} // namespace fbzz::scene
