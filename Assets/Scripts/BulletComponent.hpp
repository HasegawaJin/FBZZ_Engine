// FBZZ Engine
// BulletComponent.hpp | sandbox
#pragma once
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BulletComponent : public Script {
    FBZZ_SCRIPT(BulletComponent)

public:
    FBZZ_FIELD(float,       speed,          20.0f,    "Speed")
    FBZZ_FIELD(float,       lifetime,       3.0f,     "Lifetime")
    FBZZ_FIELD(float,       effectLifetime, 3.0f,     "Effect Lifetime")
    FBZZ_FIELD(PrefabRef,   effectPrefab,   {},       "Effect Prefab")
    FBZZ_FIELD(std::string, ignoreTag,      "Player", "Ignore Tag")

    Vector3 direction;  // BulletShooterComponent が Instantiate 後に設定する

    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnTriggerEnter(const CollisionInfo& info) override;
    void OnUpdate() override;

private:
    bool    m_alive         = true;
    bool    m_effectSpawned = false;
    float   m_remaining     = 0.0f;
    Vector3 m_hitPoint;

    void OnStart() override { m_remaining = lifetime; }

    bool ShouldIgnore(const GameObject* go) const
    {
        if (!go) return false;
        if (go == scene.Self()) return true;
        if (!ignoreTag.empty() && go->tag == ignoreTag) return true;
        return false;
    }

    void Die(const Vector3& point);
    void SpawnEffect(const Vector3& point);
};

} // namespace sandbox

#include "BulletComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef BULLET_COMPONENT_IMPL
#define BULLET_COMPONENT_IMPL

namespace sandbox {

inline void BulletComponent::OnCollisionEnter(const CollisionInfo& info)
{
    // WHY: PhysicsSystem コールバック内で Instantiate を呼ぶとシーン破壊が起きる。
    //      フラグと座標だけ記録して OnUpdate で実際の処理を行う。
    if (!m_alive || ShouldIgnore(info.other)) return;
    m_alive    = false;
    m_hitPoint = info.contactPoint;
}

inline void BulletComponent::OnTriggerEnter(const CollisionInfo& info)
{
    if (!m_alive || ShouldIgnore(info.other)) return;
    m_alive    = false;
    m_hitPoint = transform ? transform.worldPosition : Vector3{};
}

inline void BulletComponent::OnUpdate()
{
    if (!transform) return;

    if (!m_alive) {
        Die(m_hitPoint);
        return;
    }

    const Vector3 pos = transform.worldPosition;
    const Vector3 dir = (direction.LengthSq() > 0.0001f) ? direction
                                                          : transform.forward;
    const float   dt  = Time::deltaTime;

    RaycastHit hit;
    if (physics.Raycast(pos, dir, speed * dt, hit)) {
        if (!ShouldIgnore(hit.gameObject)) {
            Die(hit.point);
            return;
        }
    }

    const Vector3 nextPos = pos + dir * (speed * dt);
    const float terrainH = scene.GetTerrainHeightAt(nextPos);
    if (nextPos.y <= terrainH) {
        Die({ nextPos.x, terrainH, nextPos.z });
        return;
    }

    transform.position = nextPos;
    m_remaining -= dt;
    if (m_remaining <= 0.0f) Die(nextPos);
}

inline void BulletComponent::Die(const Vector3& point)
{
    m_alive    = false;
    m_hitPoint = point;
    scene.DestroySelf();
    if (!m_effectSpawned) {
        SpawnEffect(m_hitPoint);
        m_effectSpawned = true;
    }
}

inline void BulletComponent::SpawnEffect(const Vector3& point)
{
    if (effectPrefab.path.empty()) return;
    auto* go = scene.Instantiate(effectPrefab);
    if (!go) return;
    go->transform.position = point;
    go->AddComponent<LifetimeComponent>({effectLifetime, true});
}

} // namespace sandbox
#endif
