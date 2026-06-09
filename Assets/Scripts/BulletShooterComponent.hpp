// FBZZ Engine
// BulletShooterComponent.hpp | sandbox
// プレイヤーが弾を撃つスクリプト。
// bulletPrefab をマズル位置・カメラ前方向で Instantiate し、BulletComponent に処理を委譲する。
#pragma once

#include "BulletComponent.hpp"
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;

namespace sandbox {

class BulletShooterComponent : public Script {
public:
    static constexpr const char* TYPE_NAME = "BulletShooterComponent";
    const char* GetTypeName() const override { return TYPE_NAME; }

    PrefabRef bulletPrefab;
    EntityID  muzzleEntityId;
    // 設定するとそのカメラの向きで弾が飛ぶ。未設定時はメインカメラを使う。
    EntityID  cameraEntityId;
    // マウス左クリックで撃つ場合は useMouse = true にし、fireKey は無視される。
    bool      useMouse    = true;
    int       mouseButton = 0;            // 0=左, 1=右, 2=中
    int       fireKey     = (int)KeyCode::J;
    float     fireRate    = 5.0f;

    void Reflect(IReflector& r) override
    {
        r.Field("Bullet Prefab",  bulletPrefab);
        r.Field("Muzzle Entity",  muzzleEntityId);
        r.Field("Camera Entity",  cameraEntityId);
        r.Field("Use Mouse",      useMouse);
        r.Field("Mouse Button",   mouseButton);
        r.Field("Fire Key",       fireKey);
        r.FloatRange("Fire Rate", fireRate, 0.1f, 30.0f);
    }

    void OnUpdate(float dt) override
    {
        if (m_cooldown > 0.0f) m_cooldown -= dt;
        const bool pressed = useMouse
            ? Input::MouseButtonDown(mouseButton)
            : input.GetKeyDown((KeyCode)fireKey);
        if (!pressed) return;
        if (m_cooldown > 0.0f) return;
        Fire();
        m_cooldown = (fireRate > 0.0f) ? (1.0f / fireRate) : 0.0f;
    }

private:
    float m_cooldown = 0.0f;

    void Fire() const
    {
        if (bulletPrefab.path.empty()) return;

        Vector3    spawnPos = transform->position;
        spawnPos.y += 2.5f;
        Vector3    fireDir  = Vector3::FORWARD;

        // 向き: cameraEntityId が設定されていればそれを優先し、未設定ならメインカメラを使う
        // WHY: プレイヤーは modelYawOffsetDegrees=180° で回転するため transform.Forward() が
        //      後ろ向きになる。カメラの Forward() は常にプレイヤー（フォーカス）方向を向く。
        {
            auto* cam = cameraEntityId.IsValid()
                ? scene.GetGameObject(cameraEntityId)
                : scene.GetMainCameraObject();
            if (cam) {
                fireDir  = cam->transform.Forward();
            }
        }

        // 発射位置: muzzleEntityId が設定されていればマズル位置で上書き
        if (muzzleEntityId.IsValid()) {
            auto* muzzle = scene.GetGameObject(muzzleEntityId);
            if (muzzle) spawnPos = muzzle->transform.position;
        }

        auto* go = scene.Instantiate(bulletPrefab);
        if (!go) return;

        go->transform.localPosition = spawnPos;

        // ワールド回転の遅延更新を回避するため、方向を直接 BulletComponent に渡す
        if (auto* bullet = scene.GetScript<BulletComponent>(*go))
            bullet->direction = fireDir.Normalized();
    }
};

} // namespace sandbox
