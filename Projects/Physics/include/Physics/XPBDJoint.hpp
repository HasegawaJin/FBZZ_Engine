/// @file    XPBDJoint.hpp
/// @brief   ボールソケット + swing/twist 制限 + 角度ドライブを 1 本にまとめた関節
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 関節フレームは **X = ツイスト軸 (骨の向き)、Y/Z = スイング軸** で固定する。
/// 可動域も目標姿勢もこのフレームでの «たわみ» として測るので、Build() で
/// 今の姿勢をたわみ 0 に据えれば、以降は関節の値だけを見ればよい。
///
/// WHY 3 つを別クラスに割らないか:
///   同じ関節フレームとアンカーを共有し、解く順にも意味がある (ドライブ → 制限 →
///   ソケットの順で «硬いものを後に» 置く)。別クラスにすると順序の保証が
///   呼び出し側へ漏れ、«制限を追加したら効かない» が起きる。
///
/// WHY スイング制限を円錐ではなく軸ごとの上下限にするか:
///   円錐は中立軸まわりに対称なので «膝は前にだけ 140° 曲がる» が書けない。
///   軸ごとの非対称な上下限 (角錐) なら、関節フレームを動かさずにそのまま書ける。
///   代償として角錐の «角» が円錐より少し広いが、関節が取れる姿勢としては許容範囲。
#pragma once

#include <Physics/XPBDConstraint.hpp>

namespace fbzz::physics
{
    /// 関節の可動域 [rad]。すべて «たわみ 0» からの符号付きの量。
    struct XPBDJointLimits
    {
        bool  enabled = false;
        /// 関節フレーム X 軸まわり (ねじり)。
        float twistMin = 0.0f, twistMax = 0.0f;
        /// 関節フレーム Y 軸まわり。
        float swingMinY = 0.0f, swingMaxY = 0.0f;
        /// 関節フレーム Z 軸まわり。
        float swingMinZ = 0.0f, swingMaxZ = 0.0f;
        /// 0 で «そこで止まる»。大きくすると止まり際に食い込む (ゴムの止まり方)。
        float compliance = 0.0f;
    };

    /// 目標姿勢へ引き戻すサーボ。
    struct XPBDJointDrive
    {
        /// false で完全に脱力する (Passive ラグドール)。
        bool  enabled = false;
        /// 0 で剛。単位 [rad/(N·m)]。定常たわみは compliance × トルクで、刻みに依らない。
        float compliance = 1.0e-4f;
        /// 相対角速度を削る割合 [1/s]。ドライブの D 項。
        float damping = 0.0f;
        /// 出せるトルクの上限 [N·m]。0 以下で無制限。
        /// 超えた関節は目標へ追従できず back-drive する ─ «モーターが力負けする» 画。
        float maxTorque = 0.0f;
        /// 関節フレームでの目標相対姿勢。Identity で «たわみ 0 の姿勢»。
        math::Quaternion target = math::Quaternion::Identity();
    };

    class XPBDJoint final : public XPBDConstraint
    {
    public:
        /// @param parent null なら «ワールドへ固定» になる。ローカル量はワールド量として扱う。
        XPBDJoint(RigidBody* parent, RigidBody* child);

        /// 今の両者の姿勢を «たわみ 0» として、アンカーと関節フレームを組む。
        ///
        /// @param worldAnchor 関節の位置 (ワールド)。
        /// @param worldFrame  関節フレームの姿勢 (ワールド)。X が骨の向きになるように渡す。
        void Build(const math::Vector3& worldAnchor, const math::Quaternion& worldFrame);

        [[nodiscard]] XPBDJointLimits& Limits() { return m_limits; }
        [[nodiscard]] XPBDJointDrive&  Drive()  { return m_drive;  }
        [[nodiscard]] const XPBDJointLimits& Limits() const { return m_limits; }
        [[nodiscard]] const XPBDJointDrive&  Drive()  const { return m_drive;  }

        /// ソケットの柔らかさ [m/N]。0 で «伸びない»。
        void SetSocketCompliance(float compliance) { m_socketCompliance = compliance; }

        [[nodiscard]] RigidBody* GetParent() const { return m_parent; }
        [[nodiscard]] RigidBody* GetChild()  const { return m_child;  }

        /// Build() が組んだ関節フレーム (各剛体ローカル)。
        /// ドライブの目標を «別の姿勢» から計算する側と、可動域のデバッグ表示が使う。
        [[nodiscard]] math::Quaternion GetFrameParent() const { return m_frameParent; }
        [[nodiscard]] math::Quaternion GetFrameChild()  const { return m_frameChild;  }
        [[nodiscard]] math::Vector3    GetAnchorChild() const { return m_anchorChild;  }

        /// 可動域を «今どこを向いているか» ごと描くための、ワールドでの関節フレーム。
        [[nodiscard]] math::Quaternion GetParentFrameWorld() const { return ParentFrameWorld(); }
        [[nodiscard]] math::Quaternion GetChildFrameWorld()  const { return ChildFrameWorld();  }
        [[nodiscard]] math::Vector3    GetAnchorParentWorld() const { return ParentAnchorWorld(); }

        void ResetLambda() override;
        void SolvePosition(float h) override;
        void SolveVelocity(float h) override;

        /// 直前の substep で測ったたわみ [rad]。可動域の調整と、崩れ方の診断に使う。
        [[nodiscard]] float GetTwistAngle() const { return m_twistAngle; }
        [[nodiscard]] float GetSwingAngleY() const { return m_swingAngleY; }
        [[nodiscard]] float GetSwingAngleZ() const { return m_swingAngleZ; }
        /// ドライブがトルク上限に張り付いているか。«どの関節が力負けしたか» の表示に使う。
        [[nodiscard]] bool  IsDriveSaturated() const { return m_driveSaturated; }
        /// 可動域に食い込んで押し戻されているか。
        ///
        /// WHY 飽和と分けるか: «アニメーションどおりに動かない» の原因は «力が足りない»
        ///     (飽和) と «そこまで曲げてよいことになっていない» (可動域) の 2 つあり、
        ///     絵はどちらも同じ «崩れる» にしか見えない。切り分けが要る。
        [[nodiscard]] bool  IsLimited() const { return m_limited; }

    private:
        [[nodiscard]] math::Quaternion ParentFrameWorld() const;
        [[nodiscard]] math::Quaternion ChildFrameWorld()  const;
        [[nodiscard]] math::Vector3    ParentAnchorWorld() const;

        void SolveDrive(float h);
        void SolveLimits(float h);
        void SolveSocket(float h);

        RigidBody* m_parent = nullptr;
        RigidBody* m_child  = nullptr;

        math::Vector3    m_anchorParent;
        math::Vector3    m_anchorChild;
        math::Quaternion m_frameParent = math::Quaternion::Identity();
        math::Quaternion m_frameChild  = math::Quaternion::Identity();

        XPBDJointLimits m_limits;
        XPBDJointDrive  m_drive;
        float           m_socketCompliance = 0.0f;

        float m_lambdaSocket = 0.0f;
        float m_lambdaTwist  = 0.0f;
        float m_lambdaSwing  = 0.0f;
        float m_lambdaDrive  = 0.0f;

        float m_twistAngle     = 0.0f;
        float m_swingAngleY    = 0.0f;
        float m_swingAngleZ    = 0.0f;
        bool  m_driveSaturated = false;
        bool  m_limited        = false;
    };
} // namespace fbzz::physics
