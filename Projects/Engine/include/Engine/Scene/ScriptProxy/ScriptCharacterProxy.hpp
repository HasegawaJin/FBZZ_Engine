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

    // OnCollisionEnter / OnCollisionStay から呼ぶ。歩行可能面への接触を Controller へ通知する。
    void RegisterGroundContact(const CollisionInfo& info) const;

    bool          IsGrounded()       const;
    float         GetVerticalSpeed() const;
    math::Vector3 GetGroundNormal()  const;

    void SetEnabled(bool enabled) const;
};

} // namespace fbzz::scene
