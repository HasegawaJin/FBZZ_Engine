/// @file BossPolarityCoreComponent.hpp
/// @brief Boss「ポラリティ・コア」の極とフェーズ。IBoss の実装であり、発光色の正本
/// @author Hasegawa Jin
/// @date 2026-08-26
///
/// WHY 極を決める者が色も描くか:
///   企画書 12.2 は発光色に意味を持たせる設計で、赤 = ＋ / 青 = − がずれた瞬間に盤面が
///   読めなくなる。極を決める側と色を描く側を分けると、切替の予兆 (8 章の «切り替え
///   1 秒前から明滅») が 1 フレームずれるだけで «明滅が終わってから切り替わる» ように
///   見える。PolarityTargetComponent が塗られた極と色を同居させているのと同じ理由。
///
/// WHY PolarityTargetComponent に極を «書きに行く» か:
///   引力の候補を集めているのは PolarityFieldComponent で、その入口は
///   PolarityTargetComponent しかない。ボスが自分の中だけで極を持つと、盤面から見て
///   ボスは無極のままになり、10.6 の「逆極の雑魚をボスへ吸わせるのが唯一の解」が
///   成立しない。極の «決定» はここ、盤面への «公開» は向こう、と役割を分ける。
///
/// WHY 硬直中に極を落とすか (8 章の «極性リングが消灯し、無防備であることが見た目で分かる»):
///   8 章は「プレイヤーの攻め時は、ボスが硬直している間に雑魚を組むこと」と書いている。
///   硬直は «雑魚をぶつける時間» ではなく «雑魚を組み立てる時間» で、その間ボスが
///   吸い寄せていると、組んでいる途中の雑魚が勝手に飛んでいく。極を落とせば、
///   硬直の間は安全に塗れて、復帰した瞬間に組んだぶんがまとめて飛ぶ、という
///   リズムがルールだけで出る。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPolarityCoreComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(BossPolarityCoreComponent, Script, IBoss)

public:
    FBZZ_GROUP("Identity")
    FBZZ_FIELD(std::string, bossName, "POLARITY CORE", "Name")

    FBZZ_GROUP("Polarity (8章)")
    FBZZ_FIELD_ENUM(Polarity, startPolarity, Polarity::Plus, "Start Polarity",
                    "None", "Plus (＋赤)", "Minus (−青)")
    FBZZ_TOOLTIP("開始時の極。None にすると誰も吸い寄せられず、最初の切替まで盤面が動かない")
    FBZZ_FIELD_RANGE(float, phase1SwitchSeconds, 6.0f, "P1 Switch", 0.5f, 30.0f)
    FBZZ_TOOLTIP("10.6 の P1 は 6 秒周期")
    FBZZ_FIELD_RANGE(float, phase2SwitchSeconds, 4.0f, "P2 Switch", 0.5f, 30.0f)
    FBZZ_TOOLTIP("10.6 の P2 は 4 秒周期。フェーズが上がると読む間隔が詰まる")
    FBZZ_FIELD_RANGE(float, switchWarnSeconds, 1.0f, "Warn Lead", 0.0f, 5.0f)
    FBZZ_TOOLTIP("10.6「切替 1 秒前からリング発光が明滅する」。ここが予兆の長さ")

    FBZZ_GROUP("Phases (10.6)")
    FBZZ_FIELD_RANGE(float, phase2HealthRatio, 0.5f, "P2 At Health", 0.0f, 1.0f)
    FBZZ_TOOLTIP("HP 割合がこれ以下になったら P2 へ入る。10.6 は 50%")

    FBZZ_GROUP("Glow (12.2)")
    FBZZ_REF_LIST_FIELD(GameObject, emissiveTargets, "Emissive Targets")
    FBZZ_TOOLTIP("極性色を書き込む発光メッシュ。E_CoreGlow と E_RingGlow を両方指定する。"
                 "空なら自分自身")
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "Emissive Slot", 0, 15)
    FBZZ_FIELD_RANGE(float, emissiveScale, 0.65f, "Emissive Scale", 0.0f, 4.0f)
    FBZZ_TOOLTIP("帯電中の基準発光。12.2 の «白飛びさせない» に合わせて 1.0 より低く始める")
    FBZZ_FIELD_RANGE(float, warnBlinkHz, 7.0f, "Warn Blink Hz", 0.5f, 30.0f)
    FBZZ_TOOLTIP("予兆の明滅の速さ。遠くから読ませるので、残り時間の明滅より速く振る")
    FBZZ_FIELD_RANGE(float, warnBlinkDepth, 0.85f, "Warn Blink Depth", 0.0f, 1.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "Phase")
    FBZZ_FIELD_READ_ONLY(std::string, debugPolarity, "", "Polarity")
    FBZZ_FIELD_READ_ONLY(float, debugSwitchIn, 0.0f, "Switch In")

    // ── IBoss ───────────────────────────────────────────────────────────────
    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }
    [[nodiscard]] Polarity CurrentPolarity() const override
    {
        return m_coreDark ? Polarity::None : m_polarity;
    }
    [[nodiscard]] float PolaritySwitchRemaining() const override { return m_switchRemaining; }
    [[nodiscard]] int  CurrentPhase() const override { return m_phase; }
    [[nodiscard]] int  PhaseCount()   const override { return 2; }
    [[nodiscard]] bool IsStaggered()  const override { return m_staggered; }

    // ── AI からの通知 ───────────────────────────────────────────────────────

    /// 硬直に入った / 抜けた。攻撃ごとの隙 (8 章) を AI が知らせる。
    /// WHY 状態を AI から «もらう» か: 硬直かどうかを決めているのは攻撃の進行そのもので、
    ///     ここから覗くとステート名の綴りを 2 箇所で持つことになる。
    void SetStaggered(bool staggered) { m_staggered = staggered; }

    /// 極性リングの消灯。突進の激突スタン (8 章) だけで使う。
    /// WHY 硬直と分けるか: 踏みつけ後の 0.4 秒の硬直でも極を落とすと、盤面のリンクが
    ///     攻撃のたびに切れて «引かれていた雑魚が急に止まる» が頻発する。消灯は
    ///     «長く無防備を晒す» 激突だけの見た目で、短い硬直とは別の出来事。
    void SetCoreDark(bool dark) { m_coreDark = dark; }

    /// 極が切り替わった瞬間に呼ばれる。10.6 の P2 で磁力パルスを重ねるための合図。
    /// WHY 購読させるか: パルスを «出すかどうか» はフェーズごとの攻撃表 (10.6) の話で、
    ///     極の切替そのものの関心ではない。ここが攻撃を知っていると、攻撃を 1 つ足す
    ///     たびに極の実装を触ることになる。
    std::function<void()> onPolaritySwitch;

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// HP 割合からフェーズを決め直す。変わったら切替タイマーも新しい周期へ入れ直す。
    void RefreshPhase();
    /// 今のフェーズの切替周期。
    [[nodiscard]] float SwitchPeriod() const
    {
        return std::max(m_phase >= 2 ? phase2SwitchSeconds : phase1SwitchSeconds, 0.1f);
    }
    void Switch();
    /// 盤面 (PolarityTargetComponent) へ今の極を公開する。
    void PublishPolarity();
    void ApplyGlow();

    Polarity m_polarity  = Polarity::Plus;
    float    m_switchRemaining = 0.0f;
    int      m_phase     = 1;
    bool     m_staggered = false;
    bool     m_coreDark  = false;
};

FBZZ_REFLECT(BossPolarityCoreComponent)


inline void BossPolarityCoreComponent::OnStart()
{
    m_polarity  = startPolarity;
    m_phase     = 1;
    m_staggered = false;
    m_coreDark  = false;
    m_switchRemaining = SwitchPeriod();

    auto* target = scene.GetScript<PolarityTargetComponent>();
    if (!target) {
        // 盤面から見てボスが存在しなくなる。「雑魚をぶつけても何も起きない」という
        // 形でしか症状が出ないので、名指しで止める。
        debug.LogError("BossPolarityCoreComponent requires a PolarityTargetComponent on the "
                       "same object (the attraction field only sees that script).");
    } else if (!target->selfDriven) {
        // 塗れてしまうと 8 章の「ボスには極性を付与できない」が崩れ、しかも
        // 見た目は «たまに色が変わる» だけなので、遊んでいて原因に辿り着けない。
        debug.LogError("BossPolarityCoreComponent requires Self Driven on the "
                       "PolarityTargetComponent (8: the boss cannot be painted).");
    }

    PublishPolarity();
    ApplyGlow();
}

inline void BossPolarityCoreComponent::RefreshPhase()
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    if (!health) return;

    const float ratio = health->Normalized();
    const int   next  = ratio <= Clamp01(phase2HealthRatio) ? 2 : 1;
    if (next == m_phase) return;

    m_phase = next;
    debugPhase = m_phase;
    // 周期が縮んだのに残り時間が前の周期のままだと、フェーズが変わった直後の 1 回だけ
    // 古い間隔で切り替わる。読み手 (プレイヤー) はそこで数え直すので、必ず入れ直す。
    m_switchRemaining = std::min(m_switchRemaining, SwitchPeriod());
}

inline void BossPolarityCoreComponent::Switch()
{
    m_polarity = OppositeOf(m_polarity);
    // 開始が None のまま切り替わっても None のままになる。盤面が永久に動かないので、
    // ここで必ずどちらかへ倒す。
    if (m_polarity == Polarity::None) m_polarity = Polarity::Plus;

    m_switchRemaining = SwitchPeriod();
    PublishPolarity();
    if (onPolaritySwitch) onPolaritySwitch();
}

inline void BossPolarityCoreComponent::PublishPolarity()
{
    if (auto* target = scene.GetScript<PolarityTargetComponent>())
        target->SetPolarity(CurrentPolarity());
}

inline void BossPolarityCoreComponent::OnUpdate()
{
    RefreshPhase();

    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const bool  alive  = !health || health->IsAlive();

    // 倒れた後も切替が続くと、崩れ落ちている最中に雑魚が吸い寄せられ続ける。
    if (alive && !m_coreDark) {
        m_switchRemaining -= Time::deltaTime;
        if (m_switchRemaining <= 0.0f) Switch();
    }

    // 倒れたら極そのものを捨てる。消灯 (m_coreDark) は復帰する前提の一時的な状態なので、
    // 元の極を覚えたまま CurrentPolarity() だけ None を返す方で表す。
    if (!alive) m_polarity = Polarity::None;

    // 硬直中の消灯も死亡も、盤面へ公開する値は CurrentPolarity() 1 本に通す。
    // 見えている色と吸い寄せる力が食い違うと、プレイヤーの読みが外れる。
    PublishPolarity();

    debugPhase     = m_phase;
    debugPolarity  = PolaritySymbol(CurrentPolarity());
    debugSwitchIn  = std::max(m_switchRemaining, 0.0f);

    ApplyGlow();
}

inline void BossPolarityCoreComponent::ApplyGlow()
{
    const Polarity shown = CurrentPolarity();

    Vector4 color = PolarityColor(shown);
    float   scale = shown == Polarity::None ? 0.0f : std::max(emissiveScale, 0.0f);

    // 8 章の予兆。切替の直前だけ速く明滅させる。12.4 の «残り時間が減るほど速くなる»
    // 明滅とは別物 (あちらは切れる警告、こちらは «極が入れ替わる» 警告) なので、
    // 速さを変えずに «出るか出ないか» で区別できるようにしてある。
    if (shown != Polarity::None && switchWarnSeconds > 0.0f &&
        m_switchRemaining <= switchWarnSeconds) {
        const float phase = std::sin(Time::time * std::max(warnBlinkHz, 0.1f) * TWO_PI) * 0.5f + 0.5f;
        scale *= Lerp(1.0f - Clamp01(warnBlinkDepth), 1.0f, phase);
    }

    const uint32_t slot = static_cast<uint32_t>(emissiveSlot);
    const auto write = [&](const MaterialInstance& instance) {
        if (!instance.IsValid()) return;
        instance.SetVector3(kEmissiveColorId, { color.x, color.y, color.z });
        instance.SetFloat(kEmissiveScaleId, scale);
    };

    if (emissiveTargets.empty()) {
        write(material.Instance(slot));
        return;
    }
    for (const auto& target : emissiveTargets) {
        // 未アサインの枠は Instance が自分自身へ落ちる。空欄 1 つでルート
        // (発光しないメッシュ) が対象に混ざる。
        if (!target.IsAssigned()) continue;
        write(material.Instance(target.ref, slot));
    }
}

} // namespace sandbox
