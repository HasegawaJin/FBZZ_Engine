/// @file    SlashHitVfxComponent.hpp
/// @brief   FX_BLD_SlashHit.vfx の公開パラメーター。刀が当たった «パキッ»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note 弾き (FX_PLR_Parry) は流用しない。輪と閃光は «弾いた» の印で、当たりのたびに
///       出ると弾いているように読めるため、斬撃の当たりは光条と火花だけで言う。
/// @note 斬った向きの «線» はここで描かない。パーティクルは発生時の回転が乱数で入り
///       毎回違う角度に傾くため、一閃は SlashCutFxComponent が帯で張る。
/// @note 火花は刃が抜けた向きへ噴かせる。全方位だと «何かが弾けた» にしかならず、
///       通った向きへ噴くことで «斬り抜けた» が火花の流れで読める。
/// @note 光条と閃光には刀の色を乗せる (弾きは白)。斬撃は «どちらの刀か» が読める唯一の
///       瞬間だが、火花は熱色のまま残す (全部刀色だと肝心の一閃が沈む)。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SlashHitVfxComponent : public Script {
    FBZZ_SCRIPT(SlashHitVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(tint, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Tint")
    FBZZ_TOOLTIP("光条・閃光・光の色。振った刀の色 (BladeColor) を渡す。刀に紐付かない当たりは白")
    FBZZ_FIELD_RANGE(float, sparkPower, 10.0f, "火花の勢い", 2.0f, 40.0f)
    FBZZ_TOOLTIP("火花の飛ぶ速さ。段が進むほど強く、締めで最大")
    FBZZ_FIELD_RANGE(float, flashLight, 6.0f, "閃光のライト", 0.0f, 40.0f)
    FBZZ_TOOLTIP("当たった所を照らす点光源。0.1 秒で消える")
    /// キー名は旧 «弧の大きさ» のまま。.vfx と保存済みシーンが arcSize で持っている。
    FBZZ_FIELD_RANGE(float, arcSize, 1.4f, "Impact Size", 0.3f, 5.0f)
    FBZZ_TOOLTIP("光条の大きさ [m]")
    FBZZ_FIELD_RANGE(float, sweepLateral, -1.0f, "Sweep Lateral", -1.0f, 1.0f)
    FBZZ_TOOLTIP("刃が抜けた横の向き (±1)。エフェクトの前方 (刃が抜けていく向き) に対する横")
    FBZZ_FIELD_RANGE(float, sweepTilt, 0.0f, "Sweep Tilt", -1.0f, 1.0f)
    FBZZ_TOOLTIP("刃の傾き。正で斬り上げ、負で斬り下ろし")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SlashHitVfxComponent)

inline void SlashHitVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float size = Max(arcSize, 0.1f);
    vfxbind::ParticleSizeStart(root, "Impact Rays", size * 0.55f);
    vfxbind::ParticleSizeEnd(root, "Impact Rays", size * 0.12f);
    vfxbind::ParticleColor(root, "Impact Rays", tint);
    /// @note 通常斬撃では輪を無効化。締め・溜めのバリアントだけがこの層を持つ。
    vfxbind::ParticleSizeStart(root, "Impact Ring", size * 0.2f);
    vfxbind::ParticleSizeEnd(root, "Impact Ring", size * 1.4f);
    vfxbind::ParticleColor(root, "Impact Ring", Vector4{ tint.x, tint.y, tint.z, 0.3f });
    vfxbind::ParticleColor(root, "Hit Flash", Vector4{ tint.x, tint.y, tint.z, 0.65f });
    vfxbind::ParticleSizeStart(root, "Hit Flash", size * 0.3f);

    /// @note 刃が抜けた向き (横) に、刃の進む向き (前方 +Z) を少し混ぜて噴く。
    ///       emitVelocity は層のローカル軸で、根は PlaySlashHit が «刃が抜けていく向き» へ回してある。
    const Vector3 spray = Vector3{ sweepLateral, sweepTilt, 0.5f }.NormalizedOr(Vector3::FORWARD);
    vfxbind::ParticleEmitVelocity(root, "Sparks", spray * (sparkPower * 0.8f));
    vfxbind::ParticleVelocitySpread(root, "Sparks", sparkPower * 0.25f);

    const Vector3 lightColor{ tint.x, tint.y, tint.z };
    vfxbind::Light(root, "Hit Light", &lightColor, &flashLight);
}

} // namespace sandbox
