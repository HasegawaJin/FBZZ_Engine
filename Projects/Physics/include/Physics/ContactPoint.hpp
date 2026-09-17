/// @file    ContactPoint.hpp
/// @brief   NarrowPhase が生成する衝突接触点データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/Vector3.hpp>

namespace fbzz::physics 
{

    class RigidBody;
    class Collider;
    struct PhysicsMaterial;

    /// World が bodies を生存管理するため raw pointer で保持する。
    /// normal は bodyB から bodyA へ押し戻す向きで統一する。
    struct ContactPoint {
        math::Vector3 point;   ///< 衝突点 (ワールド座標)
        math::Vector3 normal;  ///< b → a 方向の法線
        float         depth;   ///< 貫通深度 (正の値)
        RigidBody*    bodyA = nullptr;
        RigidBody*    bodyB = nullptr;
        const Collider* colliderA = nullptr;
        const Collider* colliderB = nullptr;
        const PhysicsMaterial* materialA = nullptr;
        const PhysicsMaterial* materialB = nullptr;
        bool isTrigger = false;
        bool cacheImpulse = true;

        /// Warm Starting / PGS 用: ContactCache が設定し ResolveVelocity が更新する
        math::Vector3 tangent[2];                    ///< 摩擦平面の 2 軸 (Resolve 冒頭で計算)
        float cachedNormalImpulse       = 0.0f;      ///< 蓄積法線インパルス (Λn ≥ 0 にクランプ)
        float cachedTangentImpulse[2]   = {0.0f, 0.0f}; ///< 蓄積摩擦インパルス
        float positionCorrectionWeight  = 1.0f;
    };

} // namespace fbzz::physics
