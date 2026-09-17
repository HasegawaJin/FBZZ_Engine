/// @file    ImpactVfxComponent.hpp
/// @brief   FX_IMP_Explosion.vfx の公開パラメーター (企画書 12.6)
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note .vfx 側の «公開パラメーター» ではなくスクリプトの公開フィールドで持つ。
///       型・範囲・ツールチップの宣言がここ 1 箇所で決まり、Inspector と呼び出し側が
///       同じ定義を見る。
/// @note OnStart ではなく Apply() で当てる。この演出はプールから借りて何度も鳴らすため、
///       生成時に 1 度だけ当てる作りだと 2 発目以降が 1 発目の値のまま鳴る。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ImpactVfxComponent : public Script {
    FBZZ_SCRIPT(ImpactVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(tintColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Tint Color")
    FBZZ_TOOLTIP("閃光と爆風の色。煙と火花には乗せない (熱を持って飛ぶ «物» の側)")

    FBZZ_GROUP("Blast")
    FBZZ_FIELD_RANGE(float, sparkPower, 11.0f, "火花の勢い", 2.0f, 26.0f)
    FBZZ_TOOLTIP("火花の散り方。衝突速度をそのまま流す")
    FBZZ_FIELD_RANGE(float, smokeAmount, 4.2f, "Smoke Amount", 0.0f, 8.0f)
    FBZZ_TOOLTIP("煙の最終的な大きさ。盤面が煙で埋まるときはここを下げる")
    FBZZ_FIELD_RANGE(float, blastLight, 22.0f, "Blast Light", 0.0f, 60.0f)
    FBZZ_TOOLTIP("閃光の強さ。同じフレームに何発も重なるときは後続を弱めて画面を守る")

    FBZZ_GROUP("接地")
    FBZZ_FIELD_FILE(groundMark, "Assets/VFX/Textures/T_Scorch_Decal.png", "Ground Mark", ".png")
    FBZZ_TOOLTIP("床の跡。柱・壁・床を割ったときだけ «ひび» へ差し替える")

    /// フィールドの現在値を配下の層へ書き込む。撃つ側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(ImpactVfxComponent)

inline void ImpactVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    /// @note 色が乗るのは «光っている» 層だけ。火花 (Sparks) と煙 (Debris Smoke) は
    ///       熱を持って飛ぶ «物» なので無彩色のまま残す。
    vfxbind::ParticleColor(root, "Flash Core", tintColor);
    vfxbind::ParticleColor(root, "Splash",     tintColor);
    vfxbind::ParticleColor(root, "Blast Core", tintColor);
    vfxbind::ParticleColor(root, "Blast Body", tintColor);
    vfxbind::ParticleColor(root, "Shock Ring", tintColor);
    vfxbind::ParticleColor(root, "Arc Snap",   tintColor);

    vfxbind::ParticleVelocitySpread(root, "Sparks", sparkPower);
    vfxbind::ParticleSizeEnd(root, "Debris Smoke", smokeAmount);

    const Vector3 lightColor{ tintColor.x, tintColor.y, tintColor.z };
    vfxbind::Light(root, "Blast Light", &lightColor, &blastLight);

    vfxbind::DecalAlbedo(root, "Ground Mark", groundMark);
}

} // namespace sandbox
