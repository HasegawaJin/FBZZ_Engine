/// @file    SerpentRushVfxComponent.hpp
/// @brief   FX_SRP_Rush.vfx の公開パラメーター。15 m/s で走る胴が床を «削った» 跡
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note 走行の土煙 (FX_PLR_RunDust) や重量の土煙 (FX_BOSS_ShockDust) は流用しない。
///       どちらも «1 点で起きた» 絵でその場に留まるが、ここは «後ろへ流れて薄れる»
///       を 1 発の中で作り、0.12 秒ごとに置いた点が線として読めるようにする。
/// @note 火花を混ぜる。煙だけでは «速い» が出ず、装甲が床を削る火花が遅い胴 (斬れる)
///       と速い胴 (8 m/s 境、触れると痛い) を分ける唯一の手掛かりになる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentRushVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentRushVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.42f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)")
    FBZZ_FIELD_COLOR(sparkColor, (Vector4{ 1.0f, 0.66f, 0.22f, 1.0f }), "Spark Color")
    FBZZ_TOOLTIP("削れた装甲の火花。開口の縁と同じ琥珀")

    FBZZ_GROUP("航跡")
    FBZZ_FIELD_RANGE(float, wakeSpeed, 7.0f, "航跡", 0.0f, 24.0f)
    FBZZ_TOOLTIP("後ろへ流す速さ [m/s]。走っている速さの半分ほどにすると、"
                 "置いた点が «その場に残って薄れる» ではなく «流れて消える» になる")
    FBZZ_FIELD_RANGE(float, puffSize, 1.5f, "煙の大きさ", 0.1f, 6.0f)
    FBZZ_TOOLTIP("煙が最後に広がる大きさ [m]。0.12 秒ごとに置くので、"
                 "大きくすると隣の発と重なって «壁» になる")
    FBZZ_FIELD_RANGE(float, sparkPower, 14.0f, "火花", 0.0f, 40.0f)
    FBZZ_TOOLTIP("削れた火花の速さ [m/s]。«速い胴» を絵で言う担当")
    FBZZ_FIELD_RANGE(float, scrapeWidth, 1.4f, "Scrape Width", 0.1f, 6.0f)
    FBZZ_TOOLTIP("床に残る削り跡の幅 [m]")

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SerpentRushVfxComponent)

inline void SerpentRushVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    const float wake  = (std::max)(wakeSpeed, 0.0f);
    const float spark = (std::max)(sparkPower, 0.0f);

    vfxbind::ParticleColor(root, "Wake Smoke", dustColor);
    vfxbind::ParticleColor(root, "Scrape Grit",
                           Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });
    vfxbind::ParticleColor(root, "Sparks", sparkColor);

    /// @note 層は «ローカル +Z が走っている向き» で置いてある。後ろへ流すので符号は負。
    vfxbind::ParticleEmitVelocity(root, "Wake Smoke",  { 0.0f, wake * 0.30f, -wake });
    vfxbind::ParticleEmitVelocity(root, "Scrape Grit", { 0.0f, wake * 0.55f, -wake * 1.2f });
    /// @note 火花だけは «削った点から前方へも» 飛ぶ。全部後ろへ流すと、削っている場所が
    ///       胴の «後ろ» にあるように見える。
    vfxbind::ParticleEmitVelocity(root, "Sparks", { 0.0f, spark * 0.45f, -spark * 0.35f });

    vfxbind::ParticleSizeEnd(root, "Wake Smoke", (std::max)(puffSize, 0.05f));

    if (GameObject* scrape = vfxbind::Find(root, "Scrape Mark")) {
        const Vector3 current = scrape->transform.scale;
        const float   w       = (std::max)(scrapeWidth, 0.1f);
        scrape->transform.scale = Vector3{ w, current.y, w * 2.4f };
    }
}

} // namespace sandbox
