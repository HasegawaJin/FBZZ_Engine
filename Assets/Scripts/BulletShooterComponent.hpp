// FBZZ Engine
// BulletShooterComponent.hpp | sandbox
#pragma once
#include "BulletComponent.hpp"
#include <Engine/Scene/Script.hpp>
#include "BulletShooterComponent.generated.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;

namespace sandbox {

class BulletShooterComponent : public Script {
    FBZZ_SCRIPT(BulletShooterComponent)

public:
    FBZZ_GROUP("Prefab")
    FBZZ_FIELD(PrefabRef, bulletPrefab, {}, "Bullet Prefab")
    FBZZ_FIELD(EntityRef, muzzle,       {}, "Muzzle")
    FBZZ_FIELD(EntityRef, cameraRef,    {}, "Camera Override")

    FBZZ_GROUP("Firing")
    FBZZ_FIELD(KeyCode, fireKey,  KeyCode::MouseLeft, "Fire Key")
    FBZZ_FIELD_RANGE(float, fireRate, 5.0f, "Fire Rate", 0.1f, 30.0f)

    void OnUpdate(float dt) override;

private:
    float m_cooldown = 0.0f;
    void Fire() const;
};

} // namespace sandbox

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef BULLET_SHOOTER_IMPL
#define BULLET_SHOOTER_IMPL

namespace sandbox {

inline void BulletShooterComponent::OnUpdate(float dt)
{
    if (m_cooldown > 0.0f) m_cooldown -= dt;
    if (!input.GetKeyDown(fireKey)) return;
    if (m_cooldown > 0.0f) return;
    Fire();
    m_cooldown = (fireRate > 0.0f) ? (1.0f / fireRate) : 0.0f;
}

inline void BulletShooterComponent::Fire() const
{
    if (bulletPrefab.path.empty()) return;

    Vector3 spawnPos = transform.worldPosition;
    spawnPos.y += 2.5f;
    Vector3 fireDir = Vector3::FORWARD;

    // 向き: cameraRef 優先、未設定ならメインカメラ
    {
        GameObject* cam = nullptr;
        if (auto* go = cameraRef.Resolve(scene)) cam = go;
        else cam = scene.GetMainCameraObject();
        if (cam) fireDir = cam->transform.Forward();
    }

    // マズル位置
    if (auto* muzzleGO = muzzle.Resolve(scene))
        spawnPos = muzzleGO->transform.worldPosition;

    auto* go = scene.Instantiate(bulletPrefab);
    if (!go) return;
    go->transform.position = spawnPos;
    if (auto* bullet = scene.GetScript<BulletComponent>(*go))
        bullet->direction = fireDir.Normalized();
}

} // namespace sandbox
#endif
