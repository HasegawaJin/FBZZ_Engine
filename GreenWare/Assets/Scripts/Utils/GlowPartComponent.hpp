/// @file    GlowPartComponent.hpp
/// @brief   発光パーツへ固定色を流す。M_GlowPart を色ごとに複製しないための駆動側
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note M_GlowPart は «光り方» を決める 1 枚で、色は GameObject 単位の override が決める。
///       専用 .mat を色ごとに作ると光り方を直したとき片方だけ古いままになるため、色
///       (盤面を読む情報、Utils/BladeColors.hpp) と光り方の担当を分けている。override は
///       材質のホットリロードや Play/Stop で作り直されるため、OnStart 1 回でなく毎フレーム
///       押し直す (定数更新 2 本のみで安い)。明滅と点光源の自動生成は 2026-08-26 に撤去
///       (自発光の上下は画面に出ず、点光源は位置を持たないため)。要る箇所は
///       LightComponent を人が置く。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <cstdint>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class GlowPartComponent : public Script {
    FBZZ_SCRIPT(GlowPartComponent)

    /// 発光を書き込む先。glowTarget で別 GameObject を指す構成もあるため必須にはしない。
    FBZZ_OPTIONAL_COMPONENT(MaterialComponent)

public:
    FBZZ_GROUP("発光")
    FBZZ_FIELD_COLOR(glowColor, (Vector4{ 0.20f, 1.00f, 0.45f, 1.00f }), "Color")
    FBZZ_TOOLTIP("Utils/BladeColors.hpp の配色から選ぶこと。緑 = プレイヤー / 赤 = 右刀 / 青 = 左刀")
    FBZZ_FIELD_RANGE(float, intensity, 0.65f, "強さ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("発光量。既定は kEmissiveBase (0.65) ─ 白飛びさせずに色が残る上限")

    FBZZ_GROUP("対象")
    /// @note GameObject ではなく MaterialComponent で受ける。このスロットへ挿せるのは
    ///       «材質を持っている» GameObject だけになり、材質の無いノードを挿して
    ///       «光らない» と悩む経路が消える。
    FBZZ_REF(MaterialComponent, glowTarget, "Glow Target")
    FBZZ_TOOLTIP("発光メッシュを持つ GameObject。空なら自分自身")
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "発光スロット", 0, 15)

    void OnStart()  override;
    void OnUpdate() override;

private:
    [[nodiscard]] MaterialInstance Target() const
    {
        /// @note glowTarget 未アサインなら EntityRef が無効になり、Instance が自分自身へ落ちる。
        return material.Instance(glowTarget.ref, static_cast<uint32_t>(emissiveSlot));
    }
};

FBZZ_REFLECT(GlowPartComponent)


inline void GlowPartComponent::OnStart()
{
    if (!Target().IsValid()) {
        debug.LogError("GlowPartComponent has no material to drive. Add a "
                       "MaterialComponent here, or assign Glow Target.");
    }
}

inline void GlowPartComponent::OnUpdate()
{
    const MaterialInstance instance = Target();
    if (!instance.IsValid()) return;

    instance.SetVector3(kEmissiveColorId, { glowColor.x, glowColor.y, glowColor.z });
    instance.SetFloat(kEmissiveScaleId, Max(intensity, 0.0f));
}

} // namespace sandbox
