/// @file    ParryVfxComponent.hpp
/// @brief   FX_PLR_Parry.vfx の公開パラメーター。弾いた瞬間の «パキッ»
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// @note 衝突の爆発 (FX_IMP_Explosion) は流用しない。爆発は煙と焦げが残る «壊れた» の
///       絵だが、弾きは «触れて離れた» だけで残るものが無いのが正しい。閃光・十字の
///       光条・破片の火花・輪の 4 層のみで全部 0.4 秒で消える。
/// @note 色は白〜氷色に固定する (極性色は乗せない)。赤青は «誰が帯電しているか» の色
///       (企画書 12.2) で、追従させると弾いた瞬間に «極が乗った» と読み違える。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ParryVfxComponent : public Script {
    FBZZ_SCRIPT(ParryVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(tint, (Vector4{ 0.82f, 0.94f, 1.0f, 1.0f }), "Tint")
    FBZZ_TOOLTIP("閃光・光条・輪の色。刀の赤青は使わない (弾きは «どちらの刀か» の出来事ではない)")
    FBZZ_FIELD_RANGE(float, sparkPower, 15.0f, "火花の勢い", 2.0f, 40.0f)
    FBZZ_TOOLTIP("破片の飛ぶ速さ。弾いた攻撃が重いほど強く")
    FBZZ_FIELD_RANGE(float, flashLight, 18.0f, "閃光のライト", 0.0f, 60.0f)
    FBZZ_TOOLTIP("閃光の点光源。0.1 秒で消えるので明るくても白飛びは 1 コマだけ")
    FBZZ_FIELD_RANGE(float, ringSize, 3.2f, "Ring Size", 0.5f, 10.0f)
    FBZZ_TOOLTIP("輪が広がりきる直径 [m]。重い攻撃を弾いたときだけ大きく")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(ParryVfxComponent)

inline void ParryVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Flash", Vector4{ tint.x, tint.y, tint.z, 0.8f });
    vfxbind::ParticleColor(root, "Cross",     tint);
    vfxbind::ParticleColor(root, "Ring", Vector4{ tint.x, tint.y, tint.z, 0.55f });
    vfxbind::ParticleColor(root, "Glint Dots", tint);

    vfxbind::ParticleEmitVelocity(root, "Shards", Vector3{ 0.0f, sparkPower * 0.12f, sparkPower * 0.6f });
    vfxbind::ParticleVelocitySpread(root, "Shards", sparkPower * 0.45f);
    vfxbind::ParticleSizeEnd(root, "Ring", ringSize);
    vfxbind::ParticleSizeEnd(root, "Ring Late", ringSize * 1.25f);

    const Vector3 lightColor{ tint.x, tint.y, tint.z };
    vfxbind::Light(root, "Flash Light", &lightColor, &flashLight);
}

} // namespace sandbox
