/// @file    CollisionPair.hpp
/// @brief   BroadPhase が生成する衝突候補ペア。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Collision/Shapes/Collider.hpp>
#include <Physics/Dynamics/PhysicsMaterial.hpp>

namespace fbzz::physics
{
    class RigidBody;

    /// World に登録されるコライダー参照。すべてのフィールドは非所有ポインタ。
    /// @note BroadPhase で毎 substep 大量に複製されるため、shared_ptr の atomic refcount を排除して
    ///       衝突検出のコストを最小化する。所有権は ColliderComponent (unique_ptr) が持つ。
    struct ColliderInstance {
        Collider* collider = nullptr; ///< 登録された形状の非所有ポインター。
        RigidBody* body = nullptr; ///< 形状を移動させる剛体の非所有ポインター。nullptr は剛体なし。
        const PhysicsMaterial* material = nullptr; ///< 接触解決に使う材質の非所有ポインター。nullptr は既定材質。
        math::Vector3 centerOffset = math::Vector3::ZERO; ///< 剛体ローカルでのコライダー中心のオフセット [m]。
        bool isTrigger = false; ///< 重なりを通知し、接触力や押し戻しを加えないかどうか。
        int layer = 0; ///< レイヤー衝突表で使う形状のレイヤー番号。
    };

    /// NarrowPhase はこのペアだけを詳細判定する。
    /// @note m_colliders は NarrowPhase 中に再確保されないため、非所有ポインタで十分。
    struct CollisionPair {
        const ColliderInstance* colliderA = nullptr; ///< 詳細判定を行う 1 番目の登録。World が所有する非所有参照。
        const ColliderInstance* colliderB = nullptr; ///< 詳細判定を行う 2 番目の登録。World が所有する非所有参照。
    };

} // namespace fbzz::physics
