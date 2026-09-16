/// @file    BossCoreComponent.hpp
/// @brief   Boss「ポラリティ・コア」のフェーズと発光。IBoss の実装であり、盤面への問い合わせ口
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 状態を «もらう» 側に寄せるか:
///   硬直・消灯・交戦・進行はどれも、決めているのは攻撃の進行や脚のリグの側にある。
///   ここから覗きに行くと、ステート名や脚の数え方を 2 箇所で持つことになる。
///   決めるのは持ち主、名乗るのはここ、と向きを 1 本にしておく。
///
/// WHY 発光をここが描くか:
///   コアとリングの明るさは «無防備かどうか» を遠くから読ませる唯一の絵で、
///   消灯 (SetCoreDark) と同じ 1 つの状態から出る。状態を持つ側と描く側を分けると、
///   倒れているのにリングが点いている 1 フレームが必ず生まれる。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossCoreComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(BossCoreComponent, Script, IBoss)

public:
    FBZZ_GROUP("識別")
    FBZZ_FIELD(std::string, bossName, "鉄骸の番人 IRON WARDEN", "名前")

    FBZZ_GROUP("Phases (10.6)")
    FBZZ_FIELD_RANGE(float, phase2HealthRatio, 0.5f, "P2 At Health", 0.0f, 1.0f)
    FBZZ_TOOLTIP("HP 割合がこれ以下になったら P2 へ入る。10.6 は 50%")

    FBZZ_GROUP("Glow (12.2)")
    FBZZ_REF_LIST_FIELD(GameObject, emissiveTargets, "Emissive Targets")
    FBZZ_TOOLTIP("明るさを書き込む発光メッシュ。E_CoreGlow と E_RingGlow を両方指定する。"
                 "空なら自分自身")
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "発光スロット", 0, 15)
    FBZZ_FIELD_RANGE(float, emissiveScale, 0.65f, "Emissive Scale", 0.0f, 4.0f)
    FBZZ_TOOLTIP("平常時の基準発光。12.2 の «白飛びさせない» に合わせて 1.0 より低く始める")
    FBZZ_FIELD_RANGE(float, warnBlinkHz, 7.0f, "Warn Blink Hz", 0.5f, 30.0f)
    FBZZ_TOOLTIP("予兆の明滅の速さ。遠くから読ませるので、残り時間の明滅より速く振る")
    FBZZ_FIELD_RANGE(float, warnBlinkDepth, 0.85f, "Warn Blink Depth", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "位相")

    // ── IBoss ───────────────────────────────────────────────────────────────
    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }
    [[nodiscard]] int  CurrentPhase() const override { return m_phase; }
    [[nodiscard]] int  PhaseCount()   const override { return 2; }
    [[nodiscard]] bool IsStaggered()  const override { return m_staggered; }
    [[nodiscard]] bool IsEngaged()    const override { return m_engaged; }

    // ── 弾いて崩す (Docs/break-parry.md) ──────────────────────────────────────
    // 消灯 (m_coreDark) は激突と転倒でしか立たない。«倒れている» はそれと同じ。
    [[nodiscard]] bool IsToppled() const override { return m_coreDark; }
    void OnParried(const Vector3& hitPoint) override { if (onParried) onParried(hitPoint); }
    bool Execute(GameObject* part, const Vector3& from) override
    { return onExecute ? onExecute(part, from) : false; }
    [[nodiscard]] int PartsRemaining() const override { return m_partsRemaining; }
    [[nodiscard]] int PartsTotal()     const override { return m_partsTotal; }

    /// 弾かれた反応と とどめ の中継。AI と脚のリグがそれぞれ結ぶ。
    ///
    /// WHY 関数で結ぶか: BossAiComponent と BossRigComponent はどちらも
    ///     ここを include している。こちらから両者を引き返すと include が環になる。
    ///     持つ側が結ぶ、という向きに揃える。
    std::function<void(const Vector3&)>               onParried;
    std::function<bool(GameObject*, const Vector3&)>  onExecute;

    /// 進行の物差し。脚のリグが «残り本数 / 全本数» を毎フレーム押す。
    /// フェーズもこれから決まる (HP は崩しの遊びでは動かない)。
    void SetProgress(int remaining, int total)
    {
        m_partsRemaining = remaining;
        m_partsTotal     = total;
    }

    // ── AI からの通知 ───────────────────────────────────────────────────────

    /// 硬直に入った / 抜けた。攻撃ごとの隙 (8 章) を AI が知らせる。
    /// WHY 状態を AI から «もらう» か: 硬直かどうかを決めているのは攻撃の進行そのもので、
    ///     ここから覗くとステート名の綴りを 2 箇所で持つことになる。
    void SetStaggered(bool staggered) { m_staggered = staggered; }

    /// リングの消灯。突進の激突スタンと転倒だけで使う。
    /// WHY 硬直と分けるか: 踏みつけ後の 0.4 秒の硬直でも消すと、攻撃のたびにリングが
    ///     ちらついて «無防備» の合図として読めなくなる。消灯は «長く無防備を晒す»
    ///     激突と転倒だけの見た目で、短い硬直とは別の出来事。
    void SetCoreDark(bool dark) { m_coreDark = dark; }

    /// 交戦が始まった / まだ始まっていない。BossRoomTriggerComponent が知らせる。
    ///
    /// WHY 極の側が «交戦中か» を持つか: 進行も HUD も補充も、ボスの入口は IBoss 1 本。
    ///     登場の条件を持つスクリプトを直接見に行かせると、条件を «部屋» から
    ///     «HP 閾値» や «時間» へ変えたときに、見に行っている側を全部書き換えることになる。
    ///     条件は持つ側が決め、結果だけをここへ預ける (硬直・消灯と同じ «もらう» 向き)。
    void SetEngaged(bool engaged) { m_engaged = engaged; }

    /// 予兆の明滅。磁力パルスの溜めの間だけ AI が立てる。
    ///
    /// WHY こちらから攻撃を覗かないか: 何が予兆を出すかは攻撃表の話で、攻撃を 1 つ
    ///     足すたびにここを触ることになる。«明滅しているか» だけを預かる。
    void SetTelegraph(bool warning) { m_telegraph = warning; }

    void OnStart()  override;
    void OnUpdate() override;
    /// 名簿から降りる。載ったままだと、畳んだシーンのボスを進行が探し当てる。
    void OnDestroy() override { IBoss::Unbind(scene.Self(), this); }

private:
    /// 残り本数からフェーズを決め直す。
    void RefreshPhase();
    void ApplyGlow();

    BossArmorMaterials m_armorMaterials;
    int      m_glowHealth = -1;
    float    m_glowHit   = 0.0f;
    int      m_phase     = 1;
    bool     m_staggered = false;
    bool     m_coreDark  = false;
    bool     m_telegraph = false;
    int      m_partsRemaining = -1;
    int      m_partsTotal     = 0;
    /// WHY 初期値が false か: ScriptSystem は GameObject ごとに OnAwake → OnStart →
    ///     OnUpdate をまとめて回すので、先に並んだ GameObject (Manager の進行など) は
    ///     ボスのどのスクリプトも走る前に IsEngaged() を読む。そこで true を返すと、
    ///     まだ 0 の HP を «撃破済み» と読まれて開始と同時にクリアになる。
    ///     «まだ» から始めて、OnStart で «登場条件を持たないボス» だけを true にする。
    bool     m_engaged   = false;
};

FBZZ_REFLECT(BossCoreComponent)


inline void BossCoreComponent::OnStart()
{
    // «ボスとして» 名乗る。型で引く経路はこの環境では空を返すので、
    // ここを書き忘れると倒してもステージが終わらない (IBoss.hpp の WHY)。
    IBoss::Bind(scene.Self(), this);

    m_armorMaterials.Reset();
    m_glowHealth = -1;
    m_glowHit = 0.0f;
    m_phase     = 1;
    m_staggered = false;
    m_coreDark  = false;
    m_telegraph = false;
    m_partsRemaining = -1;
    m_partsTotal     = 0;
    // 登場条件を持つ構成では、この直後に BossRoomTriggerComponent が false へ戻す。
    // 持たない構成 (置いた瞬間から戦っている) はここで交戦中になる。
    m_engaged   = true;

    ApplyGlow();
}

inline void BossCoreComponent::RefreshPhase()
{
    // 崩しの遊びでは HP が動かない。脚の残り本数を «残り» として読む (2 本で P2)。
    float ratio = 1.0f;
    if (m_partsTotal > 0) {
        ratio = Clamp01(static_cast<float>(m_partsRemaining) / static_cast<float>(m_partsTotal));
    } else {
        const auto* health = scene.GetScript<EnemyHealthComponent>();
        if (!health) return;
        ratio = health->Normalized();
    }

    const int   next  = ratio <= Clamp01(phase2HealthRatio) ? 2 : 1;
    if (next == m_phase) return;

    m_phase = next;
    debugPhase = m_phase;
}

inline void BossCoreComponent::OnUpdate()
{
    RefreshPhase();
    debugPhase = m_phase;
    ApplyGlow();
}

inline void BossCoreComponent::ApplyGlow()
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const bool  alive  = !health || health->IsAlive();

    // WHY 警告色 1 本か: 赤と青は «どちらの刀か» に割り当ててある (BladeColors.hpp)。
    //     ボスがその 2 色を出すと、プレイヤーの刀の色と同じ語で喋ることになる。
    const int hp = health ? health->CurrentHealth() : 0;
    if (m_glowHealth >= 0 && hp < m_glowHealth && alive) m_glowHit = 1.0f;
    m_glowHealth = hp;
    m_glowHit = std::max(0.0f, m_glowHit - Time::deltaTime / 0.18f);
    if (alive) m_armorMaterials.Apply(scene.Self(), material, m_telegraph ? 1.0f : 0.0f,
        m_coreDark, m_glowHit, Time::deltaTime);
    const float breath = 0.5f + 0.5f * std::sin(Time::time * 2.6f);
    Vector4 color = m_telegraph ? kColorWarning
        : Vector4{ 1.0f, 0.56f + breath * 0.12f, 0.2f + breath * 0.08f, 1.0f };
    float scale = (alive && !m_coreDark) ? std::max(emissiveScale, 0.0f) : 0.0f;

    // 磁力パルスの予兆。溜めの間だけ速く明滅させる。12.4 の «残り時間が減るほど
    // 速くなる» 明滅とは速さで区別できるようにしてある。
    if (scale > 0.0f && m_telegraph) {
        const float phase = std::sin(Time::time * std::max(warnBlinkHz, 0.1f) * TWO_PI) * 0.5f + 0.5f;
        scale *= Lerp(1.0f - Clamp01(warnBlinkDepth), 1.0f, phase);
    }

    if (!m_telegraph) scale *= 0.78f + 0.22f * breath;

    if (alive && !m_coreDark && !m_telegraph && m_glowHit > 0.0f) {
        color.y = Lerp(color.y, 0.95f, m_glowHit);
        color.z = Lerp(color.z, 0.8f, m_glowHit);
        scale += m_glowHit * 1.4f;
    }
    const uint32_t slot = static_cast<uint32_t>(emissiveSlot);
    const auto write = [&](const MaterialInstance& instance) {
        if (!instance.IsValid() || instance.HasProperty(MaterialPropertyId{ "dissolveAmount" })) return;
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
