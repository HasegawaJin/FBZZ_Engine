/// @file    BeamScorchVfxComponent.hpp
/// @brief   FX_BEAM_Scorch.vfx の公開パラメーター (企画書 6.2)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 極性色が 1 層にしか乗らないか:
///   地形には極性が乗らない。焼け跡は «何も起きなかった» ことの表示なので、
///   帯電を示す色が付いてよいのは電弧 (Sear Arc) だけ。火花と煙は熱の色のまま残す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class BeamScorchVfxComponent : public Script {
    FBZZ_SCRIPT(BeamScorchVfxComponent)

public:
    FBZZ_GROUP("Polarity")
    FBZZ_FIELD_COLOR(polarityColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Polarity Color")
    FBZZ_TOOLTIP("電弧の層だけに乗る。火花と煙は熱の色のまま")

    FBZZ_GROUP("Sear")
    FBZZ_FIELD_RANGE(float, emberRate, 26.0f, "Ember Rate", 0.0f, 60.0f)
    FBZZ_TOOLTIP("焼けた点 1 つから出る火の粉の量 [個/秒]。なぞりは量で見せる")
    FBZZ_FIELD_RANGE(float, searSize, 0.35f, "Sear Size", 0.05f, 2.0f)
    FBZZ_TOOLTIP("光る範囲。焦げの大きさとずれると «焦げの外側が光っている» に見える")

    void Apply();
};

FBZZ_REFLECT(BeamScorchVfxComponent)

inline void BeamScorchVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Sear Arc", polarityColor);
    vfxbind::ParticleEmitRate(root, "Ember Spray", emberRate);
    vfxbind::ParticleSizeStart(root, "Sear Glow", searSize);
}

} // namespace sandbox
