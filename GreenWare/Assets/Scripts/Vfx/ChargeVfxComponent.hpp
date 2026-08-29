/// @file    ChargeVfxComponent.hpp
/// @brief   FX_POL_Charge.vfx の公開パラメーター (企画書 12.1 / 12.3)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// 設計の理由は ImpactVfxComponent.hpp と同じ (Docs/design/vfx-prefab.md §6)。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ChargeVfxComponent : public Script {
    FBZZ_SCRIPT(ChargeVfxComponent)

public:
    FBZZ_GROUP("Polarity")
    FBZZ_FIELD_COLOR(polarityColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Polarity Color")
    FBZZ_TOOLTIP("乗った極の色。赤 = ＋ / 青 = − (12.2)")
    FBZZ_FIELD_FILE(symbolMaterial, "Assets/Materials/Effects/FX_SymPlus_Additive.mat",
                    "Symbol Material", ".mat")
    FBZZ_TOOLTIP("頭上に出す ＋ / − の記号。極ごとに差し替える")

    FBZZ_GROUP("Fit To Body")
    FBZZ_FIELD(Vector3, symbolOffset, (Vector3{ 0.0f, 1.15f, 0.0f }), "Symbol Offset")
    FBZZ_TOOLTIP("記号を置く高さ。体にめり込むと 12.3 の «記号で読める» が成立しない")
    FBZZ_FIELD_RANGE(float, haloSize, 1.9f, "Halo Size", 0.2f, 8.0f)
    FBZZ_TOOLTIP("輪の大きさ。体より一回り大きくしないと «包んだ» に見えない")

    void Apply();
};

FBZZ_REFLECT(ChargeVfxComponent)

inline void ChargeVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Charge Flash", polarityColor);
    vfxbind::ParticleColor(root, "Charge Ring",  polarityColor);
    vfxbind::ParticleColor(root, "Arc Halo",     polarityColor);
    vfxbind::ParticleColor(root, "Charge Motes", polarityColor);
    vfxbind::ParticleColor(root, "Symbol Pop",   polarityColor);

    const Vector3 lightColor{ polarityColor.x, polarityColor.y, polarityColor.z };
    vfxbind::LightColor(root, "Charge Light", lightColor);

    vfxbind::ParticleMaterial(root, "Symbol Pop", symbolMaterial);
    vfxbind::NodePosition(root, "Symbol Pop", symbolOffset);
    vfxbind::ParticleSizeEnd(root, "Charge Ring", haloSize);
}

} // namespace sandbox
