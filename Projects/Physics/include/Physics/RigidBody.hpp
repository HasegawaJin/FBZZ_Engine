/// @file    RigidBody.hpp
/// @brief   剛体の状態と力の適用。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::physics
{
    class Collider;

    struct AxisLock
    {
        bool x = false;
        bool y = false;
        bool z = false;
    };

    /// @brief PhysicsSystem から同期される剛体状態。
    /// @note Scene の Transform は直接所有せず、World::Step 後に外側で反映する。
    class RigidBody
    {
    public:
        /// @brief 力・インパルスの適用。Force は積分時まで蓄積し、Impulse は速度へ即時反映する。
        void ApplyForce(const math::Vector3& force);
        /// @brief World 内部用: 重力などの常時力を sleep timer をリセットせず蓄積する。
        /// @note 通常の ApplyForce は Script/API からの外力として扱い WakeUp するが、
        ///       重力まで WakeUp すると接地中の剛体が永久に Sleep できない。
        void ApplyForceNoWake(const math::Vector3& force);
        void ApplyForceAtPoint(const math::Vector3& force,
                                const math::Vector3& worldPoint);
        void ApplyImpulse(const math::Vector3& impulse);
        /// @brief 重心から外れた点へのインパルス。並進と回転の両方が変わる。
        /// @note 重心へのインパルスは物体を «押す» だけだが、端を殴れば回る。ラグドールが
        ///       瓦礫を蹴るときに必要なのは後者で、重心へ入れると不自然な弾き方になる。
        void ApplyImpulseAtPoint(const math::Vector3& impulse,
                                 const math::Vector3& worldPoint);
        /// @brief PhysicsSolver から使用。
        void ApplyAngularImpulse(const math::Vector3& angularImpulse);
        void ApplyTorque(const math::Vector3& torque);
        /// @brief World 内部用: Volume が毎 substep 掛ける環境トルク。ApplyForceNoWake と同じ理由で Wake しない。
        void ApplyTorqueNoWake(const math::Vector3& torque);

        /// @brief 積分 (半陰的オイラー法、World::Step から呼ばれる)。
        void Integrate(float dt);

        /// @brief 質量の設定。負値可 (反重力挙動用として許容する)。
        void SetMass(float mass);
        float GetMass()    const { return m_mass; }
        float GetInvMass() const { return (m_isStatic || m_isSleeping) ? 0.0f : m_invMass; }
        bool  IsStatic()   const { return m_isStatic; }
        bool  IsSleeping() const { return m_isSleeping; }
        void  WakeUp();
        void  Sleep();
        void  UpdateSleepState(float dt, float linearThreshold, float angularThreshold, float sleepTime);

        /// @name 状態アクセス
        /// @{
        math::Vector3    GetPosition()        const { return m_position;        }
        math::Vector3    GetVelocity()        const { return m_velocity;        }
        math::Quaternion GetRotation()        const { return m_rotation;        }
        math::Vector3    GetAngularVelocity() const { return m_angularVelocity; }

        void SetPosition(const math::Vector3& pos);
        void SetVelocity(const math::Vector3& vel);
        void SetAngularVelocity(const math::Vector3& angVel);
        void SetRotation(const math::Quaternion& rot);
        void SetFreezePosition(const AxisLock& lock);
        void SetFreezeRotation(const AxisLock& lock);
        AxisLock GetFreezePosition() const { return m_freezePosition; }
        AxisLock GetFreezeRotation() const { return m_freezeRotation; }
        /// @}

        /// @name 動作制御
        /// @{
        bool  m_isStatic    = false;  ///< true のとき積分・衝突解決をスキップ
        /// @}

        void SetInertiaFromCollider(const Collider* collider);

        /// @brief 媒質 (風・水流) との結合係数 [1/s]。FlowVolume が F = k * m * (v_flow - v) で使う。
        /// @note 既定 0 = 流れを受けない (オプトイン)。既定で全剛体が風に流されると、
        ///       置いてある箱や敵が勝手に動き出す。粒子と違って剛体はゲームプレイの
        ///       当事者なので、受けるかどうかは体ごとに宣言させる。
        /// @note 正本は RigidBodyComponent::flowCoupling で、PhysicsSystem が毎フレーム
        ///       ここへ押し込む。ランタイム専用なので RigidBodySerializer は書かない。
        /// @see Docs/design/flow-field.md §2
        void SetFlowCoupling(float coupling) { m_flowCoupling = coupling; }
        [[nodiscard]] float GetFlowCoupling() const { return m_flowCoupling; }

        /// @name 拡張プロパティ
        /// @{

        /// @brief Magnetic VolumeComponent 用の Lorentz 力 F = m_charge * (v × B)。0 のとき磁場の影響を受けない。
        float m_charge = 0.0f;

        /// @brief N 体重力引力用 (World::ApplyGravitationalAttraction)。true のとき同フラグを持つ
        ///        他ボディと F = G*m₁*m₂/r² を双方向で受ける。
        bool  m_isGravitationalSource = false;
        float m_gravitationalMass     = 1.0f;  ///< 慣性質量 m_mass と独立して設定可

        void* m_userData = nullptr;  ///< engine 側コンポーネントへのポインタ (衝突コールバック用)。Physics は型を知らない。

        /// @brief CCD (Continuous Collision Detection) 設定。SphereCollider を持つ高速・小型オブジェクトのみ有効にする。
        bool  m_useCCD    = false;  ///< true のとき World::CCDPhase で TOI を計算する
        float m_ccdRadius = 0.5f;   ///< CCD 判定の代表半径 (SphereCollider の半径に合わせる)

        /// @brief 重力と減衰のゲーム向け調整値。World の重力ベクトルは共有し、剛体ごとに倍率だけ変える。
        bool  m_useGravity  = true;
        float m_gravityScale = 1.0f;
        /// ワールドのステップに対する時計倍率。ランタイム専用。
        float m_timeScale = 1.0f;
        float m_linearDrag   = 0.0f;
        float m_angularDrag  = 0.0f;
        bool  m_allowSleeping = true;
        bool  m_isSleeping = false;
        float m_sleepTimer = 0.0f;
        /// @}

    private:
        /// @name 状態
        /// @{
        math::Vector3    m_position;            ///< ワールド空間の位置
        math::Vector3    m_velocity;            ///< ワールド空間の速度
        math::Vector3    m_force;               ///< ワールド空間の合計力 (次のステップで速度に反映)
        math::Quaternion m_rotation;            ///< ワールド空間の回転 (単位クォータニオン)
        math::Vector3    m_angularVelocity;     ///< ワールド空間の角速度 (回転軸 * 角速度の大きさ)
        math::Vector3    m_torque;              ///< ワールド空間の合計トルク (次のステップで角速度に反映)
        /// @}

        float m_mass = 1.0f; ///< 質量 (負値可)
        float m_invMass = 1.0f; ///< m_isStatic == true のとき 0。m_mass < 0 のとき負になる (反重力挙動)

        /// @brief 対角慣性テンソルの逆数 (ボディ空間)。完全な 3x3 テンソルではなく対角近似にして、
        ///        Step 4-6 の単純なソルバーに合わせる。SetMass() / SetInertiaFromCollider() 呼び出し時に
        ///        自動再計算される。m_isStatic == true のとき {0,0,0}。
        math::Vector3 m_invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        const Collider* m_inertiaCollider = nullptr;
        AxisLock m_freezePosition;
        AxisLock m_freezeRotation;
        float m_flowCoupling = 0.0f; ///< 媒質との結合係数 [1/s]。0 で流れを受けない

        void RecomputeInertia();
        math::Vector3 ApplyPositionFreeze(const math::Vector3& value,
                                          const math::Vector3& base) const;
        math::Vector3 ApplyRotationFreeze(const math::Vector3& value) const;

    public:
        /// @brief ワールド空間で I⁻¹ * v を計算する (= R * (m_invInertiaDiag ⊙ (Rᵀ * v)))。
        /// @note PhysicsSolver::Resolve から使用するため public。
        math::Vector3 ApplyInvInertia(const math::Vector3& v) const;
    };
} // namespace fbzz::physics
