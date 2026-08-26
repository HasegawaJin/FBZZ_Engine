/// @file GlowPartComponent.hpp
/// @brief 極性を持たない発光パーツへ固定色を流す。M_GlowPart を敵以外でも使うための駆動側
/// @author Hasegawa Jin
/// @date 2026-08-25
///
/// WHY 材質を分けずにスクリプトで色を書くか:
///   M_GlowPart は «光り方» を決める 1 枚で、色は GameObject 単位の override が決める、
///   という形になっている。プレイヤーの緑だけ専用の .mat を作ると、光り方を直したときに
///   «敵は直ったがプレイヤーだけ古い» が起きる。12.2 は色に意味を持たせる設計なので、
///   色の担当と光り方の担当は分けたままにしておきたい。
///
/// WHY 毎フレーム書くか (OnStart の 1 回で済まさないか):
///   override は MaterialComponent 側に持たれていて、材質のホットリロードや Play/Stop の
///   往復で作り直される経路がある。1 度きりの書き込みは、そのときだけ静かに消えて
///   «なぜかプレイヤーだけ光らない» になる。書き込みは 2 本の定数更新でしかないので、
///   毎フレーム同じ値を押し直す方が安い。極性側 (PolarityTargetComponent) も同じ形。
///
/// NOTE: 明滅 (pulse) と点光源の自動生成は 2026-08-26 に撤去した。
///   明滅は自発光を上下させるだけでは画面に出ず (トーンマップとブルームが振れ幅を潰す)、
///   点光源はスキンドメッシュのノードが位置を持たないため置き場所を別に作る必要があり、
///   «発光部位へ色を流す» というこのコンポーネントの担当から外れていた。
///   光源が要る箇所は LightComponent をシーンへ置いて、位置も人が決めること。
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

    // 発光を書き込む先。glowTarget で別 GameObject を指す構成もあるため必須にはしない。
    FBZZ_OPTIONAL_COMPONENT(MaterialComponent)

public:
    FBZZ_GROUP("Glow")
    FBZZ_FIELD_COLOR(glowColor, (Vector4{ 0.20f, 1.00f, 0.45f, 1.00f }), "Color")
    FBZZ_TOOLTIP("12.2 の配色から選ぶこと。緑 = プレイヤー / 赤 = ＋ / 青 = －")
    FBZZ_FIELD_RANGE(float, intensity, 0.65f, "Intensity", 0.0f, 4.0f)
    FBZZ_TOOLTIP("発光量。極性の敵と揃えるなら 0.65 前後")

    FBZZ_GROUP("Target")
    // WHY GameObject ではなく MaterialComponent で受けるか: このスロットへ挿せるのは
    //     «材質を持っている» GameObject だけ。型で受ければピッカーにその候補しか出ず、
    //     材質の無いノードを挿して «光らない» と悩む経路が消える。
    FBZZ_REF(MaterialComponent, glowTarget, "Glow Target")
    FBZZ_TOOLTIP("発光メッシュを持つ GameObject。空なら自分自身")
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "Emissive Slot", 0, 15)

    void OnStart()  override;
    void OnUpdate() override;

private:
    [[nodiscard]] MaterialInstance Target() const
    {
        // glowTarget 未アサインなら EntityRef が無効になり、Instance が自分自身へ落ちる。
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
