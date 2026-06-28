// FBZZ Engine
// WeaponHitboxComponent.hpp | sandbox
// 剣・盾の Trigger Hitbox 同士が接触したときに衝撃アニメーションを再生するスクリプト
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include "SwordTrailComponent.hpp"
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class WeaponHitboxComponent : public Script {
    FBZZ_SCRIPT(WeaponHitboxComponent)

public:
    FBZZ_GROUP("Hitbox")
    FBZZ_FIELD(std::string, hitboxType, "Sword", "Hitbox Type")
    FBZZ_FIELD(std::string, ownerName, "", "Owner Name")
    FBZZ_FIELD(Vector3, colliderCenter, Vector3::ZERO, "Collider Center")
    FBZZ_FIELD(Vector3, colliderSize, Vector3(0.18f, 0.18f, 1.20f), "Collider Size")
    FBZZ_FIELD_RANGE(float, swingStartTime, 0.18f, "Swing Start Time", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingEndTime, 0.78f, "Swing End Time", 0.0f, 1.0f)

    FBZZ_GROUP("Impact")
    FBZZ_FIELD(std::string, impactTriggerParam, "Impact", "Impact Trigger Param")
    FBZZ_FIELD(std::string, hitTriggerParam, "Hit", "Hit Trigger Param")
    FBZZ_FIELD_RANGE(float, impactCooldown, 0.35f, "Impact Cooldown", 0.0f, 2.0f)

    void OnStart() override;
    void OnUpdate() override;
    void OnTriggerEnter(const CollisionInfo& info) override;
    void OnTriggerStay(const CollisionInfo& info) override;

private:
    [[nodiscard]] GameObject* ResolveOwner();
    [[nodiscard]] bool CanReactWith(const WeaponHitboxComponent& other) const;
    [[nodiscard]] bool IsHitboxActive() const;
    [[nodiscard]] bool IsOwnerSwinging(GameObject* owner) const;
    [[nodiscard]] bool IsOwnerBlocking(GameObject* owner) const;
    [[nodiscard]] GameObject* ResolveHitOwner(GameObject* hitObject) const;
    void UpdateColliderActive();
    void HandleContact(const CollisionInfo& info);
    void PlayReaction(GameObject* owner, bool forceImpact);
    void PlaySwordBloodSpray();

    GameObject* m_owner = nullptr;
    float m_cooldownRemaining = 0.0f;
};

} // namespace sandbox

#include "WeaponHitboxComponent.generated.hpp"

#ifndef WeaponHitboxComponent_IMPL
#define WeaponHitboxComponent_IMPL

namespace sandbox {

void WeaponHitboxComponent::OnStart()
{
    m_owner = ResolveOwner();

    // WHY: Scene 側の BoxCollider を Inspector で調整できるようにしつつ、実行時には必ず Trigger として扱う。
    if (auto* box = scene.GetComponent<BoxColliderComponent>()) {
        box->SetTrigger(true);
        box->SetCenter(colliderCenter);
        box->SetSize(colliderSize);
        box->SetEnabled(false);
    }
}

void WeaponHitboxComponent::OnUpdate()
{
    if (m_cooldownRemaining > 0.0f)
        m_cooldownRemaining = std::max(0.0f, m_cooldownRemaining - Time::deltaTime);

    if (!m_owner || !m_owner->IsValid())
        m_owner = ResolveOwner();

    UpdateColliderActive();
}

void WeaponHitboxComponent::OnTriggerEnter(const CollisionInfo& info)
{
    HandleContact(info);
}

void WeaponHitboxComponent::OnTriggerStay(const CollisionInfo& info)
{
    HandleContact(info);
}

GameObject* WeaponHitboxComponent::ResolveOwner()
{
    if (!ownerName.empty()) {
        if (auto* namedOwner = scene.Find(ownerName))
            return namedOwner;
    }

    // WHAT: Hitbox は手・盾ボーンの子として配置するため、親階層を上がって Player / Enemy を探す。
    GameObject* current = m_gameObject;
    while (current) {
        if (current->CompareTag("Player") || current->CompareTag("Enemy") ||
            current->name == "Player" || current->name == "Enemy") {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

bool WeaponHitboxComponent::CanReactWith(const WeaponHitboxComponent& other) const
{
    if (m_cooldownRemaining > 0.0f) return false;
    if (!m_owner || !other.m_owner || m_owner == other.m_owner) return false;

    const bool selfIsSword = hitboxType == "Sword";
    const bool otherIsSword = other.hitboxType == "Sword";
    const bool selfIsShield = hitboxType == "Shield";
    const bool otherIsShield = other.hitboxType == "Shield";
    return (selfIsSword || selfIsShield) && (otherIsSword || otherIsShield) &&
           (selfIsSword || otherIsSword);
}

bool WeaponHitboxComponent::IsHitboxActive() const
{
    if (hitboxType == "Sword")
        return IsOwnerSwinging(m_owner);
    if (hitboxType == "Shield")
        return IsOwnerBlocking(m_owner);
    return false;
}

bool WeaponHitboxComponent::IsOwnerSwinging(GameObject* owner) const
{
    if (!owner) return false;

    const bool isAttacking =
        animator.IsInState(owner, "Slash_01") ||
        animator.IsInState(owner, "Slash_02") ||
        animator.IsInState(owner, "Slash_03") ||
        animator.IsInState(owner, "CrouchSlash");
    if (!isAttacking) return false;

    const float t = animator.GetNormalizedTime(owner);
    return t >= swingStartTime && t <= swingEndTime;
}

void WeaponHitboxComponent::HandleContact(const CollisionInfo& info)
{
    if (!info.other) return;

    auto* otherHitbox = info.other->GetScript<WeaponHitboxComponent>();
    if (!m_owner)
        m_owner = ResolveOwner();
    if (otherHitbox && !otherHitbox->m_owner)
        otherHitbox->m_owner = otherHitbox->ResolveOwner();
    if (!IsHitboxActive()) return;
    if (m_cooldownRemaining > 0.0f) return;

    if (!otherHitbox) {
        if (hitboxType != "Sword") return;
        GameObject* hitOwner = ResolveHitOwner(info.other);
        if (!hitOwner || hitOwner == m_owner) return;

        PlayReaction(hitOwner, /*forceImpact=*/false);
        PlaySwordBloodSpray();
        m_cooldownRemaining = std::max(impactCooldown, 0.0f);
        return;
    }

    if (!otherHitbox->IsHitboxActive() || !CanReactWith(*otherHitbox)) return;

    const bool isWeaponClash =
        hitboxType == "Sword" || hitboxType == "Shield" ||
        otherHitbox->hitboxType == "Sword" || otherHitbox->hitboxType == "Shield";
    PlayReaction(m_owner, isWeaponClash);
    otherHitbox->PlayReaction(otherHitbox->m_owner, isWeaponClash);
    m_cooldownRemaining = std::max(impactCooldown, 0.0f);
    otherHitbox->m_cooldownRemaining = std::max(otherHitbox->impactCooldown, 0.0f);
}

bool WeaponHitboxComponent::IsOwnerBlocking(GameObject* owner) const
{
    if (!owner) return false;

    // WHAT: Block State または Block Bool が有効な間だけ盾で受けたリアクションとして扱う。
    return animator.IsInState(owner, "Block") || animator.GetBool(owner, "Block");
}

GameObject* WeaponHitboxComponent::ResolveHitOwner(GameObject* hitObject) const
{
    GameObject* current = hitObject;
    while (current) {
        if (current->CompareTag("Player") || current->CompareTag("Enemy") ||
            current->name == "Player" || current->name == "Enemy") {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

void WeaponHitboxComponent::UpdateColliderActive()
{
    auto* box = scene.GetComponent<BoxColliderComponent>();
    if (!box) return;

    // WHY: 常時 Trigger にすると接触しているだけで反応するため、攻撃・防御の成立フレームだけ World に参加させる。
    box->SetEnabled(IsHitboxActive());
}

void WeaponHitboxComponent::PlayReaction(GameObject* owner, bool forceImpact)
{
    if (!owner) return;

    const bool isBlocking = IsOwnerBlocking(owner);
    const std::string& trigger = (forceImpact || isBlocking) ? impactTriggerParam : hitTriggerParam;
    if (trigger.empty()) return;

    // WHAT: 武器・盾同士は弾き、身体 Hurtbox への攻撃だけを被弾として扱う。
    animator.SetTrigger(owner, trigger);

}

void WeaponHitboxComponent::PlaySwordBloodSpray()
{
    if (!m_owner) return;

    const std::string trailObjectName = m_owner->name + "_Sword_Trail";
    auto* trailObject = scene.Find(trailObjectName);
    if (!trailObject) return;

    if (auto* swordTrail = trailObject->GetScript<SwordTrailComponent>())
        swordTrail->PlayBloodSpray();
}

} // namespace sandbox
#endif
