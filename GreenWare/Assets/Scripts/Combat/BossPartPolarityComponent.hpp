/// @file    BossPartPolarityComponent.hpp
/// @brief   ボスの部位 1 つぶんの極。斬られた部位が帯び、リグ側が異極の対を探す
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY PolarityTargetComponent を使い回さないか:
///   あちらは «盤面のコマ» の入口で、PolarityFieldComponent が候補として集めたうえで
///   PolarityBodyComponent へ速度を書きに行く。部位はボーンに追従する当たり判定なので
///   速度を持てず、盤面へ混ぜても «動かない候補» が引力のリンク枠を埋めるだけになる。
///   部位の極はボスの中だけで閉じる別系統として持つ。
///
/// WHY 逆極を当てても中和しないか:
///   部位は 4 本の脚を «別々の極で塗り分ける» ための的で、塗り直しは前の指示の
///   取り消しでしかない。中和の罰 (PolarityTargetComponent の paintLock) を持ち込むと、
///   狙う脚を 1 本間違えただけで手が止まる。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPartPolarityComponent : public Script {
    FBZZ_SCRIPT(BossPartPolarityComponent)

public:
    FBZZ_GROUP("Charge")
    FBZZ_FIELD_RANGE(float, duration, 5.0f, "Duration", 0.5f, 30.0f)
    FBZZ_TOOLTIP("乗せた極が残る秒数。«もう 1 本を塗りに行く» 猶予そのもの")

    // WHY 判定用の半径を自前で持つか: 斬撃の扇は bodybounds (描画バウンズ) で
    //     相手の太さを足しているが、部位はレンダラーを持たない当たり判定なので
    //     そこから寸法が出ない。届く太さをここで宣言する。
    FBZZ_FIELD_RANGE(float, hitRadius, 1.20f, "Hit Radius", 0.1f, 5.0f)
    FBZZ_TOOLTIP("斬撃の扇へ足す太さ。脚は細いので、素の中心距離だけでは «見えているのに当たらない» になる")

    FBZZ_GROUP("Debug")
    // どの脚に属する部位か ("_FR" など)。BossHitboxRigComponent が生成時に入れる。
    //
    // WHY GameObject 名から切り出さないか: 名前は «当たり判定の名前» で、部位の
    //     識別子ではない。命名を変えた瞬間に IK のボーン名解決が黙って外れる。
    FBZZ_FIELD_READ_ONLY(std::string, legSuffix, "", "Leg")
    FBZZ_FIELD_READ_ONLY(std::string, debugPolarity, "None", "Polarity")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")

    /// 極を乗せる。同じ極を重ねたら残りを満タンへ戻す。
    void Apply(Polarity polarity);
    void Clear();

    [[nodiscard]] Polarity Current()    const { return m_polarity; }
    [[nodiscard]] bool     IsCharged()  const { return m_polarity != Polarity::None; }
    [[nodiscard]] float    Remaining()  const { return m_remaining; }
    /// 1 = 乗せた直後 / 0 = 切れる直前。
    [[nodiscard]] float    RemainingNormalized() const;

    void OnUpdate() override;

private:
    Polarity m_polarity  = Polarity::None;
    float    m_remaining = 0.0f;
};

FBZZ_REFLECT(BossPartPolarityComponent)

inline void BossPartPolarityComponent::Apply(Polarity polarity)
{
    if (polarity == Polarity::None) return;
    m_polarity  = polarity;
    m_remaining = std::max(duration, 0.1f);
}

inline void BossPartPolarityComponent::Clear()
{
    m_polarity  = Polarity::None;
    m_remaining = 0.0f;
}

inline float BossPartPolarityComponent::RemainingNormalized() const
{
    if (m_polarity == Polarity::None) return 0.0f;
    return Clamp01(m_remaining / std::max(duration, 0.1f));
}

inline void BossPartPolarityComponent::OnUpdate()
{
    if (m_polarity != Polarity::None) {
        m_remaining -= Time::deltaTime;
        if (m_remaining <= 0.0f) Clear();
    }

    debugPolarity  = PolaritySymbol(m_polarity);
    debugRemaining = std::max(m_remaining, 0.0f);
}

} // namespace sandbox
