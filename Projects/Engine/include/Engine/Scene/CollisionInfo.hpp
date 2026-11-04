/// @file    CollisionInfo.hpp
/// @brief   物理通知と接地制御が共有する非所有の接触情報。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Math/Vector3.hpp>
namespace fbzz::scene {
class GameObject;
struct ColliderComponent;
struct CollisionInfo {
    GameObject* self = nullptr;
    GameObject* other = nullptr;
    const ColliderComponent* selfCollider = nullptr;
    const ColliderComponent* otherCollider = nullptr;
    math::Vector3 contactNormal = math::Vector3::UP;
    math::Vector3 contactPoint = math::Vector3::ZERO;
    float contactDepth = 0.0f;

    /// @name 衝突の強さ
    /// @note コールバックは速度解決後に呼ばれるため、GetVelocity() は衝突後の値しか返さない。解決前の勢いはここに記録する。
    /// @note OnCollisionEnter でのみ意味を持つ (Stay / Exit では 0)。
    /// @{
    math::Vector3 relativeVelocity = math::Vector3::ZERO; ///< @note 接触点での相対速度 (self から見た other との差、角速度の寄与を含む)。
    float approachSpeed = 0.0f; ///< @note 法線方向の接近速度 [m/s]。正 = 近づいていた強さ。一定速度以上の衝突だけダメージにする判定に使う。
    float impactImpulse = 0.0f; ///< @note 解決で加わった法線インパルス (質量込み)。軽重で手応えを変えたいときは approachSpeed でなくこちらを見る。
    /// @}
};

} /// @note namespace fbzz::scene
