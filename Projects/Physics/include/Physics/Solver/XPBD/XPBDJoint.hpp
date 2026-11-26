/// @file    XPBDJoint.hpp
/// @brief   ボールソケット + swing/twist 制限 + 角度ドライブを 1 本にまとめた関節
/// @author  Hasegawa Jin
/// @date    2026-09-02

/// @note 関節フレームの X は twist、Y/Z は swing 軸で、Build 時の相対姿勢をたわみ 0 とする。
/// @note 同じフレームとアンカーを共有し、ドライブ → 制限 → ソケットの順で解く。
/// @note 非対称の可動域を表すため、swing は円錐でなく軸ごとの上下限を使う。
/// @see Docs/design/active-ragdoll.md
#pragma once
#include <Physics/Api.hpp>

#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

namespace fbzz::physics
{
    /// @note 関節の可動域 [rad]。すべて «たわみ 0» からの符号付きの量。
    struct XPBDJointLimits
    {
        bool  enabled = false; ///< 親子の相対角度を可動域に制限するかどうか。
        float twistMin = 0.0f; ///< 関節フレームの X 軸まわりのねじり下限 [rad]。
        float twistMax = 0.0f; ///< 関節フレームの X 軸まわりのねじり上限 [rad]。
        float swingMinY = 0.0f; ///< 関節フレームの Y 軸まわりの曲げ下限 [rad]。
        float swingMaxY = 0.0f; ///< 関節フレームの Y 軸まわりの曲げ上限 [rad]。
        float swingMinZ = 0.0f; ///< 関節フレームの Z 軸まわりの曲げ下限 [rad]。
        float swingMaxZ = 0.0f; ///< 関節フレームの Z 軸まわりの曲げ上限 [rad]。
        float compliance = 0.0f; ///< 0 で «そこで止まる»。大きくすると止まり際に食い込む (ゴムの止まり方)。
    };

    /// @note 目標姿勢へ引き戻すサーボ。
    struct XPBDJointDrive
    {
        bool  enabled = false; ///< false で完全に脱力する (Passive ラグドール)。
        float compliance = 1.0e-4f; ///< 0 で剛。単位 [rad/(N·m)]。定常たわみは compliance × トルクで、刻みに依らない。
        float damping = 0.0f; ///< 相対角速度を削る割合 [1/s]。ドライブの D 項。
        float maxTorque = 0.0f; ///< 比例・減衰を合算したトルクの上限 [N·m]。0 以下で無制限。 超えた関節は目標へ追従できず back-drive する ─ «モーターが力負けする» 画。
        math::Quaternion target = math::Quaternion::Identity(); ///< 関節フレームでの目標相対姿勢。Identity で «たわみ 0 の姿勢»。
    };

    class XPBDJoint final : public XPBDConstraint
    {
    public:
        /// @param[in] parent null なら «ワールドへ固定» になる。ローカル量はワールド量として扱う。
        FBZZ_PHYSICS_API XPBDJoint(RigidBody* parent, RigidBody* child);

        /// @note 今の両者の姿勢を «たわみ 0» として、アンカーと関節フレームを組む。

        /// @param[in] worldAnchor 関節の位置 (ワールド)。
        /// @param[in] worldFrame  関節フレームの姿勢 (ワールド)。X が骨の向きになるように渡す。
        FBZZ_PHYSICS_API void Build(const math::Vector3& worldAnchor, const math::Quaternion& worldFrame);

        [[nodiscard]] XPBDJointLimits& Limits() { return m_limits; }
        [[nodiscard]] XPBDJointDrive&  Drive()  { return m_drive;  }
        [[nodiscard]] const XPBDJointLimits& Limits() const { return m_limits; }
        [[nodiscard]] const XPBDJointDrive&  Drive()  const { return m_drive;  }

        /// @note ソケットの柔らかさ [m/N]。0 で «伸びない»。
        void SetSocketCompliance(float compliance) { m_socketCompliance = compliance; }

        [[nodiscard]] RigidBody* GetParent() const { return m_parent; }
        [[nodiscard]] RigidBody* GetChild()  const { return m_child;  }

        /// @note Build() が組んだ関節フレーム (各剛体ローカル)。
        /// @note ドライブの目標を «別の姿勢» から計算する側と、可動域のデバッグ表示が使う。
        [[nodiscard]] math::Quaternion GetFrameParent() const { return m_frameParent; }
        [[nodiscard]] math::Quaternion GetFrameChild()  const { return m_frameChild;  }
        [[nodiscard]] math::Vector3    GetAnchorChild() const { return m_anchorChild;  }

        /// @note 可動域を «今どこを向いているか» ごと描くための、ワールドでの関節フレーム。
        [[nodiscard]] math::Quaternion GetParentFrameWorld() const { return ParentFrameWorld(); }
        [[nodiscard]] math::Quaternion GetChildFrameWorld()  const { return ChildFrameWorld();  }
        [[nodiscard]] math::Vector3    GetAnchorParentWorld() const { return ParentAnchorWorld(); }

        FBZZ_PHYSICS_API void ResetLambda() override;
        FBZZ_PHYSICS_API void SolvePosition(float h) override;
        FBZZ_PHYSICS_API void SolvePhysicalPosition(float h) override;
        FBZZ_PHYSICS_API void SolveCorrectionPosition(float h) override;
        FBZZ_PHYSICS_API void SolveVelocity(float h) override;

        class VelocityProjection
        {
            friend class XPBDJoint;
            struct Limit
            {
                math::Vector3 axis; ///< 侵入方向の角速度を取り除くワールド空間の軸。
                bool atLower = false; ///< 関節角がこの軸の下限へ達しているかどうか。
                bool atUpper = false; ///< 関節角がこの軸の上限へ達しているかどうか。
            };

            math::Vector3 m_childArm; ///< 子の重心からソケットのアンカーへのワールドオフセット [m]。
            math::Vector3 m_parentArm; ///< 親の重心からソケットのアンカーへのワールドオフセット [m]。
            math::Vector3 m_columnX; ///< ワールド X 方向の力積へのソケットの相対速度応答。
            math::Vector3 m_columnY; ///< ワールド Y 方向の力積へのソケットの相対速度応答。
            math::Vector3 m_columnZ; ///< ワールド Z 方向の力積へのソケットの相対速度応答。
            math::Vector3 m_cofactorX; ///< ソケットの速度応答を逆算する行列の X 方向の余因子。
            float m_determinant = 0.0f; ///< ソケットの速度応答行列の行列式。退化の判定にも使う。
            Limit m_limits[3]{}; ///< 速度を射影する関節の角度制限の作業配列。
            int m_limitCount = 0; ///< m_limits に格納した角度制限の数。
        };

        /// @note 硬いソケットと可動域の速度射影を準備する。モーターの減衰は SolveVelocity が扱う。
        [[nodiscard]] FBZZ_PHYSICS_API VelocityProjection PrepareVelocityProjection() const;
        /// @pre 準備時と同じ関節、姿勢、可動域、有効逆質量・逆慣性を使うこと。速度は変更してよい。
        FBZZ_PHYSICS_API void ProjectVelocity(const VelocityProjection& projection);

        /// @note 直前の substep で測ったたわみ [rad]。可動域の調整と、崩れ方の診断に使う。
        [[nodiscard]] float GetTwistAngle() const { return m_twistAngle; }
        [[nodiscard]] float GetSwingAngleY() const { return m_swingAngleY; }
        [[nodiscard]] float GetSwingAngleZ() const { return m_swingAngleZ; }
        /// @note 直前の substep で位置追従または合算出力がトルク上限に達したか。減衰で出力が減っても位置追従の力不足を残す。
        [[nodiscard]] bool  IsDriveSaturated() const { return m_driveSaturated; }
        /// @brief 可動域に食い込んで押し戻されているか。
        /// @note 飽和と分ける理由: «アニメーションどおりに動かない» の原因は «力が足りない»
        /// @note (飽和) と «そこまで曲げてよいことになっていない» (可動域) の 2 つあり、
        /// @note 絵はどちらも同じ «崩れる» にしか見えないため切り分けが要る。
        [[nodiscard]] bool  IsLimited() const { return m_limited; }

        /// @brief 与えられた関節角が可動域の内側へ入るまで、範囲を広げる (狭めはしない)。
        /// @note 押し戻し側と同じクラスに置く理由: 学習側と検査側で «角度の測り方» が
        /// @note 1 つでも違うと «広げたはずなのに押し戻される» という形でしか現れない。
        /// @note どちらも SolveLimits と同じ分解 (DecomposeSwingTwist + TwistAngle) を通す。

        /// @param[in] deviation 親フレームから見た子フレームの相対回転。
        /// @param[in] margin    測った角へ足す余裕 [rad]。押されて少し越える分を吸収する。
        FBZZ_PHYSICS_API void LearnLimits(const math::Quaternion& deviation, float margin);

        /// @note 今の姿勢をそのまま可動域へ取り込む。
        /// @note クリップの要求値だけを学習すると、捕獲直後の «実際の角» が範囲外に残ることがある。
        FBZZ_PHYSICS_API void LearnLimitsFromCurrentPose(float margin);

    private:
        [[nodiscard]] FBZZ_PHYSICS_API math::Quaternion ParentFrameWorld() const;
        [[nodiscard]] FBZZ_PHYSICS_API math::Quaternion ChildFrameWorld()  const;
        [[nodiscard]] FBZZ_PHYSICS_API math::Vector3    ParentAnchorWorld() const;

        FBZZ_PHYSICS_API void SolveDrive(float h);
        FBZZ_PHYSICS_API void SolveDriveVelocity(float h);
        FBZZ_PHYSICS_API void SolveLimits(float h);
        FBZZ_PHYSICS_API void SolveSocket(float h);

        RigidBody* m_parent = nullptr; ///< 関節の親剛体の非所有参照。nullptr はワールド固定。
        RigidBody* m_child  = nullptr; ///< 関節の子剛体の非所有参照。

        math::Vector3    m_anchorParent; ///< 親の剛体ローカルでの接続点 [m]。親が固定ワールドならワールド位置。
        math::Vector3    m_anchorChild; ///< 子の剛体ローカルでの接続点 [m]。
        math::Quaternion m_frameParent = math::Quaternion::Identity(); ///< 親の剛体ローカルでの関節フレーム。親が固定ワールドならワールド姿勢。
        math::Quaternion m_frameChild  = math::Quaternion::Identity(); ///< 子の剛体ローカルでの関節フレーム。

        XPBDJointLimits m_limits; ///< 関節フレームを基準にしたねじり・曲げの可動域。
        XPBDJointDrive  m_drive; ///< 関節を目標姿勢へ追従させる角度ドライブの設定。
        float           m_socketCompliance = 0.0f; ///< アンカーを結ぶソケット拘束の柔らかさ [m/N]。

        float m_lambdaSocket = 0.0f; ///< 今のサブステップでソケット拘束が蓄積した乗数。
        float m_lambdaTwist  = 0.0f; ///< 今のサブステップでねじりの可動域拘束が蓄積した乗数。
        float m_lambdaSwing  = 0.0f; ///< 今のサブステップで曲げの可動域拘束が蓄積した乗数。
        float m_lambdaDrive  = 0.0f; ///< 今のサブステップで位置ドライブが蓄積した乗数。
        math::Vector3 m_driveImpulse; ///< 今のサブステップで速度ドライブが蓄積した角力積 [N·m·s]。

        float m_twistAngle     = 0.0f; ///< 直近で測った基準姿勢からのねじり角 [rad]。
        float m_swingAngleY    = 0.0f; ///< 直近で測った関節フレーム Y 軸まわりの曲げ角 [rad]。
        float m_swingAngleZ    = 0.0f; ///< 直近で測った関節フレーム Z 軸まわりの曲げ角 [rad]。
        bool  m_driveSaturated = false; ///< 直近のサブステップでドライブが出力上限に達したかどうか。
        bool  m_limited        = false; ///< 直近のサブステップで可動域からはみ出したかどうか。
    };
} /// @note namespace fbzz::physics
