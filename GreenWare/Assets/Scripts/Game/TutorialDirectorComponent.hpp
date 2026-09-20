/// @file    TutorialDirectorComponent.hpp
/// @brief   Stage_01 の «教える» 側。幕を進め、ボスに出してよい手を渡す
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// 進行と支援方針の正本は Assets/Docs/tutorial.md。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossRigComponent.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossMoveGate.hpp>
#include <Scripts/Combat/BossRoomTriggerComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/GameFlowComponent.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <algorithm>
#include <cstddef>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class TutorialDirectorComponent : public Script {
    FBZZ_SCRIPT(TutorialDirectorComponent)

public:
    FBZZ_GROUP("進行")
    FBZZ_FIELD(bool, teach, true, "教える")
    FBZZ_TOOLTIP("切ると門を置かず、最初から «読む» の段になる。"
                 "手触りだけ見たいときのための逃げ道")
    FBZZ_FIELD_RANGE(float, praiseSeconds, 1.6f, "合格の間 [s]", 0.0f, 5.0f)
    FBZZ_TOOLTIP("合格してから次の幕の文言へ移るまで。«出来た» を読ませる時間で、"
                 "この間もボスは前の幕の門のまま動いている")
    FBZZ_FIELD_RANGE_INT(int, slashChain, 3, "繋ぐ斬撃の数", 1, 6)
    FBZZ_TOOLTIP("幕 1 の合格条件。**続けて当てた最長**がこの数に届くまで。"
                 "«N 回当てた» にすると 1 発ずつ間を空けても通り、"
                 "«押し続けると繋がる» が伝わらない")
    FBZZ_FIELD_RANGE_INT(int, surviveMoves, 2, "やり過ごす手の数", 1, 6)
    FBZZ_TOOLTIP("幕 1 の合格条件。ボスの手をこの回数、被弾せずにやり過ごす。"
                 "**被弾すると数え直し** ─ そこが «避けられていない» の境目")

    /// @brief 着弾の直前に、世界を落として画面を沈め、指示を画面の真ん中へ持ってくる層。
    /// @note 予兆は «足元» に出るがボス 1 は 10m あって近づくほど足元が本体に隠れる
    ///       (BossTelegraph.hpp)。初見は «見るべき所» も «押す瞬間» も知らないため、
    ///       一度だけ世界を止めて目線の中央で言い切る形にする。
    /// @note 回数を打ち切る。毎回スローが入る戦いは «教わっている» ままで終わるため、
    ///       数回で消し合図が消えたことに気付かせないのが良い覚え方。
    /// @note 実時間で数える。この層は世界を遅くしている当人なので、スケール時間で
    ///       数えると自分の掛けたスローぶんだけ伸びて戻ってこない。
    FBZZ_GROUP("教える瞬間")
    FBZZ_FIELD(bool, cueEnabled, true, "着弾の直前に «ここだ» を出す")
    FBZZ_FIELD_RANGE(float, cueAt, 0.55f, "合図の時刻 [0,1]", 0.1f, 0.95f)
    FBZZ_TOOLTIP("予兆の進みがここを越えたら合図。1 に近いほど «ぎりぎり» になる")
    FBZZ_FIELD_RANGE(float, cueSeconds, 0.80f, "長さ [s]", 0.1f, 3.0f)
    FBZZ_TOOLTIP("実時間。スローの長さと、指示が中央に居る長さを兼ねる")
    FBZZ_FIELD_RANGE(float, cueTimeScale, 0.35f, "スロー", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, cueDim, 0.34f, "暗さ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("画面を沈める量。上げるほど文字は読めるが、予兆そのものが見えなくなる")
    FBZZ_FIELD_RANGE_INT(int, cueMaxPerAct, 3, "1 幕あたりの回数", 0, 20)
    FBZZ_TOOLTIP("この回数を過ぎたら合図を出さない。**消えたことに気付かないのが良い**")

    /// 出来たことを返す層。段が進んだのを «文字が変わった» だけで伝えると、
    /// 成功したのか勝手に進んだのか分からない ─ **手応えが無い成功は成功に見えない。**
    FBZZ_GROUP("出来たときの手応え")
    FBZZ_FIELD_RANGE(float, successFlash, 0.34f, "閃光", 0.0f, 1.0f)
    FBZZ_FIELD_COLOR(successFlashColor, (Vector4{ 1.0f, 0.97f, 0.86f, 1.0f }), "閃光の色")
    FBZZ_TOOLTIP("弾きの白 (0.85,0.95,1.0) より暖かい色にする ─ "
                 "«防いだ» と «出来た» が同じ色だと区別が付かない")
    FBZZ_FIELD_RANGE(float, successShake, 0.18f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, successFov, 0.30f, "画角の跳ね", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, successSlow, 0.55f, "スロー", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, successCueSeconds, 0.55f, "中央に出す長さ [s]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("成功のセリフ を画面中央へ出しておく時間。教える合図より短くする ─ "
                 "長いと «褒められている時間» が戦いを止める")

    FBZZ_GROUP("守り")
    FBZZ_FIELD(bool, protectWhileTeaching, true, "撃破まで体力を1残す")
    FBZZ_TOOLTIP("体力が残り 1 より下へ落ちなくなる。痛みも土壇場の演出もそのまま出る。"
                 "自分の攻撃でボスを倒すまで支援する")

    FBZZ_GROUP("テンポ")
    FBZZ_FIELD_RANGE(float, teachTempo, 0.75f, "教える間のテンポ", 0.3f, 2.0f)
    FBZZ_TOOLTIP("幕 1〜3 でボスへ渡すテンポ。素の 1.15 より遅くしないと"
                 "踏みつけの予兆 (0.8 秒 ÷ テンポ) が «見てから» に間に合わない")
    FBZZ_FIELD_RANGE(float, executeTempo, 0.85f, "反撃から撃破までのテンポ", 0.3f, 2.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugAct, "-", "幕")
    FBZZ_FIELD_READ_ONLY(std::string, debugGoal, "-", "合格条件")
    FBZZ_FIELD_READ_ONLY(int, debugProgress, 0, "進捗")
    FBZZ_FIELD_READ_ONLY(bool, debugProtected, false, "守っている")

    /// 今どの幕か。UI (プロンプト) が読む。
    [[nodiscard]] int ActIndex() const { return static_cast<int>(m_act); }
    /// 今出している 1 行。
    [[nodiscard]] const std::string& Objective() const { return m_objective; }
    /// 今この幕で押させたい操作の論理名。空なら «押させたいボタンは無い»。
    /// @note 合格した後も残す。成功のセリフはそのボタンを押して出来たことへの返事
    ///       なので、押したアイコンが隣に残っている方が «これで合っていた» が伝わる。
    [[nodiscard]] bool StaminaFocusActive() const { return m_focusRemaining > 0.0f; }
    [[nodiscard]] const char* PromptAction() const
    {
        if (!teach || PartsLeft() == 0) return "";
        if (m_act == Act::Rush || (teach && m_act >= Act::Execute && TimeManagerComponent::ParryRushActive()))
            return TimeManagerComponent::ParryRushActive() ? actions::kAttack : actions::kParry;
        if (teach && m_act >= Act::Execute) {
            const auto* ai = Ai();
            if (ai && ai->CurrentTelegraph().kind == BossAttackKind::Pulse) return actions::kDodge;
        }
        if (teach && m_act == Act::Free) return actions::kParry;
        return Current().promptAction;
    }

    /// アイコンの隣に出す文字。**合図の最中は «今どうするか» の 1 行そのもの**を返す。
    /// 画面の真ん中へ出るのはこの文字なので、短い動詞のままでは «今» が言えない。
    [[nodiscard]] const char* PromptLabel() const
    {
        if (StaminaFocusActive()) return m_focusGuard
            ? "長押し中はスタミナが減るよ。離して回復！"
            : "回避でスタミナを使ったよ。残量を見よう！";
        if (m_passed) return SuccessLine();
        return m_objective.c_str();
    }

    [[nodiscard]] bool IsCelebrating() const { return m_passed; }

    /// 合図の強さ [0,1]。UI がこれで «中央へ寄って大きくなる» を作る。
    /// 0 なら普段の位置。
    [[nodiscard]] float CueStrength() const
    {
        const float total = m_cueTotal > 0.0f ? m_cueTotal : 1.0f;
        const float t = m_cue / total;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    /// 教える段をすべて終えたか (幕 5 = 読む)。
    [[nodiscard]] bool IsTeaching() const { return teach && m_act < Act::Free; }

    static TutorialDirectorComponent* Instance() { return s_instance; }

    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;
    /// 沈めたまま消えると画面が暗いまま戻らない。持ち主が必ず戻す。
    void OnDestroy() override
    {
        ClearDim();
        ClearLessonPresentation();
        if (auto* ai = Ai()) ai->ClearMoveGate();
        if (s_instance == this) s_instance = nullptr;
    }
    void OnDisable() override
    {
        ClearDim();
        ClearLessonPresentation();
        if (auto* ai = Ai()) ai->ClearMoveGate();
    }

private:
    /// 幕。順番そのものが «何を先に覚えるか» なので、値を入れ替えないこと。
    enum class Act : int {
        Approach = 0,   ///< 踏み込む。ボスはまだ眠っている
        Slash,          ///< 斬る。**ボスは 1 手も出さない**
        Dodge,          ///< 予兆を見て避ける
        Guard,
        Parry,          ///< 弾く
        Rush,
        Execute,        ///< 崩して脚を落とす
        Range,          ///< 間合いで手が変わる
        Free,           ///< もう教えない。読む
    };

    /// 幕 1 つぶんの «何を言い、ボスに何を許すか»。
    struct Step {
        const char*  objective;
        BossMoveGate gate;
        /// 体力の下限を掛けるか。
        bool protect;
        /// 今この幕で «押させたい» 操作 (InputActions の論理名)。空なら出さない。
        /// @note 目的の 1 行とは別に持つ。目的は «何をするか» だけを言い、どのキーかは
        ///       言わない (割り当ては OPTIONS で差し替えられるので文言に焼くと嘘になる)。
        ///       教える段では «どのボタンか» が要るため、ボタンはアイコンで別に出す。
        const char* promptAction;
        /// アイコンの隣に出す短い動詞。
        const char* promptLabel;
    };

    [[nodiscard]] const Step& Current() const;
    [[nodiscard]] const char* SuccessLine() const
    {
        switch (m_act) {
        case Act::Approach: return "さあ、一緒にいこう！";
        case Act::Slash: return "お見事！ 連撃が決まった！";
        case Act::Dodge: return "鮮やか！ 回避成功！";
        case Act::Guard: return "確認完了！ 構えを解いて回復しよう！";
        case Act::Rush: return "反撃成功！ 弾けば連撃のチャンス！";
        case Act::Parry: return "見事！ 弾き成功！";
        case Act::Execute: return "やった！ 脚を一本仕留めた！";
        case Act::Range: return "その調子！ 残る脚も仕留めよう！";
        case Act::Free: return "最後まで、キミと一緒だ！";
        }
        return "お見事！";
    }
    /// 今フレームの盤面に合わせて言い換えた 1 行。
    /// @note 幕ごとの固定文にはしない。固定の 1 行は «標識» であって説明でなく、
    ///       「印が出たら離れろ」をずっと出していても印を見ていない相手には伝わらず、
    ///       印が出ている最中には «今» が言えない。説明は起きていないときに、命令は
    ///       起きている瞬間に出す。同じ 1 行が状況で言い換わるなら読む場所は 1 つで済む。
    [[nodiscard]] const char* LiveObjective(const BossAiComponent* ai) const;

    /// アイコンの隣の短い動詞。そのとき本当に出せる手だけを名乗る。
    /// @note 固定で «弾き / とどめ» と書かない。とどめは倒れている 9 秒にしか存在せず、
    ///       立っている相手の前で «とどめ» と出ていると無い手を探して押され «壊れて
    ///       いる» と読まれる。名乗るのは «今そのボタンで起きること» だけにする。
    [[nodiscard]] const char* LivePromptLabel() const
    {
        if (m_act == Act::Execute) {
            GameObject*  boss  = BossObject();
            const IBoss* iboss = boss ? IBoss::Of(boss) : nullptr;
            return (iboss && iboss->IsToppled()) ? "とどめ" : "弾き";
        }
        return Current().promptLabel;
    }
    /// 今の幕の合格条件を満たしたか。
    [[nodiscard]] bool Passed();
    /// 幕が変わった瞬間に基準の数を控える。
    void EnterAct(Act act);
    void Advance();

    /// 盤面から引く相手。どれも «居ない» が普通に起こる (ボスが眠っている・
    /// マネージャーの OnStart がまだ、など) ので、毎フレーム引き直して null を許す。
    [[nodiscard]] BossAiComponent*     Ai() const;
    [[nodiscard]] GameObject*          BossObject() const { return FindBossOnBoard(scene); }
    [[nodiscard]] GameFlowComponent*   Flow() const;
    [[nodiscard]] PlayerComponent*     Player() const;
    [[nodiscard]] int                  PartsLeft() const;
    /// 部位の数が読めない盤面を 1 度だけ名指しで言う。黙って止まると «段が進まない»
    /// としか画面に出ず、原因がどこにも現れない。
    void WarnNoParts();

    Act   m_act     = Act::Approach;
    float m_praise  = 0.0f;   ///< 合格してから次の幕へ移るまでの残り [秒]
    bool  m_passed  = false;  ///< 今の幕は合格済みか (praise を数えている最中)

    /// 幕の始まりで控える «基準の数»。合格はここからの差で見る。
    int m_baseMoves   = 0;
    int m_baseDodges  = 0;
    int m_baseDamage  = 0;
    int m_baseParries = 0;
    int m_baseChain   = 0;
    int m_baseParts   = 0;
    int m_baseBladeHits = 0;
    int m_baseRushHits = 0;
    int m_extendedParry = -1;
    float m_baseGuardSpent = 0.0f;
    float m_seenDodgeSpent = 0.0f;
    float m_seenGuardSpent = 0.0f;
    float m_previousStamina = 1.0f;
    float m_focusBefore = 1.0f;
    float m_focusRemaining = 0.0f;
    bool m_focusGuard = false;
    bool m_shownDodgeFocus = false;
    bool m_shownGuardFocus = false;
    EntityRef m_focusPanels[4];
    EntityRef m_focusTrace;
    EntityRef m_focusCaption;
    EntityRef m_legMarker;
    EntityRef m_legCaption;
    int m_markedLeg = -1;
    void DriveStaminaFocus(float dt);
    void ClearLessonPresentation();

    std::string m_objective;
    /// 「部位の数が読めない」を 1 度だけ言うためのラッチ。
    bool m_warnedNoParts = false;

    /// @name 教える瞬間
    /// @{
    /// 合図の残り [実時間 秒]。
    float m_cue = 0.0f;
    /// 今の合図の長さ [秒]。強さはこれで割る (教えると出来たで尺が違う)。
    float m_cueTotal = 0.0f;
    /// 今の合図が画面を沈めるか。教えるときだけ true。
    bool  m_cueDims  = false;
    /// 今出ている予兆に対して、もう合図を出したか。予兆が消えるまで下りない。
    bool  m_cueArmed = false;
    /// この幕で出した回数。
    int   m_cueCount = 0;
    /// 画面を沈めているか。掛けたら必ず自分で戻す (SetFade は自動で減らない)。
    bool  m_dimming = false;

    /// 前フレームの «倒れている»。立ち上がりを取って合図を出す。
    bool m_wasToppled = false;
    /// 守りが外れた直後か [秒]。«もう守らない» を先に言うための短い窓。
    float m_justUnprotected = 0.0f;
    bool  m_wasProtected    = false;

    void DriveCue(const BossAiComponent* ai, float unscaledDt);
    void ClearDim();
    /// 合図を焚く。スローは TimeManager に預け、戻りは向こうが持つ。
    /// @param dims 画面を沈めるか。**教えるときだけ沈める** ─ 出来たときは
    ///             暖色で閃かせるので、同時に沈めると «褒めながら暗くする» になる。
    void FireCue(float seconds, float timeScale, bool dims);
    /// 段を越えた手応え。閃光・揺れ・画角・振動・音を 1 か所から出す。
    void FireSuccess();

    static inline TutorialDirectorComponent* s_instance = nullptr;
    /// @}
};

FBZZ_REFLECT(TutorialDirectorComponent)

inline BossAiComponent* TutorialDirectorComponent::Ai() const
{
    GameObject* boss = BossObject();
    return boss ? scene.GetScript<BossAiComponent>(boss) : nullptr;
}

inline GameFlowComponent* TutorialDirectorComponent::Flow() const
{
    /// @note 掴んで持たない。GameFlow は別の子オブジェクトに居るのでどちらの OnStart が
    ///       先に走るかはシーンの並び次第になり、1 度きりで掴むと並び順を変えただけで
    ///       目的の 1 行が二度と出なくなる (GameFlowComponent の Combat() と同じ判断)。
    for (GameObject* object : scene.FindObjectsOfType<GameFlowComponent>(true))
        if (auto* flow = scene.GetScript<GameFlowComponent>(object)) return flow;
    return nullptr;
}

inline PlayerComponent* TutorialDirectorComponent::Player() const
{
    GameObject* player = scene.FindWithTag("Player", true);
    return player ? scene.GetScript<PlayerComponent>(player) : nullptr;
}

/// 残っている部位。«分からない» は -1 で返す (IBoss の既定がそう返す)。
/// @note 0 で誤魔化さない。0 として扱うと «脚が 1 本も無い» と読まれ幕 4 が開始と
///       同時に合格してしまうため、分からないことは分からないまま上へ渡す。
inline int TutorialDirectorComponent::PartsLeft() const
{
    GameObject* boss = BossObject();
    const IBoss* iboss = boss ? IBoss::Of(boss) : nullptr;
    return iboss ? iboss->PartsRemaining() : -1;
}

inline void TutorialDirectorComponent::WarnNoParts()
{
    if (m_warnedNoParts) return;
    m_warnedNoParts = true;
    debug.LogError("TutorialDirectorComponent: the boss does not report PartsRemaining "
                   "(IBoss::PartsRemaining returns -1). The lesson gates on legs can never "
                   "be passed. Give the boss a BossCoreComponent, or turn off 'teach'.");
}

inline const TutorialDirectorComponent::Step& TutorialDirectorComponent::Current() const
{
    static const Step kSteps[] = {
        { "さあ、あの巨体へ近づこう！", { kBossMoveNone, false, 0, 0.0f }, true, "", "" },
        { "印の脚を狙って！ 続けて押すと連撃になるよ",
          { kBossMoveNone, false, 0, 0.0f }, true, actions::kAttack, "斬撃" },
        { "足元の光を見て。踏みつけが来るよ！",
          { static_cast<unsigned>(BossMove::Stomp), false, 0, 0.0f }, true, actions::kDodge, "回避" },
        { "長押しでガード！ 左上のスタミナを見てみよう",
          { kBossMoveNone, false, 0, 0.0f }, true, actions::kParry, "ガード" },
        { "当たる瞬間に押して、踏みつけを弾こう！",
          { static_cast<unsigned>(BossMove::Stomp), false, 0, 0.0f }, true, actions::kParry, "弾き" },
        { "弾いたら反撃！ 連撃を3回当てよう",
          { static_cast<unsigned>(BossMove::Stomp), false, 0, 0.0f }, true, actions::kAttack, "連撃" },
        { "弾くと崩しゲージが溜まる。満ちれば転倒するよ！",
          { BossMove::Stomp | BossMove::Pulse, false, 0, 0.0f }, true, actions::kParry, "弾き" },
        { "次の脚を狙おう！ 衝撃波は回避してね",
          { BossMove::Stomp | BossMove::Pulse, false, 0, 0.0f }, true, actions::kParry, "弾き" },
        { "弾いて、崩して、とどめ！ キミならできる！",
          { BossMove::Stomp | BossMove::Pulse, false, 0, 0.0f }, true, actions::kParry, "弾き" },
    };
    static_assert(sizeof(kSteps) / sizeof(kSteps[0])
                      == static_cast<std::size_t>(Act::Free) + 1u,
                  "幕の数と表の行数を揃えること");
    return kSteps[static_cast<int>(m_act)];
}

inline const char* TutorialDirectorComponent::LiveObjective(const BossAiComponent* ai) const
{
    /// @note 予兆が «今フレーム出ているか» と «着弾がすぐそこか»。
    ///       shape が None なら何も来ていない (BossTelegraph.hpp)。
    const BossTelegraph tel = ai ? ai->CurrentTelegraph() : BossTelegraph{};
    const bool incoming = tel.shape != BossTelegraphShape::None;
    const bool imminent = incoming && tel.progress >= 0.55f;

    GameObject*  boss  = BossObject();
    const IBoss* iboss = boss ? IBoss::Of(boss) : nullptr;
    const auto*  brk   = boss ? scene.GetScript<BossBreakComponent>(boss) : nullptr;
    const bool   down  = iboss && iboss->IsToppled();
    if (m_act >= Act::Execute && incoming && tel.kind == BossAttackKind::Pulse)
        return "衝撃波は弾けない！ 回避でかわそう！";
    if (m_act >= Act::Execute && TimeManagerComponent::ParryRushActive())
        return "今は連撃のチャンス！ 印の脚を斬ろう！";

    switch (m_act) {
    case Act::Guard:
        return "長押しでガード！ 構えを保つとスタミナを使うよ";
    case Act::Rush:
        return TimeManagerComponent::ParryRushActive()
            ? "今はキミが速く動ける！ 印の脚へ連撃！"
            : "もう一度弾こう！ 弾いた直後が連撃のチャンス";
    case Act::Slash:
        /// @note 1 発当たったら «繋ぐ» へ言い換える。当てる前に «繋げ» と言っても、
        ///       まだ 1 度も当たっていない相手には何を繋ぐのか分からない。
        if (brk && brk->Ratio() > 0.0f) return "その調子！ 続けて斬りつなごう！";
        return "脚を狙って！ 続けて押すと連撃になるよ";

    case Act::Dodge:
        /// @note 予兆を一度も見ていない相手に «印» と言っても通じない。
        ///       出ていないときに «何が起きるか» を、出ている最中に «今どうするか» を言う。
        if (imminent) return "今だ、横へかわして！";
        if (incoming) return "足元が光った！ そこに落ちてくるよ！";
        return "足元の光を見て。踏みつけが来るよ！";

    case Act::Parry:
        /// @note «押しっぱなしでも防げる» とはここで言わない。この幕は弾き 1 回で通し、
        ///       押しっぱなしのガードでは崩しが溜まらず通過条件の `Parries()` も増えない
        ///       ため、«防げる» と教えると教えたとおりに遊んだ人が永遠に合格しない。
        ///       ガードは死ねるようになる幕 4 で教える。
        if (imminent) return "今だ、弾いて！";
        if (incoming) return "まだだよ…当たる瞬間を狙って！";
        return "当たる瞬間に押して、踏みつけを弾こう！";

    case Act::Execute:
        if (TimeManagerComponent::ParryRushActive()) return "今は連撃のチャンス！ 脚を斬ろう！";
        if (down)  return "印の脚へ近づいて、とどめを決めよう！";
        if (brk && brk->Ratio() >= 0.6f) return "あと少し！ 弾いて体勢を崩そう！";
        if (imminent) return "今だ、弾いて！";
        return "弾くと崩しゲージが溜まる。満ちれば転倒するよ！";

    case Act::Range:
        if (down) return "今がチャンス！ 脚にとどめを！";
        /// @note 守りが外れた最初の一言は «何が変わったか»。手が増えたことより、
        ///       **もう守られていない** ことの方が先に伝わらないといけない。
        if (m_justUnprotected) return "ここからは致命傷に注意！ 次の脚を狙おう！";
        if (incoming) return "踏みつけは弾こう！ 衝撃波は回避しよう！";
        return "危ないときは長押しでガード！ 崩すなら弾こう";

    case Act::Free:
        if (down) return "今がチャンス！ 脚にとどめを！";
        if (ai && ai->IsCrippled()) return "あと少し！ 印の脚を狙って、弾いて崩そう！";
        return Current().objective;

    case Act::Approach:
        break;
    }
    return Current().objective;
}

inline void TutorialDirectorComponent::ClearDim()
{
    if (!m_dimming) return;
    m_dimming = false;
    if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->ClearFade();
}

inline void TutorialDirectorComponent::FireCue(float seconds, float timeScale, bool dims)
{
    m_cue      = std::max(seconds, 0.05f);
    /// @note 長さは控えて持つ。合図は «教える» と «出来た» で尺が違うため、CueStrength を
    ///       いつも cueSeconds で割ると短い方は 1.0 へ届かず中央まで寄り切らない。
    m_cueTotal = m_cue;
    m_cueDims  = dims;
    if (auto* clock = TimeManagerComponent::Instance())
        clock->SlowFor(std::clamp(timeScale, 0.05f, 1.0f), m_cue, 0.04f, 0.18f);
}

inline void TutorialDirectorComponent::FireSuccess()
{
    /// @note 弾きと同じ «白» は使わない。弾きの閃光は «防いだ» の語で 1 戦のあいだ何十回も
    ///       出るため、段を越えた合図が同じ色だといつもの防御に埋もれる。暖かい側へ
    ///       寄せると «いつもと違うことが起きた» が色だけで伝わる。
    if (successFlash > 0.0f)
        if (auto* screen = ScreenEffectManagerComponent::Instance())
            screen->Flash(successFlashColor, std::clamp(successFlash, 0.0f, 1.0f), 0.14f);
    if (successShake > 0.0f)
        if (auto* shake = CameraShakeManagerComponent::Instance())
            shake->Shake(std::clamp(successShake, 0.0f, 1.0f));
    if (successFov > 0.0f)
        if (auto* follow = CameraFollowManagerComponent::Instance())
            follow->PunchFov(std::clamp(successFov, 0.0f, 1.0f));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.25f, 0.55f, 0.16f);
    /// @note 連撃が伸びたときの上がり音。«良いことが起きた» として既に耳が覚えている。
    se::Play(audio, se::kUiComboHigh);

    /// @note 手応えのあいだだけ中央へ出す。教える合図より短く・浅く ─ 長いと
    ///       «褒められている時間» が戦いを止める。
    if (successCueSeconds > 0.0f) FireCue(successCueSeconds, successSlow, /*dims=*/false);
}

inline void TutorialDirectorComponent::DriveCue(const BossAiComponent* ai, float unscaledDt)
{
    /// @note 教える 3 幕でだけ。合格した後 (成功のセリフ の間) は出さない ─ 済んだことを
    ///       もう一度止めて見せるのは、褒めているのか直させたいのか分からなくなる。
    const bool teaching = teach && cueEnabled && !m_passed
                       && (m_act == Act::Dodge || m_act == Act::Parry || m_act == Act::Execute);

    const BossTelegraph tel = ai ? ai->CurrentTelegraph() : BossTelegraph{};
    const bool incoming = tel.shape != BossTelegraphShape::None;

    /// @note 予兆が消えたら次の手のために札を下ろす。**進みではなく «消えたか» で下ろす**
    ///       ─ 進みは手ごとに 0 から数え直すので、同じ手が連続すると下ろし損ねる。
    if (!incoming) m_cueArmed = false;

    if (teaching && incoming && !m_cueArmed && tel.progress >= cueAt
        && m_cueCount < std::max(cueMaxPerAct, 0)) {
        m_cueArmed = true;
        ++m_cueCount;
        FireCue(cueSeconds, cueTimeScale, /*dims=*/true);
    }

    /// @note 倒れた «瞬間» も教える機会。予兆が無いので上の経路では拾えないが、
    ///       とどめを知らない相手にとってはここが一番教えるべき 1 秒になる。立ち上がり
    ///       (変わった瞬間) だけを取る ─ 転倒は 9 秒あるので «倒れている間» で焚くと
    ///       毎フレーム焚き直すことになる。
    {
        GameObject*  boss  = BossObject();
        const IBoss* iboss = boss ? IBoss::Of(boss) : nullptr;
        const bool   down  = iboss && iboss->IsToppled();
        if (down && !m_wasToppled && teach && cueEnabled && m_act == Act::Execute
            && m_cueCount < std::max(cueMaxPerAct, 0)) {
            ++m_cueCount;
            FireCue(cueSeconds, cueTimeScale, /*dims=*/true);
        }
        m_wasToppled = down;
    }

    if (m_cue <= 0.0f) { ClearDim(); return; }
    m_cue = std::max(0.0f, m_cue - unscaledDt);

    /// @note 沈むのは «教える» 合図だけ。出来たときは暖色で閃かせているので、同時に
    ///       沈めると «褒めながら暗くする» という読めない画になる。
    if (!m_cueDims) { ClearDim(); return; }

    /// @note 沈みは合図の強さへ素直に付ける。出るのは速く、引くのはゆっくり。
    const float t = CueStrength();
    const float dim = std::clamp(cueDim, 0.0f, 1.0f) * (t * (2.0f - t));
    if (auto* screen = ScreenEffectManagerComponent::Instance()) {
        screen->SetFade(dim, Vector4{ 0.02f, 0.03f, 0.05f, 1.0f });
        m_dimming = true;
    }
    if (m_cue <= 0.0f) ClearDim();
}

inline void TutorialDirectorComponent::EnterAct(Act act)
{
    m_act    = act;
    m_passed = false;
    m_praise = 0.0f;

    const auto* combat = CombatManagerComponent::Instance();
    const auto* ai     = Ai();
    const auto* player = Player();
    m_baseMoves   = ai ? ai->MoveSerial() : 0;
    m_baseDodges  = player ? player->DodgeSerial() : 0;
    m_baseDamage  = combat ? combat->DamageTaken() : 0;
    m_baseParries = combat ? combat->Parries() : 0;
    m_baseChain   = combat ? combat->BestChain() : 0;
    m_baseParts   = PartsLeft();
    m_baseBladeHits = combat ? combat->BladeHitSerial() : 0;
    m_baseRushHits = combat ? combat->RushHitSerial() : 0;
    m_baseGuardSpent = player ? player->GuardStaminaSpent() : 0.0f;
    /// @note 合図の回数は幕ごとに数え直す。前の幕で使い切った状態で次の «初めて» を
    ///       迎えると、教えるべき瞬間に何も出ない。
    m_cueCount = 0;
    m_cueArmed = false;

    switch (act) {
    case Act::Guard: debugAct = "ガード"; debugGoal = "スタミナ消費を確認"; break;
    case Act::Rush: debugAct = "反撃"; debugGoal = "ラッシュ中に斬撃を3回当てる"; break;
    case Act::Approach: debugAct = "0 踏み込む";  debugGoal = "部屋へ入る";           break;
    case Act::Slash:    debugAct = "1 斬る";      debugGoal = "斬撃を繋ぐ";           break;
    case Act::Dodge:    debugAct = "2 避ける";    debugGoal = "手をやり過ごす";       break;
    case Act::Parry:    debugAct = "3 弾く";      debugGoal = "弾きを 1 回";          break;
    case Act::Execute:  debugAct = "4 崩す";      debugGoal = "脚を 1 本";            break;
    case Act::Range:    debugAct = "5 間合い";    debugGoal = "脚 2 本 (据え付け化)"; break;
    case Act::Free:     debugAct = "6 読む";      debugGoal = "-";                    break;
    }
}

inline bool TutorialDirectorComponent::Passed()
{
    const auto* combat = CombatManagerComponent::Instance();

    switch (m_act) {
    case Act::Approach: {
        /// @note 起きたかどうかを持っているのはボス自身 (BossRoomTriggerComponent)。
        GameObject* boss = BossObject();
        const auto* room = boss ? scene.GetScript<BossRoomTriggerComponent>(boss) : nullptr;
        /// @note 部屋の仕掛けが無い盤面 (検証用シーンなど) は、この幕を素通りさせる。
        ///       ここで止めると «何をしても始まらない» になり、原因は画面に出ない。
        return !room || room->IsEngaged();
    }

    case Act::Slash:
        /// @note 繋いだ最長で見る。«N 回当てた» だと 1 発ずつ間を空けても通るので、
        ///       **押し続けると繋がる** という一番覚えてほしいことが伝わらない。
        debugProgress = combat && combat->BladeHitSerial() > m_baseBladeHits ? combat->ChainCount() : 0;
        return debugProgress >= std::max(slashChain, 1);

    case Act::Dodge: {
        const auto* ai     = Ai();
        const auto* player = Player();
        const int moves  = ai ? ai->MoveSerial() - m_baseMoves : 0;
        const int dodges = player ? player->DodgeSerial() - m_baseDodges : 0;
        const int hurt   = combat ? combat->DamageTaken() - m_baseDamage : 0;
        /// @note 食らったら数え直し。«避けられた手» だけを数えないと、突っ立っていても
        ///       手が出た回数で段が進んでしまう。
        if (hurt > 0) {
            m_baseMoves  = ai ? ai->MoveSerial() : 0;
            m_baseDodges = player ? player->DodgeSerial() : 0;
            m_baseDamage = combat ? combat->DamageTaken() : 0;
            debugProgress = 0;
            return false;
        }
        /// @note 回避の回数も要る。手の数だけを見ていると射程の外に立っているだけで
        ///       合格でき、回避を一度も使わずに «避ける» の幕を抜けられてしまう。
        ///       促しているボタン (プロンプトの «回避») と通す条件を揃える。
        const int need = std::max(surviveMoves, 1);
        debugProgress = std::min(moves, dodges);
        return moves >= need && dodges >= need;
    }

    case Act::Guard: {
        const auto* player = Player();
        return player && player->GuardStaminaSpent() - m_baseGuardSpent
            >= std::max(player->MaxStamina() * 0.08f, 0.01f) && m_shownGuardFocus;
    }
    case Act::Rush:
        debugProgress = combat ? combat->RushHitSerial() - m_baseRushHits : 0;
        return debugProgress >= 3;

    case Act::Parry:
        debugProgress = combat ? combat->Parries() - m_baseParries : 0;
        return debugProgress >= 1;

    case Act::Execute: {
        /// @note 脚が 1 本減った ＝ 崩して倒し、とどめまで通った。
        const int left = PartsLeft();
        if (left < 0 || m_baseParts < 0) { WarnNoParts(); return false; }
        debugProgress = m_baseParts - left;
        return debugProgress >= 1;
    }

    case Act::Range: {
        /// @note 据え付け化した後も、最後の脚まで案内を続ける。
        const int left = PartsLeft();
        if (left < 0) { WarnNoParts(); return false; }
        debugProgress = left;
        return left <= 2;
    }

    case Act::Free:
        return false;
    }
    return false;
}

inline void TutorialDirectorComponent::Advance()
{
    if (m_act >= Act::Free) return;
    EnterAct(static_cast<Act>(static_cast<int>(m_act) + 1));
}


inline void TutorialDirectorComponent::ClearLessonPresentation()
{
    m_focusRemaining = 0.0f;
    if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->SetTutorialFocus(0.0f);
    for (auto& panel : m_focusPanels)
        if (auto* object = panel.Resolve(scene)) ui.SetImageColor(object, { 0, 0, 0, 0 });
    if (auto* object = m_focusTrace.Resolve(scene)) ui.SetImageColor(object, { 1, 0.8f, 0.3f, 0 });
    if (auto* object = m_focusCaption.Resolve(scene)) ui.SetTextColor(object, { 1, 1, 1, 0 });
    if (auto* object = m_legMarker.Resolve(scene)) object->SetActive(false);
}

inline void TutorialDirectorComponent::DriveStaminaFocus(float dt)
{
    const auto* player = Player();
    if (!player || !player->enabled) { ClearLessonPresentation(); return; }
    m_focusRemaining = std::max(0.0f, m_focusRemaining - dt);
    const float dodgeSpent = player->DodgeStaminaSpent();
    const float guardSpent = player->GuardStaminaSpent();
    const bool dodge = m_act == Act::Dodge && !m_shownDodgeFocus && dodgeSpent > m_seenDodgeSpent;
    const bool guard = m_act == Act::Guard && !m_shownGuardFocus
        && guardSpent - m_baseGuardSpent >= std::max(player->MaxStamina() * 0.005f, 0.001f);
    if (!StaminaFocusActive() && (dodge || guard)) {
        m_focusGuard = guard;
        if (guard) m_shownGuardFocus = true;
        else m_shownDodgeFocus = true;
        m_focusBefore = m_previousStamina;
        m_focusRemaining = 2.4f;
        m_cue = 0.0f;
        ClearDim();
        if (auto* clock = TimeManagerComponent::Instance()) clock->SlowFor(0.2f, 2.4f, 0.04f, 0.2f);
    }
    m_seenDodgeSpent = dodgeSpent;
    m_seenGuardSpent = guardSpent;
    m_previousStamina = player->NormalizedBreath();
    const float weight = Clamp01(m_focusRemaining / 0.3f);
    if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->SetTutorialFocus(weight);
    for (auto& panel : m_focusPanels)
        if (auto* object = panel.Resolve(scene)) ui.SetImageColor(object, { 0.0f, 0.0f, 0.0f, 0.68f * weight });
    if (auto* trace = m_focusTrace.Resolve(scene)) {
        ui.SetImageColor(trace, { 1.0f, 0.8f, 0.3f, weight });
        ui.SetImageFillAmount(trace, m_focusBefore);
    }
    if (auto* caption = m_focusCaption.Resolve(scene)) {
        ui.SetText(caption, m_focusGuard ? "ガード中は消費 / 離すと回復"
            : player->NormalizedBreath() >= 0.999f ? "回避で消費 / ジャスト回避は全回復"
            : "回避で消費 / 金色は消費前の長さ");
        ui.SetTextColor(caption, { 1.0f, 0.87f, 0.4f, weight });
    }
}

inline void TutorialDirectorComponent::OnLateUpdate()
{
    auto* marker = m_legMarker.Resolve(scene);
    if (!marker) return;
    auto* boss = BossObject();
    const auto* rig = boss ? scene.GetScript<BossRigComponent>(boss) : nullptr;
    const auto* iboss = boss ? IBoss::Of(boss) : nullptr;
    auto* player = scene.FindWithTag("Player", true);
    const bool show = teach && player && rig && iboss && PartsLeft() > 0
        && !StaminaFocusActive() && (m_act == Act::Slash || m_act >= Act::Rush);
    marker->SetActive(show);
    if (!show) return;
    Vector3 anchor;
    if (m_markedLeg < 0 || rig->IsLegBroken(m_markedLeg) || !rig->LegAnchor(m_markedLeg, anchor)) {
        m_markedLeg = -1;
        float nearest = 1.0e30f;
        for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
            Vector3 candidate;
            if (rig->IsLegBroken(leg) || !rig->LegAnchor(leg, candidate)) continue;
            const Vector3 delta = candidate - player->transform.worldPosition;
            const float distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (distance < nearest) { nearest = distance; m_markedLeg = leg; anchor = candidate; }
        }
    }
    if (m_markedLeg < 0) { marker->SetActive(false); return; }
    anchor.y += 1.4f;
    marker->transform.position = anchor;
    marker->transform.worldPosition = anchor;
    if (auto* caption = m_legCaption.Resolve(scene)) {
        const bool rush = TimeManagerComponent::ParryRushActive();
        ui.SetText(caption, rush ? "この脚へ連撃！" : iboss->IsToppled() && m_act >= Act::Execute ? "この脚にとどめ！" : "狙う脚");
        ui.SetTextColor(caption, { 1.0f, 0.87f, 0.35f, 1.0f });
    }
}

inline void TutorialDirectorComponent::OnStart()
{
    s_instance = this;
    const char* panelNames[] = { "HUD_StaminaFocusTop", "HUD_StaminaFocusLeft",
                                "HUD_StaminaFocusRight", "HUD_StaminaFocusBottom" };
    for (int i = 0; i < 4; ++i)
        if (auto* object = scene.Find(panelNames[i], true)) m_focusPanels[i] = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("HUD_StaminaTrace", true)) m_focusTrace = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("HUD_StaminaFocusCaption", true)) m_focusCaption = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("TutorialLegMarker", true)) m_legMarker = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("TutorialLegCaption", true)) m_legCaption = EntityRef{ object->GetID() };
    ClearLessonPresentation();
    if (const auto* player = Player()) {
        m_seenDodgeSpent = player->DodgeStaminaSpent();
        m_seenGuardSpent = player->GuardStaminaSpent();
        m_previousStamina = player->NormalizedBreath();
    }
    m_objective.clear();
    m_wasToppled = false;
    /// @note 段を越えた合図は «画面の出来事» なので UI バスの 2D で鳴らす (GameFlow と同じ)。
    se::EnsureSource(scene, "UI");
    EnterAct(Act::Approach);
    if (!teach) EnterAct(Act::Free);
}

inline void TutorialDirectorComponent::OnUpdate()
{
    auto* ai = Ai();
    auto* clock = TimeManagerComponent::Instance();
    if (clock && clock->IsPaused()) return;
    if (!teach) {
        if (auto* flow = Flow()) flow->BeginBattleBgm();
        ClearLessonPresentation();
        if (ai) ai->ClearMoveGate();
        return;
    }
    DriveStaminaFocus(std::max(time.UnscaledDeltaTime(), 0.0f));
    if (m_act == Act::Rush && clock && clock->IsParryRush()) {
        const auto* combat = CombatManagerComponent::Instance();
        if (combat && combat->Parries() != m_extendedParry) {
            m_extendedParry = combat->Parries();
            clock->EnsureParryRushSeconds(4.0f);
        }
    }

    /// @note 門は毎フレーム渡し直す。1 度きりにすると、ボスが眠っていて AI をまだ引けない
    ///       最初の数フレームで渡し損ね、そのまま «絞っていない» 状態で戦いが始まる
    ///       (BossRoomTrigger が «まだ» を毎フレーム押し直しているのと同じ理由)。
    if (ai) {
        BossMoveGate gate = teach ? Current().gate : BossMoveGate{};
        /// @note テンポは表に書かず、Inspector の 2 つから引く。幕ごとに数字を撒くと、
        ///       遅さを 0.05 直すのに表の 3 行を触ることになる。
        if (m_act == Act::Dodge || m_act == Act::Parry) gate.tempo = teachTempo;
        else if (m_act == Act::Execute)                 gate.tempo = executeTempo;
        if (teach) {
            gate.reactions = false;
            gate.chainMax = 0;
            gate.recoverySeconds = 2.6f;
            if (m_act >= Act::Range) gate.allow = BossMove::Stomp | BossMove::Pulse;
            if (m_act >= Act::Rush) gate.tempo = executeTempo;
            if (StaminaFocusActive() || m_passed) gate.allow = kBossMoveNone;
            gate.holdPosition = gate.allow == kBossMoveNone
                || (m_act == Act::Rush && TimeManagerComponent::ParryRushActive());
        }
        ai->SetMoveGate(gate);
    }

    /// @note 守りも毎フレーム。押し続けている間だけ効く «1 フレームぶんの要求»。
    const bool protect = protectWhileTeaching && teach;
    /// @note 守りが «外れた» ことは、手が増えたことより先に伝えないといけない。
    ///       黙って外すと、次の 1 回の被弾が «理不尽» として読まれる。
    if (m_wasProtected && !protect) m_justUnprotected = 3.0f;
    m_wasProtected = protect;
    m_justUnprotected = std::max(0.0f, m_justUnprotected - std::max(Time::deltaTime, 0.0f));

    debugProtected = protect;
    if (protect)
        if (auto* player = Player()) player->RequestDamageFloor(1);

    /// @note 合格の判定 → «出来た» を読ませる間 → 次の幕。
    if (!m_passed && !StaminaFocusActive() && Passed()) {
        m_passed = true;
        m_praise = std::max(praiseSeconds, 0.0f);
        FireSuccess();
        if (m_act == Act::Parry) Advance();
    }
    if (m_passed) {
        m_praise -= std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_praise <= 0.0f) Advance();
    }

    /// @note 目的の 1 行。幕 5 だけは盤面の状態で言い換える ── 教えるのをやめた後も
    ///       «今どうすればいいか» は返し続ける (通しで教える、という方針)。
    m_objective = m_passed && m_act < Act::Free ? SuccessLine() : LiveObjective(ai);

    /// @note «ここだ» の層。文言を決めた後に回す ─ 合図の最中は、この 1 行がそのまま
    ///       画面の真ん中へ出る (PromptLabel)。
    if (!StaminaFocusActive()) DriveCue(ai, std::max(time.UnscaledDeltaTime(), 0.0f));

    if (auto* flow = Flow()) {
        /// @note 上の 1 行と中央は別のことを言う。合図の最中は中央へ «今だ、横へ逃げろ»
        ///       が出るため、上にも同じ文を出すと情報が増えず中央から目線が割れる。
        ///       合図の間、上は «この幕で何を覚えるのか» という据え置きの説明に戻す。
        flow->SetObjective(m_cue > 0.0f && !m_passed ? Current().objective : m_objective);
        /// @note 守りが外れて手が全部出るところが «ここからが本番»。曲を持っているのは
        ///       盤面 (GameFlow の BGM 欄) なので、こちらは境目だけを告げる。幕が変わった
        ///       瞬間でなく毎フレーム渡す ─ 進行は別の子オブジェクトに居てどちらの
        ///       OnStart が先に走るかはシーンの並び次第で、1 度きりだと «先に言って
        ///       しまって届かない» が起こる (門を毎フレーム渡し直すのと同じ)。
        if (m_act >= Act::Range) flow->BeginBattleBgm();
    }
}

} // namespace sandbox
