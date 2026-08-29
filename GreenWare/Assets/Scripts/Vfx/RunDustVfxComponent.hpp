/// @file    RunDustVfxComponent.hpp
/// @brief   FX_PLR_RunDust.vfx の公開パラメーター (走っている足元の土煙)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 極性色を持たないか:
///   足元は画面のほぼ中央に出続ける。ここへ赤青を乗せると、盤面の «誰が帯電しているか»
///   が最も見たい場所で潰れる (12.2 / VFX/Game/README.md)。差し替えるのは床の色だけ。
///
/// WHY 蹴り出しの «向き» を持たないか:
///   土煙は足が後ろへ掻いた側へ残るので、向きは 1 発ごとに変わる。ただしそれは枠そのものの
///   姿勢で、層ごとの値ではない。VfxManagerComponent が Prepare へ回転を渡し、
///   ここは «ローカル +Z へどれだけ強く吹くか» だけを持つ。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class RunDustVfxComponent : public Script {
    FBZZ_SCRIPT(RunDustVfxComponent)

public:
    FBZZ_GROUP("Ground")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.4f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。極性色は乗せない (12.2)。アルファが煙の濃さになる")

    FBZZ_GROUP("Kick")
    FBZZ_FIELD_RANGE(float, puffSize, 0.78f, "Puff Size", 0.1f, 3.0f)
    FBZZ_TOOLTIP("煙が最後に広がる大きさ。速いほど大きく")
    FBZZ_FIELD_RANGE(float, kickSpeed, 1.6f, "Kick Speed", 0.0f, 8.0f)
    FBZZ_TOOLTIP("後ろへ蹴り出す速さ [m/s]。走行速度に比例させる")
    FBZZ_FIELD_RANGE(float, gritPower, 0.9f, "Grit Power", 0.0f, 4.0f)
    FBZZ_TOOLTIP("砂粒の散り方。上げるほど粒が扇状に広がる")

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(RunDustVfxComponent)

inline void RunDustVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Ground Wash", dustColor);
    vfxbind::ParticleColor(root, "Dust Puff",   dustColor);
    // 砂粒は «物» なので不透明のまま。煙の濃さ (dustColor.w) を掛けると、
    // 薄い床で粒まで消えて速さの手掛かりが煙だけになる。
    vfxbind::ParticleColor(root, "Grit", Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });

    vfxbind::ParticleSizeEnd(root, "Dust Puff", puffSize);
    // 床に触れた印は煙より一回り広い。同じ大きさにすると煙の足元に隠れて見えなくなる。
    vfxbind::ParticleSizeEnd(root, "Ground Wash", puffSize * 1.4f);

    // 上下と後方の配分はここが正本。層ごとに数値を書くと、蹴り出しを強めたときに
    // 煙と砂粒の «飛び出す角度» が別々にずれる。
    vfxbind::ParticleEmitVelocity(root, "Dust Puff", { 0.0f, kickSpeed * 0.42f, kickSpeed });
    vfxbind::ParticleEmitVelocity(root, "Grit",      { 0.0f, kickSpeed * 0.85f, kickSpeed * 1.8f });
    vfxbind::ParticleVelocitySpread(root, "Grit", gritPower);
}

} // namespace sandbox
