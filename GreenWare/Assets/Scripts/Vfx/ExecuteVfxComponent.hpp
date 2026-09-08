/// @file    ExecuteVfxComponent.hpp
/// @brief   FX_BOSS_Execute.vfx の公開パラメーター。居合で脚 / 節を落とす «溶断»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 切断面の «赤熱 → 冷める» を芯にするか:
///   とどめは 5 秒の転倒の中で 1 回だけ起きる、盤面の形が変わる出来事 (break-parry.md)。
///   火花と破片は衝突の爆発にもあるので、それだけでは «壊れた» と読み分けられない。
///   «斬った面が熱を持って冷める» は刀にしか無い絵で、これが «斬り落とした» の語になる。
///
/// WHY 色は白〜熱色で、極性色を乗せないか:
///   とどめは極の出来事ではない (極性の色合わせは 2026-09-04 に捨てた)。
///   火花・火の粉・切断面は 12.2 の «熱色のまま» の側。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ExecuteVfxComponent : public Script {
    FBZZ_SCRIPT(ExecuteVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(tint, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Tint")
    FBZZ_TOOLTIP("弧と閃光の色。白のまま置く (極の出来事ではない)")
    FBZZ_FIELD_RANGE(float, sparkPower, 18.0f, "火花の勢い", 2.0f, 40.0f)
    FBZZ_TOOLTIP("溶断スパークの飛ぶ速さ")
    FBZZ_FIELD_RANGE(float, cutLight, 30.0f, "切り口のライト", 0.0f, 60.0f)
    FBZZ_TOOLTIP("白熱の点光源。0.35 秒で冷める")
    FBZZ_FIELD_RANGE(float, cutSize, 2.6f, "切り口の大きさ", 0.5f, 8.0f)
    FBZZ_TOOLTIP("弧の最終的な大きさ [m]。脚の太さに合わせる (コアの脚 2.6 / 蛇の節 1.8)")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(ExecuteVfxComponent)

inline void ExecuteVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    vfxbind::ParticleColor(root, "Cut Arc",   tint);
    vfxbind::ParticleColor(root, "Cut Flash", tint);
    vfxbind::ParticleSizeEnd(root,   "Cut Arc", cutSize);
    vfxbind::ParticleSizeStart(root, "Cut Arc", cutSize * 0.55f);
    // 縫い目の長さは弧に追従させる (sizeStart が基準寸法)。
    vfxbind::ParticleSizeStart(root, "Cut Seam", cutSize * 0.38f);
    vfxbind::ParticleSizeEnd(root,   "Cut Seam", cutSize * 0.44f);
    vfxbind::ParticleVelocitySpread(root, "Molten Sparks", sparkPower);

    vfxbind::LightIntensity(root, "Cut Light", cutLight);
}

} // namespace sandbox
