/// @file    SerpentSnapVfxComponent.hpp
/// @brief   FX_SRP_Snap.vfx の公開パラメーター。檻が角へ寄って砕けた «外へ抜ける» 絵
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 叩きつけ (FX_SRP_Slam) と分けるか:
///   締め上げは «壁 2 枚が角を軸に寄る» 手なので、危険なのは角から外側 ─ プレイヤーは
///   その角から離れる方向へ逃げることになる。叩きつけの «帯が落ちてくる» とは
///   逃げる向きが違う。同じ絵にしていた間は、予兆のデカールを見ていない限り
///   2 つを区別できなかった。
///
/// WHY 輪を出さないか (VFX/Game/README.md「弾き・当たり・転倒・とどめの読み分け」):
///   輪は «弾いた» の印。締め上げで輪を出すと、プレイヤーが «弾ける手» だと読む。
///   ここは砕けた床と、角から放射に抜ける煙だけで言う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentSnapVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentSnapVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.6f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)")
    FBZZ_FIELD_COLOR(rimColor, (Vector4{ 1.0f, 0.62f, 0.16f, 1.0f }), "Rim Color")
    FBZZ_TOOLTIP("開口の縁と同じ琥珀。砕けた装甲の火花だけがこの色を持つ")

    FBZZ_GROUP("バースト")
    FBZZ_FIELD_RANGE(float, outwardSpeed, 9.0f, "Outward", 0.0f, 24.0f)
    FBZZ_TOOLTIP("角から放射に抜ける速さ [m/s]。«寄ってきた» の向きはここが全部言う")
    FBZZ_FIELD_RANGE(float, cloudSize, 4.0f, "Cloud Size", 0.5f, 12.0f)
    FBZZ_TOOLTIP("煙が最後に広がる大きさ [m]。壁 2 枚の高さ (約 3 m) を包む値にする")
    FBZZ_FIELD_RANGE(float, sparkPower, 12.0f, "火花", 0.0f, 40.0f)
    FBZZ_TOOLTIP("装甲どうしが噛んだ火花の速さ [m/s]。«硬い物が当たった» の音の相方")
    FBZZ_FIELD_RANGE(float, crackSize, 6.0f, "亀裂の大きさ", 1.0f, 20.0f)
    FBZZ_TOOLTIP("角に残る床のひびの直径 [m]")
    FBZZ_FIELD_RANGE(float, snapLight, 9.0f, "Snap Light", 0.0f, 40.0f)

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SerpentSnapVfxComponent)

inline void SerpentSnapVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float out = (std::max)(outwardSpeed, 0.0f);

    vfxbind::ParticleColor(root, "Corner Smoke", dustColor);
    vfxbind::ParticleColor(root, "Ground Wash",  dustColor);
    vfxbind::ParticleColor(root, "Grit", Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });
    vfxbind::ParticleColor(root, "Sparks", rimColor);

    // 放射は «球へ撒いて外へ押す»。固定の向きを与えると 1 方向だけが濃くなり、
    // «どちらから寄ったか» という嘘の向きが出る。
    vfxbind::ParticleEmitVelocity(root, "Corner Smoke", { 0.0f, out * 0.22f, 0.0f });
    vfxbind::ParticleRadialVelocity(root, "Corner Smoke", out * 2.6f);
    vfxbind::ParticleRadialVelocity(root, "Ground Wash",  out * 3.4f);
    vfxbind::ParticleEmitVelocity(root, "Grit", { 0.0f, out * 0.85f, 0.0f });
    vfxbind::ParticleRadialVelocity(root, "Grit", out * 2.0f);
    vfxbind::ParticleEmitVelocity(root, "Sparks",
                                  { 0.0f, (std::max)(sparkPower, 0.0f) * 0.5f, 0.0f });
    vfxbind::ParticleRadialVelocity(root, "Sparks", (std::max)(sparkPower, 0.0f) * 2.2f);

    vfxbind::ParticleSizeEnd(root, "Corner Smoke", (std::max)(cloudSize, 0.1f));
    vfxbind::ParticleSizeEnd(root, "Ground Wash",  (std::max)(cloudSize, 0.1f) * 1.6f);

    if (GameObject* crater = vfxbind::Find(root, "Crack")) {
        const float d = (std::max)(crackSize, 0.5f);
        crater->transform.scale = Vector3{ d, crater->transform.scale.y, d };
    }

    vfxbind::LightIntensity(root, "Snap Light", snapLight);
    vfxbind::LightColor(root, "Snap Light", Vector3{ rimColor.x, rimColor.y, rimColor.z });
}

} // namespace sandbox
