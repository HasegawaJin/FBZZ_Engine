/// @file    BossBreakComponent.hpp
/// @brief   崩しゲージ。弾き・見切り・斬撃で溜まり、満ちたらボスが倒れる
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// @note ボスに HP は無い。HP に見せると «削れば勝てる» と読まれ、弾く理由が «損をしない»
///       で終わるため、満ちるか戻るかだけの器にし、満ちた瞬間を必ず転倒にする。
/// @note ゲージはボス側に置く。耐えの量・転倒の長さはボスごとに違い、プレイヤーは
///       «どれだけ溜めたか» を送るだけで満ちた時の挙動はボスが決める (onBreak)。
/// @note 放っておくと戻る。戻らないと弾かずに削るだけでも «いつか満ちる» が保証され、
///       «弾き続ける» ことの意味が消えるため。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <functional>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossBreakComponent : public Script {
    FBZZ_SCRIPT(BossBreakComponent)

public:
    FBZZ_GROUP("Gauge")
    FBZZ_FIELD_RANGE(float, maxBreak, 100.0f, "Max", 10.0f, 1000.0f)
    FBZZ_FIELD_RANGE(float, parryGain, 40.0f, "弾き", 0.0f, 500.0f)
    FBZZ_TOOLTIP("弾き 1 回。3 回で満ちる量が既定。ここが戦いのテンポそのもの")
    FBZZ_FIELD_RANGE(float, parryHeavyGain, 60.0f, "Parry (heavy)", 0.0f, 500.0f)
    FBZZ_TOOLTIP("突進のような重い手を弾いたとき。読み切りの難しさに見合う量")
    FBZZ_FIELD_RANGE(float, perfectDodgeGain, 10.0f, "Just Dodge", 0.0f, 500.0f)
    FBZZ_TOOLTIP("ジャスト回避。弾けない手 (輪・ビーム) にも道を残すが、弾きより薄く")
    /// @note 3.5 は «斬るだけで満たすには 30 発近く要る» という宣言 (BladeComponent::
    ///       ResolveHit) と揃えてある。6.0 に上げると弾かずに斬るだけで転倒できてしまう
    ///       ため戻した (Docs/break-parry.md)。斬った分は «刃の熱» として弾きへ返す。
    FBZZ_FIELD_RANGE(float, slashGain, 3.5f, "Slash", 0.0f, 100.0f)
    FBZZ_TOOLTIP("斬撃 1 発。«斬るだけ» で満たすには約 30 発 要る量に留める")
    FBZZ_FIELD_RANGE(float, chargedSlashGain, 9.0f, "Charged Slash", 0.0f, 200.0f)

    /// @note «上手い弾き» に上乗せする。窓 0.22 秒のどこで受けても同じ量だと、読む理由が
    ///       «損をしない» で終わるため、窓の頭で受けた弾きだけ量を上げる。
    FBZZ_GROUP("Mastery")
    FBZZ_FIELD_RANGE(float, justParryScale, 1.5f, "Just Parry x", 1.0f, 3.0f)
    FBZZ_TOOLTIP("窓の頭 (Just Parry) で受けた弾きの倍率。1 で通常と同じ")
    FBZZ_FIELD_RANGE(float, streakBonus, 0.15f, "Streak Bonus", 0.0f, 1.0f)
    FBZZ_TOOLTIP("被弾せずに続けた弾き 1 回ごとに足す倍率。被弾でリセット")
    FBZZ_FIELD_RANGE_INT(int, streakCap, 4, "Streak Cap", 0, 10)
    FBZZ_TOOLTIP("倍率が積み上がる上限の回数。4 × 0.15 = 最大 +60%")

    /// 刃の熱 ── 斬った分が «次の弾き 1 回» に乗る。
    /// @note 斬撃だけが動詞の輪から切れていたため導入。崩しゲージ自体は増やさず、
    ///       «弾きを濃くする» 側へ返して斬る動機に繋げる。
    /// @note 溜め置きはできない。熱が乗るのは実際に当たった斬撃 (AddSlash) だけで、
    ///       当てるには危険な間合いへ入る必要があり、さらに被弾で消える。
    FBZZ_FIELD_RANGE(float, edgeBonus, 0.10f, "刃の熱", 0.0f, 1.0f)
    FBZZ_TOOLTIP("弾く前に当てた斬撃 1 発ごとに、その弾きへ足す倍率。"
                 "弾いた瞬間に使い切る。被弾で消える")
    FBZZ_FIELD_RANGE_INT(int, edgeCap, 5, "熱の上限 [発]", 0, 20)
    FBZZ_TOOLTIP("熱が積み上がる上限の発数。5 × 0.10 = 最大 +50%。"
                 "**連撃 1 セット (5 連) がちょうど上限**になるように置いてある")

    FBZZ_GROUP("Decay")
    /// @note 8/秒 は溜めてから 2.5 秒後、12.5 秒で全部消える速さ。ボスの手番は 2〜5 秒
    ///       なので、これより速いと何度弾いてもゲージが «同じ所» に戻り実感が出ない。
    FBZZ_FIELD_RANGE(float, decayDelay, 2.5f, "保持", 0.0f, 20.0f)
    FBZZ_TOOLTIP("最後に溜めてからこの秒数は戻らない。連撃の合間に減り始めると «溜まらない» に見える")
    FBZZ_FIELD_RANGE(float, decayPerSecond, 8.0f, "Decay / s", 0.0f, 200.0f)

    FBZZ_GROUP("Topple")
    /// @note 9 秒 (5 秒から拡張)。登攀ギミック向けの拡張は取り下げ済み (Docs/climb-core.md)
    ///       だが、9 秒は «どの脚を斬るか選ぶ» 時間として効いているため戻していない。
    FBZZ_FIELD_RANGE(float, toppleSeconds, 9.0f, "Topple Seconds", 0.5f, 15.0f)
    FBZZ_TOOLTIP("満ちて倒れている時間。この間だけ «とどめ» が通る。"
                 "短くすると «どの部位を斬るか» の選択が消える")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugBreak, 0.0f, "Break")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Idle", "状態")
    FBZZ_FIELD_READ_ONLY(std::string, debugLastSource, "-", "Last Source")
    FBZZ_FIELD_READ_ONLY(int, debugStreak, 0, "Parry Streak")
    FBZZ_FIELD_READ_ONLY(int, debugEdge, 0, "刃の熱")

    /// 満ちた瞬間。ボス側が転倒へ繋ぐ。引数は転倒の長さ [秒]。
    std::function<void(float seconds)> onBreak;

    /// ゲージの比 [0,1]。倒れている間は «残り時間» の比になる。
    [[nodiscard]] float Ratio() const;
    [[nodiscard]] bool  IsToppled() const { return m_toppled; }
    /// 倒れている残り [0,1]。倒れていなければ 0。
    [[nodiscard]] float ToppleRemaining01() const
    { return m_toppled ? Clamp01(m_toppleLeft / std::max(m_toppleSeconds, 0.01f)) : 0.0f; }
    /// 直近に溜まった瞬間からの秒数。HUD の «伸びた» 演出が読む。
    [[nodiscard]] float SinceLastGain() const { return m_idle; }

    /// 弾き 1 回。just は窓の頭で受けた «読み切り»。連続の弾きは回数で倍率が積み、
    /// 直前に当てた斬撃 (刃の熱) はここで使い切られる。
    void AddParry(bool heavy, bool just = false)
    {
        if (m_toppled || m_gainScale <= 0.0f) return;
        const float base   = heavy ? parryHeavyGain : parryGain;
        const float streak = 1.0f + std::max(streakBonus, 0.0f)
                           * static_cast<float>(std::min(m_parryStreak, std::max(streakCap, 0)));
        const float edge   = 1.0f + std::max(edgeBonus, 0.0f)
                           * static_cast<float>(std::min(m_edge, std::max(edgeCap, 0)));
        const float scale  = (just ? std::max(justParryScale, 1.0f) : 1.0f) * streak * edge;
        ++m_parryStreak;
        debugStreak = m_parryStreak;
        /// @note 熱は «次の 1 回» に乗る物なので、乗せたら必ず 0 へ戻す。残すと
        ///       1 回斬っておけば以後ずっと濃い弾きになり、斬る動機が最初の 1 回で終わる。
        m_edge    = 0;
        debugEdge = 0;
        Add(base * scale, just ? (heavy ? "Just Parry (heavy)" : "Just Parry")
                               : (heavy ? "Parry (heavy)" : "Parry"));
    }
    void AddPerfectDodge()    { Add(perfectDodgeGain, "Just Dodge"); }
    /// 被弾した。続けていた弾きの積み上げも刃の熱も消える。
    void ResetParryStreak() { m_parryStreak = 0; m_edge = 0; debugStreak = 0; debugEdge = 0; }
    [[nodiscard]] int ParryStreak() const { return m_parryStreak; }
    /// 今の刃の熱 [発]。HUD が «次の弾きがどれだけ濃いか» を出すのに読む。
    [[nodiscard]] int Edge() const { return std::min(m_edge, std::max(edgeCap, 0)); }
    /// 全部の溜まり方に掛かる倍率。プレイヤーの «土壇場» (残り HP 僅か) が上げる。
    void SetGainScale(float scale) { m_gainScale = std::max(scale, 0.0f); }

    /// 潜行・登場演出など、ボス側の都合で攻防できない間は減衰の時計だけを止める。
    void SetDecayPaused(bool paused) { m_decayPaused = paused; }

    /// 斬撃だけに掛かる倍率。盤面が «斬る番» になっている間だけボスが上げる。
    ///
    /// @note 全体倍率 (SetGainScale) とは分ける。あちらはプレイヤーの状態 (土壇場) で
    ///       弾きにも回避にも乗るが、こちらは «斬撃だけが答えになる盤面» (蛇の檻) 用で、
    ///       混ぜると檻の中で弾きまで濃くなる。
    void SetSlashScale(float scale) { m_slashScale = std::max(scale, 0.0f); }
    [[nodiscard]] float SlashScale() const { return m_slashScale; }

    void AddSlash(bool charged)
    {
        if (m_toppled || m_gainScale <= 0.0f) return;
        /// @note 熱を 1 発ぶん溜める。上限で頭打ちにしておかないと、連撃を延々当てた後の
        ///       1 回だけが桁違いに濃くなる。
        if (m_edge < std::max(edgeCap, 0)) ++m_edge;
        debugEdge = m_edge;
        Add((charged ? chargedSlashGain : slashGain) * m_slashScale,
            charged ? "Charged Slash" : "Slash");
    }
    /// 任意の量を溜める。満ちたら onBreak を 1 度だけ呼ぶ。倒れている間は無視する。
    void Add(float amount, const char* source);

    /// ボスが倒れた。ここから seconds 秒はゲージが «残り時間» を表す。
    /// 突進の自滅激突のように、ゲージを経由せず倒れる経路もここへ来る。
    void BeginTopple(float seconds);
    /// 倒れている残りを «あと remaining 秒» へ伸ばす。倒れていなければ何もしない。
    /// 縮めることはできない ─ バーが逆走すると «起きかけて座り直した» に見える。
    void ExtendTopple(float remaining);
    /// 起き上がった (時間切れ・とどめ)。ゲージは 0 へ戻る。
    void EndTopple();

    void OnStart()  override;
    void OnUpdate() override;

private:
    float m_break         = 0.0f;
    float m_idle          = 0.0f;
    float m_decayIdle     = 0.0f;
    bool  m_decayPaused   = false;
    bool  m_toppled       = false;
    float m_toppleLeft    = 0.0f;
    float m_toppleSeconds = 0.0f;
    bool  m_warnedNoOwner = false;
    int   m_parryStreak   = 0;
    /// 弾く前に当てた斬撃の数 (刃の熱)。次の弾きで使い切る。
    int   m_edge          = 0;
    float m_slashScale    = 1.0f;
    float m_gainScale     = 1.0f;
};

FBZZ_REFLECT(BossBreakComponent)

inline float BossBreakComponent::Ratio() const
{
    if (m_toppled) return ToppleRemaining01();
    return Clamp01(m_break / std::max(maxBreak, 1.0f));
}

inline void BossBreakComponent::OnStart()
{
    m_break         = 0.0f;
    m_idle          = 0.0f;
    m_decayIdle     = 0.0f;
    m_decayPaused   = false;
    m_toppled       = false;
    m_toppleLeft    = 0.0f;
    m_toppleSeconds = 0.0f;
    m_warnedNoOwner = false;
    m_parryStreak   = 0;
    m_edge          = 0;
    m_gainScale     = 1.0f;
    m_slashScale    = 1.0f;
    debugBreak      = 0.0f;
    debugState      = "Idle";
    debugLastSource = "-";
    debugStreak     = 0;
    debugEdge       = 0;
}

inline void BossBreakComponent::Add(float amount, const char* source)
{
    if (amount <= 0.0f || m_toppled || m_gainScale <= 0.0f) return;

    m_break = std::min(m_break + amount * m_gainScale, std::max(maxBreak, 1.0f));
    m_idle  = 0.0f;
    m_decayIdle = 0.0f;
    debugBreak      = m_break;
    debugLastSource = source ? source : "-";

    if (m_break < std::max(maxBreak, 1.0f)) return;

    /// @note 満ちた。倒すのはボスの仕事で、こちらは合図を出すだけ。呼び先が無く何も
    ///       起きないとゲージが «飾り» に見えるため、未購読ならエラーで知らせる。
    if (onBreak) {
        onBreak(std::max(toppleSeconds, 0.5f));
    } else if (!m_warnedNoOwner) {
        m_warnedNoOwner = true;
        debug.LogError("BossBreakComponent: the gauge is full but no boss subscribed to onBreak. "
                       "The boss AI must set it (BossAiComponent / SerpentAiComponent).");
    }
}

inline void BossBreakComponent::BeginTopple(float seconds)
{
    m_toppled       = true;
    m_toppleSeconds = std::max(seconds, 0.05f);
    m_toppleLeft    = m_toppleSeconds;
    m_break         = std::max(maxBreak, 1.0f);
    debugBreak      = m_break;
    debugState      = "Toppled";
}

inline void BossBreakComponent::ExtendTopple(float remaining)
{
    if (!m_toppled || remaining <= m_toppleLeft) return;
    m_toppleLeft = remaining;
    /// @note 分母も一緒に伸ばす。残りだけ増やすと比が 1 を超え、バーが枠から溢れる。
    m_toppleSeconds = std::max(m_toppleSeconds, remaining);
}

inline void BossBreakComponent::EndTopple()
{
    if (!m_toppled) return;
    m_toppled    = false;
    m_toppleLeft = 0.0f;
    m_break      = 0.0f;
    m_idle       = 0.0f;
    m_decayIdle  = 0.0f;
    debugBreak   = 0.0f;
    debugState   = "Idle";
}

inline void BossBreakComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (m_toppled) {
        /// @note 時間切れの起き上がりはボスが EndAct で告げる。ここでは残りを数えるだけ。
        ///       告げ忘れの保険として 0 で止める (負の残りをバーに出さない)。
        m_toppleLeft = std::max(m_toppleLeft - dt, 0.0f);
        debugBreak   = m_break;
        return;
    }

    m_idle += dt;
    const float previousIdle = m_decayIdle;
    if (!m_decayPaused) m_decayIdle += dt;
    if (m_break > 0.0f && m_decayIdle >= std::max(decayDelay, 0.0f)) {
        const float decayTime = m_decayIdle - std::max(previousIdle, std::max(decayDelay, 0.0f));
        m_break = std::max(m_break - std::max(decayPerSecond, 0.0f) * decayTime, 0.0f);
        debugBreak = m_break;
    }
    debugState = m_break > 0.0f ? "Charging" : "Idle";
}

} // namespace sandbox
