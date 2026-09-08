/// @file    ParryVfxComponent.hpp
/// @brief   FX_PLR_Parry.vfx の公開パラメーター。弾いた瞬間の «パキッ»
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// WHY 衝突の爆発 (FX_IMP_Explosion) を流用しないか:
///   爆発は «物がぶつかって壊れた» の絵で、煙と焦げが残る。弾きは «硬い物どうしが
///   一瞬だけ触れて離れた» ことで、残るものが何も無いのが正しい。煙が 1 つでも
///   立つと «当たった» と読み違える。ここにあるのは閃光・十字の光条・破片の火花・
///   輪の 4 層だけで、全部 0.4 秒で消える。
///
/// WHY 色を白〜氷色に固定するか (極性色を乗せないか):
///   赤青は «誰が帯電しているか» の色 (12.2)。弾きの色が刀の極に追従すると、
///   弾いた瞬間に «極が乗った» と読める。白は極性の表に無い色なので、
///   «弾いた» だけを言える。
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

    vfxbind::ParticleColor(root, "Flash",     tint);
    vfxbind::ParticleColor(root, "Cross",     tint);
    vfxbind::ParticleColor(root, "Ring",      tint);
    vfxbind::ParticleColor(root, "Glint Dots", tint);

    vfxbind::ParticleVelocitySpread(root, "Shards", sparkPower);
    vfxbind::ParticleSizeEnd(root, "Ring", ringSize);

    const Vector3 lightColor{ tint.x, tint.y, tint.z };
    vfxbind::Light(root, "Flash Light", &lightColor, &flashLight);
}

} // namespace sandbox
