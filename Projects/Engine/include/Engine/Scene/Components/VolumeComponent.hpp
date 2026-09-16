/// @file    VolumeComponent.hpp
/// @brief   トリガー領域に付与する物理効果コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// 重力・渦・爆風などの VolumeType と効果パラメーターを保持する。
/// 具体的な力の適用は physics / system 側で処理する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/ColliderVolume.hpp>

namespace fbzz::scene {

struct VolumeComponent {
    physics::VolumeHandle volumeHandle;
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
        // VolumeType を int 経由で反映する（LightComponent と同じパターン）
        int typeValue = static_cast<int>(type);
        r.Field("type", typeValue);
        if (typeValue < 0) typeValue = 0;
        if (typeValue > 5) typeValue = 5; // Gravity=0 Vortex=1 Buoyancy=2 Explosion=3 TimeDilation=4 Magnetic=5
        type = static_cast<physics::VolumeType>(typeValue);

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
        r.Field("elapsed", elapsed);
    }
};

} // namespace fbzz::scene
