/// @file    SerpentRigComponent.hpp
/// @brief   斬られた節を輪郭で光らせる。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note 発光帯でなく輪郭で出す。装甲は暗く自発光を上げても飽和が先に来て見えにくい上、
///       `M_E_RingGlow` は中立色で灯りっぱなしだと斬っていない節まで反応して見える。
///       輪郭なら視界の端でも «今そこに入った» が読める。光るのは節ごと、消えるまでの
///       秒数は部位 (`BossPartComponent`) が持つ。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <algorithm>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentRigComponent : public Script {
    FBZZ_SCRIPT(SerpentRigComponent)

public:
    FBZZ_GROUP("Outline")
    FBZZ_FIELD(bool, outlineSegments, true, "Outline Segments")
    FBZZ_FIELD_RANGE(float, outlineWidth, 0.85f, "幅", 0.1f, 1.0f)
    /// @note 遮蔽を既定で無視する。胴は自分自身でとぐろを巻き手前の節が奥を隠すのが
    ///       常態なので、遮蔽で捨てるとマスクがほとんど残らない。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "壁を透かす")
    FBZZ_FIELD_COLOR(damageFlashColor, (Vector4{ 1.0f, 0.97f, 0.90f, 1.0f }), "Damage Flash")
    FBZZ_TOOLTIP("斬られた節の輪郭が一瞬寄る色。長さは BossPartComponent の Flash")
    FBZZ_FIELD_RANGE(float, damageFlashStrength, 1.0f, "被弾フラッシュ倍率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で «斬られても光らない»。輪郭をどれだけ太らせるか")

    /// 斬撃の吸い付き先を振る «前» に返す。意味は BossRigComponent 側と同じ ─
    /// «狙っている» と «入った» は別の合図なので、色も太さも分ける。
    FBZZ_FIELD(bool, outlineAimedSegment, true, "Outline Aimed Segment")
    FBZZ_TOOLTIP("斬撃の吸い付き先に選ばれている節を薄く縁取る。切ると振るまで分からなくなる")
    FBZZ_FIELD_COLOR(aimOutlineColor, (Vector4{ 0.55f, 0.80f, 1.0f, 1.0f }), "Aim Outline")
    FBZZ_TOOLTIP("狙っている節の輪郭色。斬られた合図 (Damage Flash) と見分けが付く色にする")
    FBZZ_FIELD_RANGE(float, aimOutlineWidthScale, 0.55f, "狙い線の細さ", 0.1f, 1.0f)
    FBZZ_TOOLTIP("上の 幅 に対する比。斬られた合図より必ず細くする")

    /// «今は斬る番» を胴そのものが言う。檻 (SerpentAiComponent の 檻の中の斬撃倍率) が
    /// 立っている間だけ、地上に出ている節を全部縁取る。
    ///
    /// @note 檻の間の崩し倍率上昇は数字でしか出ず、輪郭が唯一の合図になる。倍率は
    ///       ゲージから直接読む ─ AI へ聞くと SerpentRig → SerpentAi の include が
    ///       環になる上、«斬撃が濃いか» の一次情報を持つのはゲージなので、光る条件と
    ///       得をする条件が原理的にずれない。
    FBZZ_FIELD(bool, outlineWhenSlashPays, true, "斬る番を縁取る")
    FBZZ_FIELD_COLOR(feastOutlineColor, (Vector4{ 1.0f, 0.42f, 0.12f, 1.0f }), "斬る番の色")
    FBZZ_TOOLTIP("檻の間、地上の胴を囲う色。斬られた合図 (白) とも狙い (青) とも"
                 "違う色にする ─ 意味が «そこを斬れ» で 3 つ目だから")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "輪郭を出す")

    void OnStart() override { m_glowMaterials.Reset(); m_armorMaterials.Reset(); }
    void OnUpdate() override;

private:
    BossGlowMaterials m_glowMaterials;
    BossArmorMaterials m_armorMaterials;
    [[nodiscard]] SerpentBodyComponent*      Body()  const { return scene.GetScript<SerpentBodyComponent>(); }
    [[nodiscard]] SerpentSpineComponent*     Spine() const { return scene.GetScript<SerpentSpineComponent>(); }
    [[nodiscard]] SerpentHitboxRigComponent* Rig()   const { return scene.GetScript<SerpentHitboxRigComponent>(); }
};

FBZZ_REFLECT(SerpentRigComponent)

inline void SerpentRigComponent::OnUpdate()
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    if (health && health->IsAlive()) {
        const auto* state = scene.GetScript<BossBreakComponent>();
        const bool down = state && state->IsToppled();
        const float stress = state ? state->Ratio() : 0.0f;
        m_armorMaterials.Apply(scene.Self(), material, stress, down, 0.0f, Time::deltaTime);
        const float pulse = 0.5f + 0.5f * std::sin(Time::time * (down ? 5.0f : 3.0f + stress * 5.0f));
        const Vector4 tint = down ? Vector4{1.0f, 0.8f, 0.25f, 1.0f}
            : Vector4{1.0f, 0.55f + pulse * 0.15f, 0.18f, 1.0f};
        m_glowMaterials.Apply(scene.Self(), material, tint,
            down ? 1.4f + pulse * 0.6f : 0.7f + stress * pulse, Time::time);
    }
    debugOutlined = 0;
    if (!outlineSegments) return;

    auto* rig   = Rig();
    auto* body  = Body();
    auto* spine = Spine();
    if (!rig || !body) return;

    /// @note 斬撃が濃くなっている盤面か (檻)。持っているのはゲージ 1 か所だけ。
    const auto* brk  = scene.GetScript<BossBreakComponent>();
    const bool  pays = outlineWhenSlashPays && brk && brk->SlashScale() > 1.001f;

    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        GameObject* hitbox = rig->SegmentHitbox(i);
        if (!hitbox) continue;
        const auto* part = scene.GetScript<BossPartComponent>(hitbox);
        if (!part) continue;
        const float flash = part->DamageFlash();
        const bool  aimed = outlineAimedSegment && part->AimHighlight() > 0.0f;
        if (flash <= 0.0f && !aimed && !pays) continue;
        if (!body->IsAlive(i)) continue;
        if (spine && !spine->IsExposed(i)) continue;

        /// @note マスクの意味は読む側 (Outline.hlsl) との取り決め: RGB = 色 / A = 太さ。
        ///       斬られた合図が出ているあいだは必ずそちらを採る ─ 入った合図の途中で
        ///       狙いの色へ落ちると、当たったことが薄まる。
        ///       優先順は «入った» > «狙っている» > «斬る番»。後ろの 2 つは «これから» の話で、
        ///       入った合図の途中でそちらへ落ちると、当たったことが薄まる。
        const float   k   = Clamp01(flash) * Clamp01(damageFlashStrength);
        const bool    hit = flash > 0.0f;
        const Vector4 color = hit
            ? Vector4{ damageFlashColor.x, damageFlashColor.y, damageFlashColor.z, 1.0f }
            : aimed ? Vector4{ aimOutlineColor.x, aimOutlineColor.y, aimOutlineColor.z, 1.0f }
                    : Vector4{ feastOutlineColor.x, feastOutlineColor.y,
                               feastOutlineColor.z, 1.0f };
        const float   width = hit
            ? Clamp01(Lerp(Clamp01(outlineWidth), 1.0f, k))
            : Clamp01(Clamp01(outlineWidth) * Clamp01(aimOutlineWidthScale));

        for (const EntityRef& ref : body->Meshes(i))
            if (GameObject* piece = ref.Resolve(scene)) {
                if (hit && health && health->IsAlive())
                    m_glowMaterials.Apply(scene.Self(), material,
                        {1.0f, 0.94f, 0.78f, 1.0f}, 0.8f + 2.2f * k, Time::time,
                        "", piece->name.c_str(), 0.0f);
                objectMask.Set(*piece, color, width, true, !outlineThroughWalls);
                ++debugOutlined;
            }
    }

    /// @note 申告はそのフレームだけ有効なので、出したいフレームは毎回パスも要求する。
    if (debugOutlined > 0)
        if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

} // namespace sandbox
