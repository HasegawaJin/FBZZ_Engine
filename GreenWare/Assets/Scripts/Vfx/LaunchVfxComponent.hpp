/// @file    LaunchVfxComponent.hpp
/// @brief   FX_ATR_Launch.vfx の公開パラメーター (企画書 7.3 ②)
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class LaunchVfxComponent : public Script {
    FBZZ_SCRIPT(LaunchVfxComponent)

public:
    FBZZ_GROUP("Polarity")
    FBZZ_FIELD_COLOR(polarityColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Polarity Color")

    FBZZ_GROUP("Wake")
    FBZZ_FIELD_RANGE(float, wakeSpeed, 8.0f, "Wake Speed", 2.0f, 22.0f)
    FBZZ_TOOLTIP("尾を置き去りにする速さ。等倍だと尾が本体を追い越して «前へ飛んだ» に見える")

    void Apply();
};

FBZZ_REFLECT(LaunchVfxComponent)

inline void LaunchVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Launch Wake",   polarityColor);
    vfxbind::ParticleColor(root, "Launch Collar", polarityColor);
    vfxbind::ParticleColor(root, "Snap Arc",      polarityColor);

    const Vector3 lightColor{ polarityColor.x, polarityColor.y, polarityColor.z };
    vfxbind::LightColor(root, "Launch Light", lightColor);

    // ルートの回転が «飛んだ向きの逆» を向いているので、吹き出しはローカル +Z で足りる。
    vfxbind::ParticleEmitVelocity(root, "Launch Wake", Vector3{ 0.0f, 0.0f, wakeSpeed });
}

} // namespace sandbox
