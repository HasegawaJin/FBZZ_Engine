/// @file    NeutralizeVfxComponent.hpp
/// @brief   FX_POL_Neutralize.vfx の公開パラメーター (企画書 12.4)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 極性色を持たないか: 中和は «帯電が消えた» 出来事なので、無彩色が正しい。
///     色を持たせられるようにすると、赤青の意味が «いま何極か» から離れる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class NeutralizeVfxComponent : public Script {
    FBZZ_SCRIPT(NeutralizeVfxComponent)

public:
    FBZZ_GROUP("Fit To Body")
    FBZZ_FIELD(Vector3, symbolOffset, (Vector3{ 0.0f, 1.15f, 0.0f }), "Symbol Offset")
    FBZZ_TOOLTIP("× 記号を置く高さ")
    FBZZ_FIELD_RANGE(float, collapseRadius, 1.1f, "Collapse Radius", 0.2f, 6.0f)
    FBZZ_TOOLTIP("吸い込みの開始半径。体の外から始めないと «畳んだ» 動きが体に隠れる")

    void Apply();
};

FBZZ_REFLECT(NeutralizeVfxComponent)

inline void NeutralizeVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::NodePosition(root, "Cross Pop", symbolOffset);
    vfxbind::ParticleSphereRadius(root, "Collapse Motes", collapseRadius);
}

} // namespace sandbox
