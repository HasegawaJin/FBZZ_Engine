/// @file    SerpentGeyserVfxComponent.hpp
/// @brief   FX_SRP_Geyser.vfx の公開パラメーター。床の口から «噴き上がる» 縦の柱
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note PlayGroundDust は使えない。あちらの向きは VfxManager の LookRotation が渡すが、
///       渡す前に y 成分を捨てて正規化するため Vector3::UP が +Z へ退化し «真上» を
///       表せない。噴き上がりは «上» が主役なので別の層として持つ。
/// @note 突き上げと «胴が口を通る» は同じグラフを使う (どちらも «穴から噴き上がった»
///       で規模が違うだけ)。枠 (VfxManager の Pool) はタグで分けてあり干渉しない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentGeyserVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentGeyserVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.5f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)。アルファが煙の濃さ")
    FBZZ_FIELD_COLOR(rimColor, (Vector4{ 1.0f, 0.62f, 0.16f, 1.0f }), "Rim Color")
    FBZZ_TOOLTIP("開口の縁と同じ琥珀。火の粉と芯だけがこの色を持つ ─ "
                 "煙と砂へ乗せると «燃えている穴» になる")

    FBZZ_GROUP("Column")
    FBZZ_FIELD_RANGE(float, riseSpeed, 11.0f, "上昇速度", 0.0f, 30.0f)
    FBZZ_TOOLTIP("噴き上げる速さ [m/s]。高さはここと寿命の積で決まる ─ "
                 "大きさ (sizeEnd) を上げるより、こちらを上げた方が «縦» が出る")
    FBZZ_FIELD_RANGE(float, columnWidth, 1.6f, "幅", 0.2f, 6.0f)
    FBZZ_TOOLTIP("柱の太さ [m]。開口の半径 (2.2) より細くして «穴から» に見せる")
    FBZZ_FIELD_RANGE(float, skirtSpread, 2.2f, "Skirt", 0.0f, 8.0f)
    FBZZ_TOOLTIP("根元で外へ逃げる空気の速さ [m/s]。0 にすると柱が «棒» に見える ─ "
                 "縦の勢いは裾との対比でしか読めない")
    FBZZ_FIELD_RANGE(float, emberPower, 8.0f, "Embers", 0.0f, 30.0f)
    FBZZ_TOOLTIP("縁から散る火の粉の速さ [m/s]。0 で «土だけ» になる")
    FBZZ_FIELD_RANGE(float, rimLight, 7.0f, "Rim Light", 0.0f, 40.0f)
    FBZZ_TOOLTIP("噴き上がった瞬間の光。閃光にしないこと ─ 爆発と読み違える")

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SerpentGeyserVfxComponent)

inline void SerpentGeyserVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float rise = (std::max)(riseSpeed, 0.0f);

    vfxbind::ParticleColor(root, "Column Smoke", dustColor);
    vfxbind::ParticleColor(root, "Skirt",        dustColor);
    /// @note 砂粒は «物» なので不透明のまま (RunDustVfxComponent と同じ理由)。
    vfxbind::ParticleColor(root, "Grit", Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });
    vfxbind::ParticleColor(root, "Embers", rimColor);

    /// @note 縦は «速さ» が作る。層のローカル +Y をそのまま上に取ってあるので、
    ///       ここは 3 軸のうち y だけを持ち上げれば柱になる。
    vfxbind::ParticleEmitVelocity(root, "Column Smoke", { 0.0f, rise, 0.0f });
    vfxbind::ParticleEmitVelocity(root, "Grit",         { 0.0f, rise * 1.45f, 0.0f });
    vfxbind::ParticleEmitVelocity(root, "Embers",
                                  { 0.0f, (std::max)(emberPower, 0.0f), 0.0f });
    /// @note 裾は «全方位» へ。固定の向きを与えると、噴き上がりの根元が 1 方向だけ濃くなり、
    ///       «横から風が吹いた» に見える。放射は粒ごとに向きが違わなければ成立しない。
    vfxbind::ParticleEmitVelocity(root, "Skirt", { 0.0f, rise * 0.10f, 0.0f });
    vfxbind::ParticleRadialVelocity(root, "Skirt", (std::max)(skirtSpread, 0.0f) * 3.0f);

    vfxbind::ParticleSizeEnd(root, "Column Smoke", (std::max)(columnWidth, 0.05f) * 2.0f);
    vfxbind::ParticleSizeEnd(root, "Skirt",        (std::max)(skirtSpread, 0.05f) * 1.1f);
    vfxbind::ParticleSphereRadius(root, "Column Smoke", (std::max)(columnWidth, 0.05f) * 0.4f);

    vfxbind::LightIntensity(root, "Rim Light", rimLight);
    vfxbind::LightColor(root, "Rim Light", Vector3{ rimColor.x, rimColor.y, rimColor.z });
}

} // namespace sandbox
