// FBZZ Engine
// ScriptCharacterProxy.hpp | fbzz::scene
// Script から CharacterControllerComponent を操作するショートハンド。
// Tick / Jump / RegisterGroundContact を呼び出し元から RigidBody 取得コードを排除し、
// isGrounded / verticalSpeed / groundNormal を読み取れるようにする。
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;
struct CollisionInfo;

struct ScriptCharacterProxy {
    Script* script = nullptr;

    // OnUpdate 先頭で呼ぶ。CharacterControllerComponent::Tick を rb を自動取得して実行する。
    void Tick(float dt) const;

    // ジャンプを実行する。RigidBody のインパルス適用と接地ステート遷移を一括で行う。
    void Jump(const math::Vector3& impulse) const;
    // 目標の上向き速度 (m/s) を指定してジャンプする。質量・インパルス換算は Engine が行う。
    void JumpAtVelocity(float verticalSpeed) const;

    // 水平速度だけを CharacterController 経由で変更する。Y 速度は保持する。
    void SetHorizontalVelocity(const math::Vector3& velocity) const;
    void AddHorizontalVelocity(const math::Vector3& velocity) const;
    void Move(const math::Vector3& desiredVelocity,
              float dt,
              float acceleration = -1.0f,
              float deceleration = -1.0f) const;
    math::Vector3 GetVelocity() const;
    math::Vector3 GetHorizontalVelocity() const;

    // 特殊な接地を Script から追加通知する。通常の物理接触は Engine が自動通知する。
    void RegisterGroundContact(const CollisionInfo& info) const;

    bool          IsGrounded()       const;
    float         GetVerticalSpeed() const;
    math::Vector3 GetGroundNormal()  const;

    // 梯子・水中・飛行など、接地判定をゲーム側で制御するための API。
    void ForceGrounded(const math::Vector3& normal = math::Vector3::UP) const;
    void ForceAirborne() const;
    void UseAutomaticGrounding() const;

    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
};

} // namespace fbzz::scene
