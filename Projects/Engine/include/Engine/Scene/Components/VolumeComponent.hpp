// FBZZ Engine
// VolumeComponent.hpp | fbzz::scene
// トリガー領域に付与する物理効果コンポーネント
// 重力・渦・爆風などの VolumeType と効果パラメーターを保持する。
// 具体的な力の適用は physics / system 側で処理する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Physics/ColliderVolume.hpp>

namespace fbzz::scene {

struct VolumeComponent {
    physics::VolumeType type = physics::VolumeType::Gravity;
    bool enabled = true;

    math::Vector3 gravity = { 0.0f, -9.81f, 0.0f };
    math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f };

    float swirlStrength = 1.0f;
    float inwardStrength = 0.0f;
    float liftStrength = 0.0f;
    float buoyancy = 10.0f;
    float drag = 1.0f;
    float explosionImpulse = 10.0f;
    float timeScale = 1.0f;
    float duration  = -1.0f; // 負値は無限継続。正値は秒単位の有効期間
    float elapsed   = 0.0f;  // 経過時間。duration >= 0 のときだけ PhysicsSystem が加算する

    const char* GetTypeName() const { return "Volume"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("gravity", gravity);
        r.Field("magneticField", magneticField);
        r.Field("swirlStrength", swirlStrength);
        r.Field("inwardStrength", inwardStrength);
        r.Field("liftStrength", liftStrength);
        r.Field("buoyancy", buoyancy);
        r.Field("drag", drag);
        r.Field("explosionImpulse", explosionImpulse);
        r.Field("timeScale", timeScale);
        r.Field("duration", duration);
    }
};

} // namespace fbzz::scene
