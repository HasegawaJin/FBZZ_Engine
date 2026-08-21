// FBZZ Engine
// PolarityTargetComponent.hpp | sandbox
// 極性を帯びられる対象。敵・柱・壁の全てに付ける。
//
// WHY プレイヤー側ではないのにここにあるか:
//   銃が撃つ先がこれである。これが無いと「撃っても何も起きない」ため、
//   企画書 12.1 が最優先とした命中の手応えを一度も確認できない。
//   引力 (7.3) と衝突 (7.4) は次フェーズで、ここには状態と見た目までを入れる。
//
// WHY プレイヤーには付けないか:
//   5 章「プレイヤー自身は極性を持たない」。引力は敵同士・敵と障害物の間でのみ発生する。
#pragma once

#include <Scripts/Data/PolarityTuning.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 7.2 の持続時間テーブルのどれを使うか。質量ではなく役割で選ばせる。
// WHY 秒数を直接持たせないか: 各インスタンスに秒数を書けるようにすると、
//     同じノーマルスライムでも配置ごとに値がばらつき、7.7 の制約を
//     PolarityTuning 側で検算できなくなる。種別だけを選ばせ、値は共有アセットから引く。
enum class PolarityClass : int {
    NormalSlime  = 0,
    ShooterSlime = 1,
    HeavySlime   = 2,
    Pillar       = 3,
};

class PolarityTargetComponent : public Script {
    FBZZ_SCRIPT(PolarityTargetComponent)

    // 発光色の駆動に MaterialInstance を使うため、スロットを持つ Material が要る。
    // 無いと極性が乗っても見た目が一切変わらず、12.4「残り時間の可視化は仕様」が崩れる。
    FBZZ_REQUIRE_COMPONENT(MaterialComponent)

public:
    FBZZ_REQUIRED_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("持続時間と明滅の共有調整値。未割り当てでは動作を開始しない")

    FBZZ_FIELD_ENUM(PolarityClass, polarityClass, PolarityClass::NormalSlime, "Class",
                    "Normal Slime", "Shooter Slime", "Heavy Slime", "Pillar")
    FBZZ_TOOLTIP("7.2 の持続時間テーブルのどれを使うか")

    // 引力で動かない側 (7.3)。柱とヘビースライムが該当する。
    // 引力の実装は次フェーズだが、対象の性格はここで確定させておく。
    FBZZ_FIELD(bool, isAnchor, false, "Is Anchor")
    FBZZ_TOOLTIP("true なら引力で動かない。柱とヘビースライムに立てる")

    // 発光を書き込む Material スロット。SkinnedMeshRenderer は 1 GameObject で
    // モデル全体を描くため、光らせたいサブメッシュを番号で指す。
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "Emissive Slot", 0, 15)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")

    // ── 問い合わせ ──────────────────────────────────────────────────────
    [[nodiscard]] Polarity Current() const { return m_polarity; }
    [[nodiscard]] bool  IsCharged() const { return m_polarity != Polarity::None; }
    [[nodiscard]] float RemainingSeconds() const { return m_remaining; }
    // 極性弾が命中した直後の短い硬直。敵 AI はこの間だけ移動・攻撃を止める。
    [[nodiscard]] bool IsHitReacting() const { return m_hitReactRemaining > 0.0f; }
    // 1 = 付与直後 / 0 = 切れる直前。明滅速度と濃さの両方がこれで決まる。
    [[nodiscard]] float RemainingNormalized() const;
    // 7.2 の種別ごとの基準持続時間。
    [[nodiscard]] float BaseDuration() const;

    // 極性弾が当たったときに銃から呼ばれる。7 章のルール 3 行を適用する。
    // 戻り値の change を見て、呼び出し側が延長 / 中和のフィードバックを出し分ける。
    PolarityResult Apply(Polarity incoming);

    // 中和以外で強制的に落とす場合 (衝突で倒れた・Wave リセット等)。
    void ClearPolarity();

    void OnStart()  override;
    void OnUpdate() override;

private:
    void ApplyVisual();

    Polarity m_polarity  = Polarity::None;
    float    m_remaining = 0.0f;
    // 現在の帯電が始まったときの基準持続時間。延長の上限計算に使う。
    float    m_chargeBase = 0.0f;
    float    m_hitReactRemaining = 0.0f;
};

FBZZ_REFLECT(PolarityTargetComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

// Material プロパティ名。AssetBrowser が生成する .mat テンプレートの
// [params] セクションのキーと一致させる。
// MaterialInstance は HLSL リフレクション上の変数名を検証するため camelCase を使う。
// .mat 内の別名ではなくシェーダー変数へ合わせないと、警告だけ出て発光が更新されない。
inline constexpr MaterialPropertyId kEmissiveColorId{ "emissiveColor" };
inline constexpr MaterialPropertyId kEmissiveScaleId{ "emissiveScale" };

inline float PolarityTargetComponent::BaseDuration() const
{
    switch (polarityClass) {
    case PolarityClass::ShooterSlime: return tuning->durationShooter;
    case PolarityClass::HeavySlime:   return tuning->durationHeavy;
    case PolarityClass::Pillar:       return tuning->durationPillar;
    case PolarityClass::NormalSlime:  break;
    }
    return tuning->durationNormal;
}

inline float PolarityTargetComponent::RemainingNormalized() const
{
    if (m_chargeBase <= 0.0f) return 0.0f;
    return Clamp01(m_remaining / m_chargeBase);
}

inline PolarityResult PolarityTargetComponent::Apply(Polarity incoming)
{
    const PolarityResult result = ResolvePolarity(m_polarity, incoming);
    m_hitReactRemaining = tuning->hitReactSeconds;

    switch (result.change) {
    case PolarityChange::Applied:
        m_polarity   = result.polarity;
        m_chargeBase = BaseDuration();
        m_remaining  = m_chargeBase;
        break;

    case PolarityChange::Extended: {
        // 残り時間へ加算する。上限が無いと 1 体を撃ち続けるだけで永久に帯電でき、
        // 3.3 の「残り何秒かを把握し続ける」という思考そのものが消える。
        const float base    = BaseDuration();
        const float ratio   = tuning->extendRatio;
        const float capMul  = tuning->extendCapRatio;
        m_remaining = Min(m_remaining + base * ratio, base * capMul);
        // 上限まで伸びた状態を 1.0 として扱えるよう、基準も伸ばす。
        m_chargeBase = Max(m_chargeBase, m_remaining);
        break;
    }

    case PolarityChange::Neutralized:
        m_polarity   = Polarity::None;
        m_remaining  = 0.0f;
        m_chargeBase = 0.0f;
        break;
    }

    ApplyVisual();
    return result;
}

inline void PolarityTargetComponent::ClearPolarity()
{
    m_polarity   = Polarity::None;
    m_remaining  = 0.0f;
    m_chargeBase = 0.0f;
    ApplyVisual();
}

inline void PolarityTargetComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PolarityTargetComponent requires PolarityTuning.fzdata.");
        enabled = false;
        return;
    }
    // 開始時は必ず無極。前回 Play の状態が見た目に残らないようにする。
    m_hitReactRemaining = 0.0f;
    ClearPolarity();
}

inline void PolarityTargetComponent::OnUpdate()
{
    m_hitReactRemaining = Max(0.0f, m_hitReactRemaining - Time::deltaTime);

    if (m_polarity == Polarity::None) {
        debugRemaining = 0.0f;
        return;
    }

    m_remaining -= Time::deltaTime;
    if (m_remaining <= 0.0f) {
        ClearPolarity();
        debugRemaining = 0.0f;
        return;
    }

    debugRemaining = m_remaining;
    ApplyVisual();
}

inline void PolarityTargetComponent::ApplyVisual()
{
    const MaterialInstance instance = material.Instance(static_cast<uint32_t>(emissiveSlot));
    if (!instance.IsValid()) return;

    const Vector4 color = PolarityColor(m_polarity);
    instance.SetVector3(kEmissiveColorId, { color.x, color.y, color.z });

    if (m_polarity == Polarity::None) {
        instance.SetFloat(kEmissiveScaleId, 0.0f);
        return;
    }

    // 12.4 は残り時間の可視化を「演出ではなく仕様」と書いている。
    //   - 残りが減るほど明滅が速くなる
    //   - 切れる直前に色が薄くなる
    const float remaining = RemainingNormalized();
    const float hzMin  = tuning->blinkHzMin;
    const float hzMax  = tuning->blinkHzMax;
    const float depth  = tuning->blinkDepth;

    // remaining 1 → hzMin / remaining 0 → hzMax
    const float hz    = Lerp(hzMax, hzMin, remaining);
    const float phase = std::sin(Time::time * hz * TWO_PI) * 0.5f + 0.5f;
    const float pulse = Lerp(1.0f - Clamp01(depth), 1.0f, phase);

    instance.SetFloat(kEmissiveScaleId,
                      kEmissiveBase * FadeFromRemaining(remaining) * pulse);
}

} // namespace sandbox
