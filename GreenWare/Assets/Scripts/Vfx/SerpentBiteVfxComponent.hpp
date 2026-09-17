/// @file    SerpentBiteVfxComponent.hpp
/// @brief   FX_SRP_Bite.vfx の公開パラメーター。噛みつきの «溜め» と «外した着弾»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note 1 つのグラフで溜めと着弾の両方を持つ。噛みつきは «溜まって → 突っ込む» の
///       1 続きで、2 本に割ると «溜めだけ» / «着弾だけ» の組み合わせが作れてしまう。
///       鳴らす側は Charge/Impact のどちらの姿かを bite=true/false で選ぶ。
/// @note 頭が地上に居る唯一の時間にだけ絵を足す (boss-serpent.md「撃破までの形」)。
///       終盤まで頭は床下に居るため、ここに何も無いと «近づいてよい 1 秒» が伝わらない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Vfx/VfxBinding.hpp>

#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentBiteVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentBiteVfxComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(rimColor, (Vector4{ 1.0f, 0.62f, 0.16f, 1.0f }), "Rim Color")
    FBZZ_TOOLTIP("開口の縁・柱・槍と同じ琥珀。極の赤青を使うと «帯電した頭» と読まれる")
    FBZZ_FIELD_COLOR(dustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.5f }), "Dust Color")

    FBZZ_GROUP("チャージ")
    FBZZ_FIELD_RANGE(float, throatSize, 1.1f, "Throat Size", 0.1f, 4.0f)
    FBZZ_TOOLTIP("喉の芯の大きさ [m]。頭の当たり (半径 0.72) より少し大きくして"
                 "«開いた口の中で光っている» に見せる")
    FBZZ_FIELD_RANGE(float, throatLight, 14.0f, "Throat Light", 0.0f, 60.0f)
    FBZZ_TOOLTIP("溜めの光。ブルームのしきい値 (4.0) を越える値にすると、"
                 "視界の端でも «来る» が読める")
    FBZZ_FIELD_RANGE(float, intakeSpeed, 5.0f, "Intake", 0.0f, 20.0f)
    FBZZ_TOOLTIP("喉へ吸い込まれる火の粉の速さ [m/s]。«溜めている» を «外から内» で言う")

    FBZZ_GROUP("着弾")
    FBZZ_FIELD_RANGE(float, impactSize, 2.2f, "Impact Size", 0.2f, 8.0f)
    FBZZ_TOOLTIP("外して床へ突っ込んだときの土煙の大きさ [m]")
    FBZZ_FIELD_RANGE(float, impactKick, 7.0f, "Impact Kick", 0.0f, 20.0f)
    FBZZ_TOOLTIP("突っ込んだ先へ押し出される速さ [m/s]。層のローカル +Z が噛んだ向き")
    FBZZ_FIELD_RANGE(float, sparkPower, 10.0f, "火花", 0.0f, 40.0f)

    /// 溜めの姿にするか、外した着弾の姿にするか。Apply の前に決める。
    FBZZ_FIELD(bool, impactPose, false, "Impact Pose")
    FBZZ_TOOLTIP("false = 喉が溜まる / true = 床へ突っ込んだ。要らない側の層を畳む")

    /// フィールドの現在値を配下の層へ書き込む。鳴らす側が値を入れた直後に呼ぶ。
    void Apply();
};

FBZZ_REFLECT(SerpentBiteVfxComponent)

inline void SerpentBiteVfxComponent::Apply()
{
    GameObject* self = scene.Self();
    if (self == nullptr) return;
    GameObject& root = *self;

    /// @note 要らない側は «畳む»。層を残したまま量を 0 にすると、枠を借りたときに
    ///       前回の姿の粒が 1 フレームだけ出る。
    const auto pose = [&](const char* node, bool live) {
        if (GameObject* target = vfxbind::Find(root, node))
            if (target->activeSelf() != live) target->SetActive(live);
    };
    pose("Throat Core",   !impactPose);
    pose("Throat Intake", !impactPose);
    pose("Throat Light",  !impactPose);
    pose("Impact Dust",   impactPose);
    pose("Impact Grit",   impactPose);
    pose("Impact Sparks", impactPose);

    vfxbind::ParticleColor(root, "Throat Core",   rimColor);
    vfxbind::ParticleColor(root, "Throat Intake", rimColor);
    vfxbind::ParticleColor(root, "Impact Sparks", rimColor);
    vfxbind::ParticleColor(root, "Impact Dust",   dustColor);
    vfxbind::ParticleColor(root, "Impact Grit",
                           Vector4{ dustColor.x, dustColor.y, dustColor.z, 1.0f });

    vfxbind::ParticleSizeEnd(root, "Throat Core", (std::max)(throatSize, 0.05f));
    vfxbind::ParticleSphereRadius(root, "Throat Intake", (std::max)(throatSize, 0.05f) * 2.4f);
    /// @note 吸い込みは «外から内» なので放射が負。固定の向きだと 1 方向からしか吸わず、
    ///       «溜めている» ではなく «横から流れてきた» に見える。
    vfxbind::ParticleRadialVelocity(root, "Throat Intake", -(std::max)(intakeSpeed, 0.0f) * 2.5f);

    const float kick = (std::max)(impactKick, 0.0f);
    vfxbind::ParticleSizeEnd(root, "Impact Dust", (std::max)(impactSize, 0.1f));
    vfxbind::ParticleEmitVelocity(root, "Impact Dust", { 0.0f, kick * 0.45f, kick });
    vfxbind::ParticleEmitVelocity(root, "Impact Grit", { 0.0f, kick * 0.9f, kick * 1.1f });
    vfxbind::ParticleEmitVelocity(root, "Impact Sparks",
                                  { 0.0f, (std::max)(sparkPower, 0.0f) * 0.55f,
                                    (std::max)(sparkPower, 0.0f) });

    vfxbind::LightIntensity(root, "Throat Light", impactPose ? 0.0f : throatLight);
    vfxbind::LightColor(root, "Throat Light", Vector3{ rimColor.x, rimColor.y, rimColor.z });
}

} // namespace sandbox
