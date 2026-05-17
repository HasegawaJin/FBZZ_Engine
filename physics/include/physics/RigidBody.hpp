// FBZZ Engine
// RigidBody.hpp | fbzz::physics
// 剛体の状態と力の適用
#pragma once
#include <math/Vector3.hpp> // math::Vector3
#include <math/Quaternion.hpp> // math::Quaternion
#include <memory> // std::shared_ptr
#include <physics/Collider.hpp> // physics::Collider
#include <physics/PhysicsMaterial.hpp> // physics::PhysicsMaterial

namespace fbzz::physics
{
    class RigidBody
    {
    public:
        // 力・インパルスの適用
        void ApplyForce(const math::Vector3& force);
        void ApplyForceAtPoint(const math::Vector3& force,
                                const math::Vector3& worldPoint);
        void ApplyImpulse(const math::Vector3& impulse);
        void ApplyAngularImpulse(const math::Vector3& angularImpulse); // PhysicsSolver から使用
        void ApplyTorque(const math::Vector3& torque);

        // 積分 (半陰的オイラー法、World::Step から呼ばれる)
        void Integrate(float dt);

        // 質量管理
        void SetMass(float mass); // 負値可 (反重力挙動)
        float GetMass()    const { return m_mass; }
        float GetInvMass() const { return m_isStatic ? 0.0f : m_invMass; }
        bool  IsStatic()   const { return m_isStatic; }

        // 状態アクセス
        math::Vector3    GetPosition()        const { return m_position;        }
        math::Vector3    GetVelocity()        const { return m_velocity;        }
        math::Quaternion GetRotation()        const { return m_rotation;        }
        math::Vector3    GetAngularVelocity() const { return m_angularVelocity; }

        void SetPosition(const math::Vector3& pos);
        void SetVelocity(const math::Vector3& vel);
        void SetRotation(const math::Quaternion& rot);

        // 物理マテリアル (反発・摩擦・密度)
        // 詳細: docs_helper/physics/material.md
        PhysicsMaterial m_material;

        // 動作制御
        bool  m_isStatic    = false;  // true のとき積分・衝突解決をスキップ

        // Collider 紐付け
        void SetCollider(std::shared_ptr<Collider> collider);
        std::shared_ptr<Collider> GetCollider() const { return m_collider; }

        // --- 拡張プロパティ ---

        // MagneticVolume 用: Lorentz 力 F = m_charge * (v × B)
        // 0 のとき磁場の影響を受けない
        float m_charge = 0.0f;

        // N 体重力引力用: World::ApplyGravitationalAttraction で使用
        // true のとき同フラグを持つ他ボディと F = G*m₁*m₂/r² を双方向で受ける
        bool  m_isGravitationalSource = false;
        float m_gravitationalMass     = 1.0f;  // 慣性質量 m_mass と独立して設定可

        // engine 側コンポーネントへのポインタ (衝突コールバック用)
        void* m_userData = nullptr;

    private:
        // 状態
        math::Vector3    m_position;            // ワールド空間の位置
        math::Vector3    m_velocity;            // ワールド空間の速度
        math::Vector3    m_force;               // ワールド空間の合計力 (次のステップで速度に反映)
        math::Quaternion m_rotation;            // ワールド空間の回転 (単位クォータニオン)
        math::Vector3    m_angularVelocity;     // ワールド空間の角速度 (回転軸 * 角速度の大きさ) 
        math::Vector3    m_torque;              // ワールド空間の合計トルク (次のステップで角速度に反映)

        float m_mass = 1.0f; // 質量 (負値可)
        float m_invMass = 1.0f; // m_isStatic == true のとき 0
                                // m_mass < 0 のとき負になる (反重力挙動)
            
        // 対角慣性テンソルの逆数 (ボディ空間)
        // SetMass() / SetCollider() 呼び出し時に自動再計算される
        // m_isStatic == true のとき {0,0,0}
        math::Vector3 m_invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        std::shared_ptr<Collider> m_collider;

        void RecomputeInertia(); // SetMass / SetCollider から呼ばれる

    public:
        // ワールド空間で I⁻¹ * v を計算する
        // = R * (m_invInertiaDiag ⊙ (Rᵀ * v))
        // PhysicsSolver::Resolve から使用するため public
        math::Vector3 ApplyInvInertia(const math::Vector3& v) const;
    };
} // namespace fbzz::physics