/// @file    SerpentRigComponent.hpp
/// @brief   斬られた節を輪郭で光らせる。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 発光帯ではなく輪郭で出すか (boss-serpent.md「節の表示」):
///   装甲が暗く、面積のわりに画面では細い。自発光を上げても «光っている» と気づく前に
///   色が飽和する。しかも `M_E_RingGlow` は «電気が通っている» を示す中立の琥珀で、
///   灯しっぱなしにすると斬っていない節まで反応して見える。
///   形の外側へ出る輪郭なら、視界の端の節まで «今そこに入った» が読める。
///
/// WHY 節ごとに独立して光るか:
///   胴は 28 節あり、どこに入ったかが分からないと «当たったが手応えが無い» になる。
///   光るのは斬られた節だけで、消えるまでの秒数は部位 (BossPartComponent) が持つ。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <algorithm>

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
    // WHY 既定で遮蔽を無視するか: 胴は自分自身でとぐろを巻く。手前の節が奥の節を
    //     隠すのが常態なので、遮蔽で捨てるとマスクがほとんど残らない ─
    //     «どの節に入ったか» は隠れていても読めなければ意味が無い。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "壁を透かす")
    FBZZ_FIELD_COLOR(damageFlashColor, (Vector4{ 1.0f, 0.97f, 0.90f, 1.0f }), "Damage Flash")
    FBZZ_TOOLTIP("斬られた節の輪郭が一瞬寄る色。長さは BossPartComponent の Flash")
    FBZZ_FIELD_RANGE(float, damageFlashStrength, 1.0f, "被弾フラッシュ倍率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で «斬られても光らない»。輪郭をどれだけ太らせるか")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "輪郭を出す")

    void OnUpdate() override;

private:
    [[nodiscard]] SerpentBodyComponent*      Body()  const { return scene.GetScript<SerpentBodyComponent>(); }
    [[nodiscard]] SerpentSpineComponent*     Spine() const { return scene.GetScript<SerpentSpineComponent>(); }
    [[nodiscard]] SerpentHitboxRigComponent* Rig()   const { return scene.GetScript<SerpentHitboxRigComponent>(); }
};

FBZZ_REFLECT(SerpentRigComponent)

inline void SerpentRigComponent::OnUpdate()
{
    debugOutlined = 0;
    if (!outlineSegments) return;

    auto* rig   = Rig();
    auto* body  = Body();
    auto* spine = Spine();
    if (!rig || !body) return;

    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        GameObject* hitbox = rig->SegmentHitbox(i);
        if (!hitbox) continue;
        const auto* part = scene.GetScript<BossPartComponent>(hitbox);
        if (!part) continue;
        const float flash = part->DamageFlash();
        if (flash <= 0.0f) continue;
        if (!body->IsAlive(i)) continue;
        if (spine && !spine->IsExposed(i)) continue;

        // マスクの意味は読む側 (Outline.hlsl) との取り決め: RGB = 色 / A = 太さ。
        const float   k     = Clamp01(flash) * Clamp01(damageFlashStrength);
        const Vector4 color{ damageFlashColor.x, damageFlashColor.y, damageFlashColor.z, 1.0f };
        const float   width = Clamp01(Lerp(Clamp01(outlineWidth), 1.0f, k));

        for (const EntityRef& ref : body->Meshes(i))
            if (GameObject* piece = ref.Resolve(scene)) {
                objectMask.Set(*piece, color, width, true, !outlineThroughWalls);
                ++debugOutlined;
            }
    }

    // 申告はそのフレームだけ有効なので、出したいフレームは毎回パスも要求する。
    if (debugOutlined > 0)
        if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

} // namespace sandbox
