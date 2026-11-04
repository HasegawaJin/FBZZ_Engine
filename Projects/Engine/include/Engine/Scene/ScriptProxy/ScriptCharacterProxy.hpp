/// @file    ScriptCharacterProxy.hpp
/// @brief   Script から CharacterControllerComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note Tick / Jump / RegisterGroundContact を呼び出し元から RigidBody 取得コードを排除し、
/// @note isGrounded / verticalSpeed / groundNormal を読み取れるようにする。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;
struct CollisionInfo;

struct ScriptCharacterProxy {
    Script* script = nullptr;

    /// @brief OnUpdate 先頭で呼ぶ。CharacterControllerComponent::Tick を rb を自動取得して実行する。
    FBZZ_ENGINE_API void Tick(float dt) const;

    /// @brief ジャンプを実行する。RigidBody のインパルス適用と接地ステート遷移を一括で行う。
    FBZZ_ENGINE_API void Jump(const math::Vector3& impulse) const;
    /// @brief 目標の上向き速度 [m/s] を指定してジャンプする。質量・インパルス換算は Engine が行う。
    FBZZ_ENGINE_API void JumpAtVelocity(float verticalSpeed) const;

    /// @brief 水平速度だけを CharacterController 経由で変更する。Y 速度は保持する。
    FBZZ_ENGINE_API void SetHorizontalVelocity(const math::Vector3& velocity) const;
    FBZZ_ENGINE_API void AddHorizontalVelocity(const math::Vector3& velocity) const;
    FBZZ_ENGINE_API void Move(const math::Vector3& desiredVelocity,
              float dt,
              float acceleration = -1.0f,
              float deceleration = -1.0f) const;
    FBZZ_ENGINE_API math::Vector3 GetVelocity() const;
    FBZZ_ENGINE_API math::Vector3 GetHorizontalVelocity() const;

    /// @brief 特殊な接地を Script から追加通知する。通常の物理接触は Engine が自動通知する。
    FBZZ_ENGINE_API void RegisterGroundContact(const CollisionInfo& info) const;

    FBZZ_ENGINE_API bool          IsGrounded()       const;
    FBZZ_ENGINE_API float         GetVerticalSpeed() const;
    FBZZ_ENGINE_API math::Vector3 GetGroundNormal()  const;

    /// @name 手動接地
    /// @note 梯子・水中・飛行など、接地判定をゲーム側で制御するための API。
    /// @{
    FBZZ_ENGINE_API void ForceGrounded(const math::Vector3& normal = math::Vector3::UP) const;
    FBZZ_ENGINE_API void ForceAirborne() const;
    FBZZ_ENGINE_API void UseAutomaticGrounding() const;
    /// @}

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API bool IsEnabled() const;
};

} /// @note namespace fbzz::scene
