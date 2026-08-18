// FBZZ Engine
// EnemyChaserComponent.hpp | sandbox
// プレイヤーへ接近して接触攻撃する MVP 用ノーマルスライム AI
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyChaserComponent : public Script {
    FBZZ_SCRIPT(EnemyChaserComponent)
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)

public:
    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed, 3.2f, "Move Speed", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, stopDistance, 1.35f, "Stop Distance", 0.1f, 10.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed, 10.0f, "Turn Speed", 0.0f, 30.0f)

    FBZZ_GROUP("Attack")
    FBZZ_FIELD_RANGE_INT(int, attackDamage, 1, "Attack Damage", 1, 100)
    FBZZ_FIELD_RANGE(float, attackCooldown, 1.0f, "Attack Cooldown", 0.05f, 10.0f)

    FBZZ_GROUP("Target")
    FBZZ_FIELD(std::string, playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Idle", "State")

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    void OnCollisionStay(const CollisionInfo& info) override;

private:
    [[nodiscard]] bool IsMovementLocked() const;
    void StopHorizontal();

    EntityRef m_player;
    float m_attackRemaining = 0.0f;
};

FBZZ_REFLECT(EnemyChaserComponent)

inline void EnemyChaserComponent::OnStart()
{
    if (GameObject* player = scene.FindWithTag(playerTag))
        m_player = EntityRef{ player->GetID() };
    physics.SetFreezeRotation(true, true, true);
    m_attackRemaining = 0.0f;
    if (!scene.GetScript<PolarityBodyComponent>() ||
        !scene.GetScript<PolarityTargetComponent>() ||
        !scene.GetScript<EnemyHealthComponent>()) {
        debug.LogError("EnemyChaserComponent requires PolarityBody, PolarityTarget, and EnemyHealth scripts.");
    }
}

inline void EnemyChaserComponent::OnUpdate()
{
    m_attackRemaining = std::max(0.0f, m_attackRemaining - Time::deltaTime);
    if (!m_player.Resolve(scene)) {
        if (GameObject* player = scene.FindWithTag(playerTag))
            m_player = EntityRef{ player->GetID() };
    }
}

inline bool EnemyChaserComponent::IsMovementLocked() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const auto* body = scene.GetScript<PolarityBodyComponent>();
    const auto* target = scene.GetScript<PolarityTargetComponent>();
    return !health || !health->IsAlive() ||
           (body && (body->IsBeingPulled() || body->IsStunned())) ||
           (target && target->IsHitReacting());
}

inline void EnemyChaserComponent::StopHorizontal()
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    physics.SetVelocity(velocity);
}

inline void EnemyChaserComponent::OnFixedUpdate()
{
    if (IsMovementLocked()) {
        debugState = "Locked";
        StopHorizontal();
        return;
    }

    GameObject* player = m_player.Resolve(scene);
    if (!player) {
        debugState = "No Player";
        StopHorizontal();
        return;
    }

    Vector3 direction = player->transform.worldPosition - transform.worldPosition;
    direction.y = 0.0f;
    const float distanceSq = direction.LengthSq();
    if (distanceSq <= stopDistance * stopDistance || distanceSq < EPSILON) {
        debugState = "Attack Range";
        StopHorizontal();
        return;
    }

    direction = direction.Normalized();
    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(moveSpeed, 0.0f);
    velocity.z = direction.z * std::max(moveSpeed, 0.0f);
    physics.SetVelocity(velocity);

    const float response = 1.0f - std::exp(-std::max(turnSpeed, 0.0f) * time.FixedDeltaTime());
    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (rb && rb->rigidBody) {
        rb->rigidBody->SetRotation(Quaternion::Slerp(
            rb->rigidBody->GetRotation(), Quaternion::LookRotation(direction), response).Normalized());
    }
    debugState = "Chasing";
}

inline void EnemyChaserComponent::OnCollisionStay(const CollisionInfo& info)
{
    GameObject* player = m_player.Resolve(scene);
    if (!player || info.other != player || m_attackRemaining > 0.0f || IsMovementLocked())
        return;

    if (auto* health = scene.GetScript<PlayerHealthComponent>(player)) {
        if (health->TakeDamage(std::max(attackDamage, 1)))
            m_attackRemaining = std::max(attackCooldown, 0.05f);
    }
}

} // namespace sandbox
