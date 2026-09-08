/// @file    SlashHitVfxComponent.hpp
/// @brief   FX_BLD_SlashHit.vfx の公開パラメーター。刀が当たった «パキッ»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 弾き (FX_PLR_Parry) の流用をやめたか:
///   輪と閃光は «弾いた» の印で、当たりのたびに輪が出ると 1 秒に何度も弾いているように
///   読める。斬撃の当たりは «刃が通った弧» と火花だけで言い、輪は持たない。
///
/// WHY 弧と光には刀の色を乗せるか (弾きは白なのに):
///   斬撃は «どちらの刀が入ったか» が読める唯一の瞬間。弧と Hit Light をその色にすれば、
///   連撃のどこで手を替えたかが跡に残る。火花だけは熱色のまま ─ 火花まで赤青にすると
///   画面のほとんどが刀の色で埋まって、肝心の弧が沈む。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SlashHitVfxComponent : public Script {
    FBZZ_SCRIPT(SlashHitVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(tint, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Tint")
    FBZZ_TOOLTIP("弧・閃光・光の色。振った刀の色 (BladeColor) を渡す。刀に紐付かない当たりは白")
    FBZZ_FIELD_RANGE(float, sparkPower, 10.0f, "火花の勢い", 2.0f, 40.0f)
    FBZZ_TOOLTIP("火花の飛ぶ速さ。段が進むほど強く、締めで最大")
    FBZZ_FIELD_RANGE(float, flashLight, 6.0f, "閃光のライト", 0.0f, 40.0f)
    FBZZ_TOOLTIP("当たった所を照らす点光源。0.1 秒で消える")
    FBZZ_FIELD_RANGE(float, arcSize, 1.4f, "Arc Size", 0.3f, 5.0f)
    FBZZ_TOOLTIP("刃が通った弧の最終的な大きさ [m]。3 段目 (締め) だけ太く出す")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SlashHitVfxComponent)

inline void SlashHitVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Slash Arc", tint);
    vfxbind::ParticleColor(root, "Hit Flash", tint);
    vfxbind::ParticleSizeEnd(root, "Slash Arc", arcSize);
    vfxbind::ParticleSizeStart(root, "Slash Arc", arcSize * 0.5f);
    vfxbind::ParticleVelocitySpread(root, "Sparks", sparkPower);

    const Vector3 lightColor{ tint.x, tint.y, tint.z };
    vfxbind::Light(root, "Hit Light", &lightColor, &flashLight);
}

} // namespace sandbox
