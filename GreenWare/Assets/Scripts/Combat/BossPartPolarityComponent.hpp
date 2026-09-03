/// @file    BossPartPolarityComponent.hpp
/// @brief   ボスの部位 1 つぶんの極と体力。斬られた部位が帯び、削られる
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
    // 極の «持ち主» を切り替える。
    //
    // WHY 2 通り要るか: ボス 1 の脚は «ボスが帯びていて、プレイヤーが逆極で斬る» 的で、
    //     蛇の節は «プレイヤーが塗って、逆極どうしを引き合わせる» 素材。同じ部位の器を
    //     使っているが、極を書き込む側が逆になる。どちらかに寄せると、もう一方の
    //     ボスが丸ごと成立しなくなる。
    FBZZ_FIELD(bool, selfDriven, false, "Self Driven")
    FBZZ_TOOLTIP("ON = ボス自身が極を持つ (斬った側は «合っているか» を見るだけ)。"
                 "OFF = プレイヤーが斬って極を乗せる")

    FBZZ_FIELD_RANGE(float, duration, 5.0f, "Duration", 0.5f, 30.0f)
    FBZZ_TOOLTIP("塗られた極が残る秒数 (Self Driven が OFF のときだけ)。"
                 "«もう 1 本を塗りに行く» 猶予そのもの")

    // 逆極の刃で斬られてから、極を取り戻すまで。無極のあいだはどちらの剣でも満額通る。
    FBZZ_FIELD_RANGE(float, neutralizeSeconds, 1.5f, "Neutralized For", 0.1f, 10.0f)
    FBZZ_TOOLTIP("正しい極で斬られた部位が無極でいる秒数 (Self Driven のときだけ)。"
                 "長いほど «持ち替えずに畳み掛けられる» 時間が伸びる")

    // WHY 判定用の半径を自前で持つか: 斬撃の扇は bodybounds (描画バウンズ) で
    //     相手の太さを足しているが、部位はレンダラーを持たない当たり判定なので
    //     そこから寸法が出ない。届く太さをここで宣言する。
    FBZZ_FIELD_RANGE(float, hitRadius, 1.20f, "Hit Radius", 0.1f, 5.0f)
    FBZZ_TOOLTIP("斬撃の扇へ足す太さ。脚は細いので、素の中心距離だけでは «見えているのに当たらない» になる")

    FBZZ_GROUP("Health")
    // WHY ボス本体の HP と別に持つか: 部位を落とすのは HP を削るのとは別の勝ち筋で、
    //     目盛りが 1 桁違う (本体 800 に対して部位は数発ぶん)。同じ数字を分け合うと、
    //     «脚を落とす» と «倒す» のどちらを進めているのかが画面から読めなくなる。
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 150, "Max Health", 1, 1000)
    FBZZ_TOOLTIP("この部位を削り切るのに要る量。斬撃 1 発は PolarityTuning の Damage (既定 25)")

    FBZZ_GROUP("Debug")
    // どの脚に属する部位か ("_FR" など)。BossHitboxRigComponent が生成時に入れる。
    //
    // WHY GameObject 名から切り出さないか: 名前は «当たり判定の名前» で、部位の
    //     識別子ではない。命名を変えた瞬間に IK のボーン名解決が黙って外れる。
    FBZZ_FIELD_READ_ONLY(std::string, legSuffix, "", "Leg")
    FBZZ_FIELD_READ_ONLY(std::string, debugPolarity, "None", "Polarity")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "Health")
    FBZZ_FIELD_READ_ONLY(bool, debugBroken, false, "Broken")

    /// 極を乗せる。同じ極を重ねたら残りを満タンへ戻す。壊れた部位は受け付けない。
    /// Self Driven の部位は受け付けない (極を決めるのはボスの側)。
    void Apply(Polarity polarity);
    void Clear();

    /// ボスが自分の極を決める。Self Driven の部位へリグが入れる。
    void SetPolarity(Polarity polarity);

    /// 正しい極で斬られた。`neutralizeSeconds` のあいだ無極になる。
    ///
    /// WHY 極を «消す» ことを報酬にするか: 合っていたら削れるだけだと、色を読む理由が
    ///     «損をしない» で終わる。読み切った数秒はどちらの剣でも通る、という形にすると
    ///     «正解を当てた» が次の数秒の攻めやすさとして返る。
    void Neutralize();
    [[nodiscard]] bool IsNeutralized() const { return m_neutralize > 0.0f; }

    /// もぎ取られた。以後この部位は極を持てない。
    ///
    /// WHY 極を «持てない» まで含めるか: 脚を落とした後も塗れると、画面から消えた部位が
    ///     盤面には残ることになる。«見えているものが全部» を崩さない。
    void Break();

    /// 斬られて削る。**削り切った呼び出しだけ** true を返す。
    ///
    /// WHY 残量ではなく «削り切った瞬間» を返すか: 呼び手 (斬撃) は毎フレーム来る。
    ///     0 かどうかを外から見て壊す形にすると、次の一振りでもう一度壊すことになる。
    bool Damage(int amount);

    [[nodiscard]] Polarity Current()    const { return m_polarity; }
    [[nodiscard]] bool     IsCharged()  const { return m_polarity != Polarity::None; }
    [[nodiscard]] bool     IsBroken()   const { return m_broken; }
    [[nodiscard]] float    Remaining()  const { return m_remaining; }
    /// 1 = 乗せた直後 / 0 = 切れる直前。
    [[nodiscard]] float    RemainingNormalized() const;

    [[nodiscard]] int   Health()      const { return std::max(m_health, 0); }
    [[nodiscard]] bool  IsDepleted()  const { return m_health == 0; }
    /// 1 = 無傷 / 0 = 削り切った。
    [[nodiscard]] float HealthNormalized() const;

    void OnStart()  override;
    void OnUpdate() override;

private:
    Polarity m_polarity  = Polarity::None;
    float    m_remaining = 0.0f;
    bool     m_broken    = false;
    /// 打ち消しの残り [秒]。Self Driven の部位でだけ動く。
    float    m_neutralize = 0.0f;
    // -1 = 未初期化。BossHitboxRigComponent が実行時に組むので、OnStart より先に
    // 斬られうる (同じフレームに扇が通る)。
    int      m_health    = -1;
};

FBZZ_REFLECT(BossPartPolarityComponent)

inline void BossPartPolarityComponent::Apply(Polarity polarity)
{
    if (m_broken || selfDriven || polarity == Polarity::None) return;
    m_polarity  = polarity;
    m_remaining = std::max(duration, 0.1f);
}

inline void BossPartPolarityComponent::SetPolarity(Polarity polarity)
{
    if (m_broken) return;
    m_polarity  = polarity;
    // 自前の極は時間で消えない。消えるのは斬られて打ち消されたときだけ。
    m_remaining = 0.0f;
}

inline void BossPartPolarityComponent::Neutralize()
{
    if (m_broken) return;
    m_polarity   = Polarity::None;
    m_remaining  = 0.0f;
    m_neutralize = std::max(neutralizeSeconds, 0.05f);
}

inline void BossPartPolarityComponent::Break()
{
    m_broken    = true;
    debugBroken = true;
    m_health    = 0;
    Clear();
}

inline bool BossPartPolarityComponent::Damage(int amount)
{
    if (m_broken || amount <= 0) return false;
    if (m_health < 0) m_health = std::max(maxHealth, 1);
    if (m_health == 0) return false;

    m_health = std::max(m_health - amount, 0);
    return m_health == 0;
}

inline float BossPartPolarityComponent::HealthNormalized() const
{
    if (m_health < 0) return 1.0f;
    return Clamp01(static_cast<float>(m_health) / static_cast<float>(std::max(maxHealth, 1)));
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

inline void BossPartPolarityComponent::OnStart()
{
    if (m_health < 0) m_health = std::max(maxHealth, 1);
}

inline void BossPartPolarityComponent::OnUpdate()
{
    if (selfDriven) {
        // 自前の極は時間で切れない。動くのは打ち消しの残りだけで、戻す判断は
        // リグ (BossPolarityRigComponent) が持つ ─ 4 本の色の配り方を決めるのは
        // 部位 1 つの都合ではないため。
        if (m_neutralize > 0.0f)
            m_neutralize = std::max(m_neutralize - Time::deltaTime, 0.0f);
    } else if (m_polarity != Polarity::None) {
        m_remaining -= Time::deltaTime;
        if (m_remaining <= 0.0f) Clear();
    }

    debugPolarity  = PolaritySymbol(m_polarity);
    debugRemaining = std::max(m_remaining, 0.0f);
    debugHealth    = Health();
}

} // namespace sandbox
