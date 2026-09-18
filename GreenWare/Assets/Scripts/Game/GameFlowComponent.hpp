/// @file    GameFlowComponent.hpp
/// @brief   敵全滅・プレイヤー死亡・HUD・リザルト遷移といったゲーム進行を統括する
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 戦闘の解決 (ダメージ式) は `CombatManagerComponent` の担当。ダメージ式は頻繁に
///       変わるが、ここが持つ「いつリザルトへ行くか」はほとんど変わらないため、
///       戦果は取得用の API で受け取り、リザルトへ渡すだけにする。
#pragma once
#include <Scripts/Utils/PlayerActionState.hpp>

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/ArenaHazardComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/UI/StageProgressState.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class GameFlowComponent : public Script {
    FBZZ_SCRIPT(GameFlowComponent)

public:
    /// @note 型 (IBoss) だけで探すと、参照が外れたときの症状が «倒しても何も起きない»
    ///       で進行側も黙るため、明示の割り当てを先に見て空のときだけ型で探す
    ///       (BossHealthBarComponent と同じ形)。
    FBZZ_GROUP("ボス")
    FBZZ_REF(GameObject, bossObject, "ボス")
    FBZZ_TOOLTIP("これを倒したらクリア。未設定なら IBoss を実装したオブジェクトを盤面から探す")

    FBZZ_GROUP("シーン")
    FBZZ_FIELD_RANGE_INT(int, stageNumber, 1, "採点するステージ番号", 1, 7)
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")
    FBZZ_FIELD_RANGE(float, endDelay, 0.8f, "End Delay", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, defeatDelay, 3.6f, "死亡演出の秒数", 2.0f, 8.0f)
    /// @note ボス撃破だけ別の待ちを持つ。ボスは崩れ切るまでに数秒かかる
    ///       (BossDeathVfxComponent) ため、雑魚戦と同じ値では «消える前に画面が変わる»
    ///       か «終わりが間延びする» のどちらかになる。敗北はここに載せない
    ///       (待たせている間は倒れたプレイヤーしか映らないので短いほどよい)。
    FBZZ_FIELD_RANGE(float, bossEndDelay, 5.0f, "Boss End Delay", 0.0f, 12.0f)
    FBZZ_TOOLTIP("ボスを倒したときだけ使う待ち。撃破演出が終わるより短くすると、"
                 "崩れている途中でリザルトへ切り替わる。ボスの EnemyHealthComponent の "
                 "Destroy Delay はこれより長くしておくこと (先に消える)")

    /// @note 体力の表示は持たない。`PlayerHealthBarComponent` がバーとして出しており、
    ///       同じ値を文字にすると片方の書式・色を変えたときもう片方が取り残される。
    FBZZ_GROUP("HUD Names")
    FBZZ_FIELD(std::string, objectiveTextName, "HUD_Objective", "Objective Text")

    /// @brief 文言が変わった «瞬間» だけ解読で出し直す。
    /// @note 目的の 1 行は出しっぱなしなので、差し替わっても気付かれないのを防ぐため
    ///       音も閃光も足さずに解読で目を向けさせる (タイトル / OPTIONS と同じ語彙)。
    /// @note 中央の命令には掛けない。あちらは 0.8 秒で読ませるのが仕事で、確定まで
    ///       掛かる演出は読みやすさを削るだけになる。
    /// @note 対象の UIText は `richText = true` にしておくこと (UiTextFx.hpp の約束)。
    FBZZ_FIELD_RANGE(float, objectiveDecodeSeconds, 0.35f, "解読 [s]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("0 で解読しない (差し替えるだけ)。長いと «読めるまで待たされる»")
    FBZZ_FIELD_COLOR(objectiveColor, (Vector4{ 0.92f, 0.95f, 1.0f, 1.0f }), "確定した字")
    FBZZ_FIELD_COLOR(objectiveHotColor, (Vector4{ 1.0f, 0.86f, 0.55f, 1.0f }), "確定する瞬間")
    FBZZ_FIELD_COLOR(objectiveScrambleColor, (Vector4{ 0.38f, 0.42f, 0.50f, 1.0f }), "未確定")

    /// @note 環境音は進行側が持つ。«この場所に居る» は始まりと終わりを進行自身が知る
    ///       状態で、専用 GameObject を置く形だとシーンから消しただけで無音になり
    ///       気付けない。
    FBZZ_GROUP("Ambience")
    FBZZ_FIELD_RANGE(float, ambienceVolume, 0.30f, "Arena", 0.0f, 1.0f)
    FBZZ_TOOLTIP("アリーナの環境音の音量。0 で鳴らさない")

    /// @note 曲は `BgmLibrary` に置かない。ステージの曲は盤面ごとに «開幕» と «本番» の
    ///       2 つがあるため、表に持たせると盤面を足すたびにコードを触ることになる。
    FBZZ_GROUP("BGM")
    FBZZ_FIELD_AUDIO(stageBgm, "", "開幕")
    FBZZ_TOOLTIP("盤面に入った時点で鳴らす曲。空なら鳴らさない")
    FBZZ_FIELD_AUDIO(battleBgm, "", "本番")
    FBZZ_TOOLTIP("«ここからが本番» を告げられたときへ掛け替える曲 (BeginBattleBgm)。"
                 "空なら開幕の曲のまま ─ 最初から本番の盤面 (Stage_02) はそれでよい")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "経過")
    FBZZ_FIELD_READ_ONLY(std::string, debugBoss, "", "ボスのオブジェクト")
    FBZZ_TOOLTIP("勝利条件として見ている相手。空ならボスがまだ眠っている")
    FBZZ_FIELD_READ_ONLY(std::string, debugOutcome, "-", "Outcome")

    /// 目的の 1 行を外から差す。空文字で進行側の文言へ戻る。
    /// @note 同じ 1 行へ複数の書き手 (チュートリアルの段表示など) が毎フレーム書くと
    ///       走行順でちらつくため、持ち主を 1 人に決められる口を置く。
    /// @note 決着 (AREA CLEAR / SYSTEM DOWN) だけは進行側が奪い返す。«盤面が終わった»
    ///       は段の話ではなく、外に任せたままだと終わっても段の文言が残る。
    void SetObjective(const std::string& text) { m_objective = text; }

    /// «ここからが本番» を告げる。掛け替える曲が無ければ何も起きない。
    /// @note きっかけだけを受け取る。いつ本番へ入るかを知るのは教える側だが、どの曲かは
    ///       盤面の持ち物なので、曲名まで教える側に持たせない。
    /// @note 何度呼ばれてもよい (`bgm::Play` は同じ宣言なら何もしない)。呼ぶ側は
    ///       «自分の OnStart が進行より先か後か» を気にしなくて済む。
    void BeginBattleBgm()
    {
        if (m_ending || battleBgm.empty()) return;
        bgm::Play(audio, battleBgm);
    }

    void OnStart() override;
    void OnUpdate() override;
    void OnDisable() override { m_ambience.Stop(*this); }

private:
    void BeginEnd(bool victory);
    void RefreshHud();
    /// 決着からの残り時間ぶんだけ環境音を残す。待ちが尽きると 0 になる。
    void FadeOutAmbience();

    /// HUD と待ち時間が «代表» として読む 1 体。生死を決めるのはこちらではない。
    [[nodiscard]] GameObject* FindBoss() const;

    /// 盤面のボスを数える。out に «まだ倒していない相手» の数を返す。
    /// @return 盤面に立っているボスの数。0 なら勝ち負けを決める相手が居ない。
    /// @note Stage_02 は蛇を 2 体置くため数で持つ。1 体だけ見て終わりを決めると、
    ///       先に倒れた方でステージが閉じ、もう 1 体が生きたままリザルトへ抜ける。
    [[nodiscard]] int CountBosses(int& outAlive) const;

    /// 盤面の «開いている / 閉じている» をボスの交戦状態へ合わせる。
    /// @note 進行がここを持つ。各スクリプトが自分で «ボスは起きたか» を見に行くと、
    ///       同じ問いの答えが盤面に複数できる。
    void SyncBoard(bool fighting) const;

    /// @note 保持せず毎回引く。マネージャーは別の子オブジェクトに居るため、開始時に
    ///       1 度掴んで持ち続けると並び順を変えただけで戦果が丸ごと 0 になる。
    [[nodiscard]] CombatManagerComponent* Combat() const
    {
        return CombatManagerComponent::Instance();
    }

    PlayerComponent* m_player = nullptr;
    /// 「戦闘が居ない」の報告口。最初のフレームだけ空なのは並び順の都合なので、
    /// 猶予を過ぎても見つからないときだけ出す (ManagerWatch.hpp)。
    ManagerWatch m_combatWatch;
    /// ボスが 1 体も名乗っていない盤面を報告するための猶予。
    ManagerWatch m_bossWatch;
    /// 「ボスに体力が付いていない」を 1 度だけ言うためのラッチ。
    bool m_warnedNoBossHealth = false;
    /// 「リザルトへ移れない」を 1 度だけ言うためのラッチ。
    bool m_warnedNoResultScene = false;
    /// 外から差された目的の 1 行。空なら進行側の文言を出す。
    std::string m_objective;
    /// 今 HUD に出している文言と、解読の残り [実時間 秒]。
    std::string m_shownObjective;
    float       m_decode = 0.0f;
    bool m_ending = false;
    bool m_victory = false;
    float m_endRemaining = 0.0f;
    /// 決着からリザルトまでの長さ。音を «どこまで下げたか» の分母になる。
    float m_endDuration = 0.0f;
    bool m_resultCaptured = false;
    float m_elapsed = 0.0f;
    /// アリーナの環境音。進行の一発ものと同じ口から出すと、鳴らすたびに環境音が切れる。
    se::LoopVoice m_ambience;
};

FBZZ_REFLECT(GameFlowComponent)

inline void GameFlowComponent::OnStart()
{
    GameResultState::stageIndex = std::clamp(stageNumber - 1, 0, kStageCount - 1);
    StageProgressState::cursor = GameResultState::stageIndex;
    m_resultCaptured = false;
    GameResultState::victory = false;
    m_elapsed = 0.0f;
    m_ending = false;
    m_warnedNoBossHealth = false;
    m_warnedNoResultScene = false;
    debugBoss.clear();
    debugOutcome = "-";

    /// @note 進行の音は画面の出来事であって空間の出来事ではないので UI バスの 2D。
    se::EnsureSource(scene, "UI");
    /// @note Wave 制を畳んだので «Wave 開始» は無い。ステージの始まりはボス部屋の扉が開く音。
    se::Play(audio, se::kUiGateOpen);

    /// @note 環境音は «場所を持たない» 音。3D で鳴らすと、進行スクリプトが置かれた座標が
    ///       アリーナの中心だという前提が要る上に、プレイヤーがそこから離れるほど
    ///       «アリーナに居る» が薄れるという逆立ちが起きる。
    m_ambience.SetKey("ArenaAmbience");
    m_ambience.SetOutput("SE", 0.0f);
    m_ambience.Update(*this, se::kEnvArenaAmb.First(), ambienceVolume);

    bgm::Play(audio, stageBgm);
    /// @note 本番の曲は «戦いの最中» に掛け替わる。mp3 の展開は同期なので、そのとき初めて
    ///       読むと弾いている最中に 1 回だけ引っ掛かる。扉 (ワイプ) の裏で先に読んでおく。
    if (!battleBgm.empty() && !audio.Preload(battleBgm))
        debug.LogWarning("GameFlowComponent could not preload the battle BGM '" + battleBgm
                         + "' (the track will be decoded mid-fight, or not play at all).");

    m_combatWatch.Reset();
    m_bossWatch.Reset();

    if (GameObject* player = scene.FindWithTag("Player")) {
        m_player = scene.GetScript<PlayerComponent>(player);
        if (m_player)
            m_player->SetOnDeath([this]() { BeginEnd(false); });
    }

    if (!m_player)
        debug.LogError("GameFlowComponent requires a PlayerComponent on the Player-tagged object.");
}

inline GameObject* GameFlowComponent::FindBoss() const
{
    /// @note 明示の割り当てでも «出ている» ことは条件。畳まれている間まで勝利条件に据えると、
    ///       まだ動いていないボスの HP を 0 と読んでしまう (理由は FindBossOnBoard を参照)。
    if (GameObject* assigned = bossObject.Get())
        return assigned->activeInHierarchy() ? assigned : nullptr;

    return FindBossOnBoard(scene);
}

inline int GameFlowComponent::CountBosses(int& outAlive) const
{
    outAlive = 0;

    std::vector<GameObject*> bosses;
    CollectBossesInStage(scene, bosses);
    for (GameObject* boss : bosses) {
        const auto* health = scene.GetScript<EnemyHealthComponent>(boss);
        /// @note 体力が読めない相手は «生きている» 側に数える。死んだ側へ倒すと、組み方が
        ///       壊れているだけの盤面が開始と同時にクリアしてしまう (下で名指しする)。
        if (!health || health->IsAlive()) ++outAlive;
    }
    return static_cast<int>(bosses.size());
}

inline void GameFlowComponent::SyncBoard(bool fighting) const
{
    /// @note 中央の焼けはボス戦の一部。開幕から作動していると、踏み込む前から
    ///       «近づけない場所» ができて、部屋の中がどう見えるかが変わってしまう。
    if (auto* hazard = ArenaHazardComponent::Instance()) hazard->SetHazardActive(fighting);
}

inline void GameFlowComponent::BeginEnd(bool victory)
{
    if (m_ending) return;
    m_ending = true;
    m_victory = victory;
    if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->SetDefeat(!victory);
    m_endRemaining = victory ? std::max(FindBoss() ? bossEndDelay : endDelay, 0.0f)
                             : std::max(defeatDelay, 2.0f);
    m_endDuration  = m_endRemaining;
    debugOutcome = victory ? "Victory" : "Defeat";

    /// @note 決着はリザルト画面へ移る前に鳴らす。endDelay の間、画面には結果が出ているのに
    ///       音だけシーン遷移まで来ない、という間ができないようにする。
    se::Play(audio, victory ? se::kUiGameClear : se::kUiGameOver);

    /// @note 曲は止めず «引く»。崩れ落ちる数秒で戦いの曲を断つと、断った瞬間が撃破の
    ///       一撃より目立つ出来事になる。
    /// @note 下げ切る長さは待ち (bossEndDelay / endDelay) に合わせる。固定長にすると
    ///       長い方は無音が残り短い方は曲がそのままリザルトへ入るため。
    bgm::Stop(audio, std::max(m_endRemaining, bgm::kStopFade));

    /// @note 締めの曲は待ちの裏で読んでおく。mp3 の展開は同期なので、リザルトが読んでから
    ///       鳴らすと «画は出ているのに音だけ後から来る» になる (battleBgm の Preload と同じ理由)。
    (void)audio.Preload(victory ? bgm::kResultClear : bgm::kResultFailed);

    FadeOutAmbience();
}

inline void GameFlowComponent::FadeOutAmbience()
{
    /// @note 残り時間をそのまま音量にする。0 まで来ると LoopVoice が自分で止める。
    const float remain = m_endDuration > 0.0f
        ? std::clamp(m_endRemaining / m_endDuration, 0.0f, 1.0f)
        : 0.0f;
    m_ambience.Update(*this, se::kEnvArenaAmb.First(), ambienceVolume * remain);
}

inline void GameFlowComponent::RefreshHud()
{
    if (GameObject* text = scene.Find(objectiveTextName)) {
        /// @note 目的は «削り切る» ではなく «弾いて崩し、とどめで脚を落とす»。倒れている間
        ///       だけ言い方を変える。キー名は書かない ─ 弾きの割り当ては差し替えられる
        ///       (OPTIONS の parry 行) ため、どのキーかは OPTIONS / UiHintBar に任せる。
        ///       登攀は撤去済みで、決着は脚 4 本 (Docs/climb-core.md)。
        const char* objective = "PARRY  >  BREAK  >  EXECUTE";
        /// @note 2 体居る盤面では «どちらかが倒れている» で言い切る。代表の 1 体だけを見ると、
        ///       倒れていない方が代表になっているあいだ、開いた窓が画面に出ない。
        if (!m_ending) {
            std::vector<GameObject*> bosses;
            CollectBossesOnBoard(scene, bosses);
            for (const GameObject* boss : bosses) {
                const auto* iboss = IBoss::Of(boss);
                if (iboss && iboss->IsToppled()) {
                    objective = iboss->UsesBodyHitbox() ? "IT'S DOWN!  ATTACK!" : "IT'S DOWN!  EXECUTE";
                    break;
                }
            }
        }
        const std::string line =
            m_ending ? std::string(m_victory ? "AREA CLEAR" : "SYSTEM DOWN")
                     : (m_objective.empty() ? std::string(objective) : m_objective);

        /// @note 差し替わった瞬間だけ解読を焚く。
        if (line != m_shownObjective) {
            m_shownObjective = line;
            m_decode = std::max(objectiveDecodeSeconds, 0.0f);
            /// @note 解読しない設定なら、ここで 1 度だけ素の文字を置いて終わり。
            if (m_decode <= 0.0f) ui.SetText(text, line);
        }

        if (m_decode <= 0.0f) return;

        /// @note 実時間で数える。教える合図は世界を 0.35 倍まで落とすため、スケール時間だと
        ///       解読が 3 倍近く伸びて «読めない字» が居座る。
        m_decode = std::max(0.0f, m_decode - std::max(Time::unscaledDeltaTime, 0.0f));
        const float total = std::max(objectiveDecodeSeconds, 0.01f);
        const float p     = 1.0f - m_decode / total;

        /// @note 頂点色は UIText.color に掛かる。色は文字列の側で組むので、こちらは白 + α
        ///       にしておく (UiTextFx.hpp の «絶対色で書く» の約束)。
        ui.SetTextColor(text, { 1.0f, 1.0f, 1.0f, objectiveColor.w });
        ui.SetText(text, textfx::Decode(
            m_shownObjective, p,
            static_cast<std::uint32_t>(m_shownObjective.size()) * 2654435761u,
            static_cast<std::uint32_t>(Time::unscaledTime * 30.0f),
            objectiveColor, objectiveHotColor, objectiveScrambleColor));

        /// @note 解読し切ったら素の文字へ戻す。リッチテキストのまま置きっぱなしにすると、
        ///       1 行ぶんのタグを毎フレーム組み直す必要が無いのに組み直し続けることになる。
        if (m_decode <= 0.0f) {
            ui.SetTextColor(text, objectiveColor);
            ui.SetText(text, m_shownObjective);
        }
    }
}

inline void GameFlowComponent::OnUpdate()
{
    /// @note 戦闘が居ないと敵は永遠に減らず、勝利条件が来ないため 1 度だけ名指しで報告する。
    ///       初回フレームでは判定しない。CombatManager がこのスクリプトより後ろに並ぶと
    ///       最初の OnUpdate 時点でまだ Instance() が空になるため、猶予は ManagerWatch が持つ。
    if (m_combatWatch.ShouldReport(Combat() != nullptr)) {
        debug.LogError("GameFlowComponent found no CombatManagerComponent in the scene "
                       "(no damage is applied and the wave never clears).");
    }

    const auto* clock = TimeManagerComponent::Instance();
    if (!m_ending && FindBoss() && !cutscene::HoldsPlayer(fbzz::Time::unscaledTime) &&
        (!clock || (!clock->IsPaused() && !clock->HasOverride())))
        m_elapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
    debugElapsed = m_elapsed;

    /// @note 終わりを決めるのはボスの生死だけ。«居る» と «戦っている» は別で、部屋へ
    ///       入る前のボスは立っているだけで相手ではない (FindBossOnBoard を参照)。
    GameObject* boss = FindBoss();
    debugBoss = boss ? boss->name : std::string{};

    /// @note 決着が付いた後まで盤面を開けておくと、崩れているボスの周りで床が焼け続ける。
    SyncBoard(boss != nullptr && !m_ending);

    /// @note ボスが 1 体も名簿に載っていない盤面は «勝ちようがない» ため 1 度だけ報告する
    ///       (IBoss.hpp を参照)。
    if (!m_ending && m_bossWatch.ShouldReport(StageHasBoss(scene))) {
        debug.LogError("GameFlowComponent: no IBoss is registered on this board. "
                       "The stage can never be cleared. Each boss must call "
                       "IBoss::Bind(scene.Self(), this) in OnStart.");
    }

    if (!m_ending) {
        /// @note 生死を持っているのは HP の側 (IBoss.hpp が HP を重ねない理由)。
        ///       盤面の全員が倒れて初めて決着。1 体でも立っていれば戦闘は続く。
        int       alive = 0;
        const int count = CountBosses(alive);
        if (count > 1)
            debugBoss += "  (" + std::to_string(alive) + "/" + std::to_string(count) + ")";
        if (count > 0 && alive == 0) BeginEnd(true);

        /// @note 体力が読めないときは «倒した» 扱いにしない。成立しない側へ倒し
        ///       (CountBosses)、以後この盤面が開始と同時にクリアしないようにする。
        if (boss && !m_warnedNoBossHealth && !scene.GetScript<EnemyHealthComponent>(boss)) {
            m_warnedNoBossHealth = true;
            debug.LogError("GameFlowComponent found the boss but no EnemyHealthComponent "
                           "on it (defeating it can never end the stage).");
        }
        /// @note ボスの居ない盤面に «勝ち» は無い。«敵 0 なら勝ち» を残すと、盤面が
        ///       空の間ずっと成立してしまう。
    }
    RefreshHud();

    if (!m_ending) return;
    if (!m_resultCaptured) {
        const auto* combat = Combat();
        GameResultState::bossHealthRemaining = GameResultState::bossHealthTotal = 0;
        GameResultState::bossPhase = 1;
        std::vector<GameObject*> resultBosses;
        CollectBossesInStage(scene, resultBosses);
        for (GameObject* resultBoss : resultBosses) {
            if (const auto* health = scene.GetScript<EnemyHealthComponent>(resultBoss)) {
                GameResultState::bossHealthRemaining += health->Current();
                GameResultState::bossHealthTotal += health->MaxHealth();
            }
            if (const auto* identity = IBoss::Of(resultBoss))
                GameResultState::bossPhase = std::max(GameResultState::bossPhase, identity->CurrentPhase());
        }
        GameResultState::bossHpRemain01 = GameResultState::bossHealthTotal > 0
            ? static_cast<float>(GameResultState::bossHealthRemaining) / GameResultState::bossHealthTotal : 0.0f;
        GameResultState::victory = m_victory;
        GameResultState::clearSeconds = m_elapsed;
        GameResultState::defeatedEnemies = combat ? combat->Kills() : 0;
        /// @note ランク評価の 3 軸のうち、時間以外の 2 つ。攻め (途切れなかった斬撃) と
        ///       守り (受けた量) を別々に運ぶ (Docs/game-flow.md「評価とランク」)。
        GameResultState::bestChain     = combat ? combat->BestChain() : 0;
        GameResultState::damageTaken   = combat ? combat->DamageTaken() : 0;
        GameResultState::perfectDodges = combat ? combat->PerfectDodges() : 0;
        GameResultState::parries = combat ? combat->Parries() : 0;
        GameResultState::perfectCadences = combat ? combat->PerfectCadences() : 0;
        m_resultCaptured = true;
    }
    if (!clock || !clock->IsPaused())
        m_endRemaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
    /// @note 曲は AudioManager 側で下がっていくが、環境音は毎フレーム書かないと下がらない。
    FadeOutAmbience();
    if (m_endRemaining > 0.0f) return;


    /// @note 戻り値を見る。読み込めないと «決着はついたのに何も起きない» で止まるため、
    ///       1 度だけ名指しで言い以後は試み続ける (アセットを直せばその場で復帰する)。
    ///       塗っている最中 (`transition::Active()`) は毎フレーム来ても何もしない。
    if (transition::Active()) return;
    if (!transition::Begin(resultScene, true) && !m_warnedNoResultScene) {
        m_warnedNoResultScene = true;
        debugOutcome = "Result scene missing";
        debug.LogError("GameFlowComponent could not load the result scene '" + resultScene
                       + "' (the stage is over but the screen never changes).");
    }
}

} // namespace sandbox
