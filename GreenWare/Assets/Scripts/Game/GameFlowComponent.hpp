/// @file    GameFlowComponent.hpp
/// @brief   敵全滅・プレイヤー死亡・HUD・リザルト遷移といったゲーム進行を統括する
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 戦闘の解決を持たないか:
///   衝突をダメージへ変換するのは CombatManagerComponent の担当。ダメージ式は 18.2 が
///   未決で何度も触るのに対し、ここが持つ「いつリザルトへ行くか」はほとんど変わらない。
///   同居させると、片方を触るたびにもう片方を読む必要が出る。
///   戦果 (撃破数・衝突回数) は取得用の API で受け取り、リザルトへ渡すだけにする。
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
    // WHY 勝利条件の «相手» を明示で持てるようにするか:
    //   ボスは型 (IBoss) で探せば足りるはずだが、この参照が外れたときの症状は
    //   «倒しても何も起きない» で、しかも進行側は何も言わない。勝ち負けが決まらない
    //   のは盤面で最も重い壊れ方なのに、原因は画面のどこにも出ない。
    //   BossHealthBarComponent と同じく、明示の割り当てを先に見て、
    //   空のときだけ型で探す。
    FBZZ_GROUP("ボス")
    FBZZ_REF(GameObject, bossObject, "ボス")
    FBZZ_TOOLTIP("これを倒したらクリア。未設定なら IBoss を実装したオブジェクトを盤面から探す")

    FBZZ_GROUP("シーン")
    FBZZ_FIELD_RANGE_INT(int, stageNumber, 1, "採点するステージ番号", 1, 7)
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")
    FBZZ_FIELD_RANGE(float, endDelay, 0.8f, "End Delay", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, defeatDelay, 3.6f, "死亡演出の秒数", 2.0f, 8.0f)
    // WHY ボス撃破だけ別の待ちを持つか:
    //   決着の «見え» は倒した相手の大きさで長さが変わる。雑魚を掃除し終えた勝ちは
    //   0.8 秒で切り上げてよいが、ボスは崩れ切るまでに数秒かかる (BossDeathVfxComponent)。
    //   同じ 1 つの値で両方を賄うと、雑魚側に合わせればボスが «消える前に画面が変わる»、
    //   ボス側に合わせれば雑魚戦の終わりが毎回間延びする。
    //   敗北をこちら側に載せないのは、待たせているあいだ画面に «倒れたプレイヤー» しか
    //   映らないため (見せるものが無い時間は短いほどよい)。
    FBZZ_FIELD_RANGE(float, bossEndDelay, 5.0f, "Boss End Delay", 0.0f, 12.0f)
    FBZZ_TOOLTIP("ボスを倒したときだけ使う待ち。撃破演出が終わるより短くすると、"
                 "崩れている途中でリザルトへ切り替わる。ボスの EnemyHealthComponent の "
                 "Destroy Delay はこれより長くしておくこと (先に消える)")

    // WHY 体力の表示を持たないか:
    //   体力は PlayerHealthBarComponent がバーとして出している。同じ値を進行側でも
    //   文字にすると、片方の書式や色を変えたときにもう片方だけが取り残される。
    //   ここが出すのは進行 (今の目的) だけ。
    FBZZ_GROUP("HUD Names")
    FBZZ_FIELD(std::string, objectiveTextName, "HUD_Objective", "Objective Text")

    // 文言が変わった «瞬間» だけ解読で出し直す。
    //
    // WHY 常駐の行にこそ要るか: 目的の 1 行は画面に出しっぱなしなので、差し替わっても
    //   **変わったことに気付かれない**のが一番起きやすい失敗。音も閃光も足さずに
    //   目を向けさせる手が要る。解読ならタイトルと OPTIONS で既に使っている語彙で、
    //   画面をまたいで «文字が確定していく» の意味が一貫する。
    //
    // WHY 中央の命令には掛けないか: あちらは 0.8 秒で **読ませる** のが仕事。
    //   確定するまで 0.3 秒かかる演出は、読みやすさを削るだけで本末転倒になる
    //   (TutorialPromptComponent は読めた後に走査を 1 回だけ通す)。
    //
    // NOTE: 対象の UIText は richText = true にしておくこと (UiTextFx.hpp の約束)。
    FBZZ_FIELD_RANGE(float, objectiveDecodeSeconds, 0.35f, "解読 [s]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("0 で解読しない (差し替えるだけ)。長いと «読めるまで待たされる»")
    FBZZ_FIELD_COLOR(objectiveColor, (Vector4{ 0.92f, 0.95f, 1.0f, 1.0f }), "確定した字")
    FBZZ_FIELD_COLOR(objectiveHotColor, (Vector4{ 1.0f, 0.86f, 0.55f, 1.0f }), "確定する瞬間")
    FBZZ_FIELD_COLOR(objectiveScrambleColor, (Vector4{ 0.38f, 0.42f, 0.50f, 1.0f }), "未確定")

    // WHY 環境音を進行側が持つか:
    //   «この場所に居る» はステージが始まってから終わるまで続く状態で、始まりと
    //   終わりを知っているのは進行そのもの。空の GameObject を 1 つ置いて鳴らす形にすると、
    //   シーンからそれを消しただけで無音になり、消えたことに誰も気付けない。
    FBZZ_GROUP("Ambience")
    FBZZ_FIELD_RANGE(float, ambienceVolume, 0.30f, "Arena", 0.0f, 1.0f)
    FBZZ_TOOLTIP("アリーナの環境音の音量。0 で鳴らさない")

    // WHY 曲だけコードの表 (BgmLibrary) に置かないか:
    //   画面の曲は «タイトルの曲» のように 1 つに決まるが、ステージの曲は盤面ごとに
    //   違い、しかも «開幕» と «本番» の 2 つある。表に持たせると盤面を 1 つ足すたびに
    //   コードを触ることになり、曲を差し替えるだけで再ビルドが要る。
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
    ///
    /// WHY 進行が «自分の文言» を捨てられるようにするか (2026-09-14):
    ///     チュートリアルは «今どの段に居るか» を目的として出す。同じ 1 行へ 2 人が
    ///     毎フレーム書くと、どちらが後に走ったかで文言がちらつく。持ち主を 1 人に
    ///     決められる口を置いて、差されている間は進行側が黙る。
    ///
    /// WHY 決着だけは奪い返すか: AREA CLEAR / SYSTEM DOWN は «盤面が終わった» で、
    ///     段の話ではない。ここまで外に任せると、終わったのに段の文言が残る。
    void SetObjective(const std::string& text) { m_objective = text; }

    /// «ここからが本番» を告げる。掛け替える曲が無ければ何も起きない。
    ///
    /// WHY きっかけだけを受け取るか: いつ本番へ入るかを知っているのは教える側
    ///     (Stage_01 の TutorialDirector) だが、どの曲かは盤面の持ち物。曲名まで
    ///     向こうに持たせると、教える側がステージごとの素材を知ることになる。
    ///
    /// WHY 何度呼ばれてもよい形にするか: 押し直しで届くようにしておくと、
    ///     呼ぶ側は «自分の OnStart が進行より先か後か» を気にしなくて済む
    ///     (宣言が同じなら bgm::Play は何もしない)。
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
    ///
    /// WHY 数で持つか: Stage_02 は蛇を 2 体置く。1 体だけ見て終わりを決めると、
    ///     先に倒れた方でステージが閉じ、もう 1 体が生きたままリザルトへ抜ける。
    ///     «全部倒したか» は数え上げでしか言えない。
    [[nodiscard]] int CountBosses(int& outAlive) const;

    /// 盤面の «開いている / 閉じている» をボスの交戦状態へ合わせる。
    ///
    /// WHY 進行がここを持つか: 弾薬の供給もハザードも «戦闘が始まったら開く» もので、
    ///     いつ始まるかを知っているのは進行だけ。それぞれのスクリプトが自分で
    ///     «ボスは起きたか» を見に行くと、同じ問いの答えが盤面に 3 つできる。
    void SyncBoard(bool fighting) const;

    // WHY 保持せず毎回引くか: マネージャーは別の子オブジェクトに居るので、
    //     どちらの OnStart が先に走るかはシーンの並び次第になる。開始時に 1 度
    //     掴んで持ち続けると、並び順を変えただけで戦果が丸ごと 0 になる。
    [[nodiscard]] CombatManagerComponent* Combat() const
    {
        return CombatManagerComponent::Instance();
    }

    PlayerComponent* m_player = nullptr;
    // 「戦闘が居ない」の報告口。最初のフレームだけ空なのは並び順の都合なので、
    // 猶予を過ぎても見つからないときだけ出す (ManagerWatch.hpp)。
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

    // 進行の音は画面の出来事であって空間の出来事ではないので UI バスの 2D。
    se::EnsureSource(scene, "UI");
    // Wave 制を畳んだので «Wave 開始» は無い。ステージの始まりはボス部屋の扉が開く音。
    se::Play(audio, se::kUiGateOpen);

    // 環境音は «場所を持たない» 音。3D で鳴らすと、進行スクリプトが置かれた座標が
    // アリーナの中心だという前提が要る上に、プレイヤーがそこから離れるほど
    // «アリーナに居る» が薄れるという逆立ちが起きる。
    m_ambience.SetKey("ArenaAmbience");
    m_ambience.SetOutput("SE", 0.0f);
    m_ambience.Update(*this, se::kEnvArenaAmb.First(), ambienceVolume);

    bgm::Play(audio, stageBgm);
    // 本番の曲は «戦いの最中» に掛け替わる。mp3 の展開は同期なので、そのとき初めて
    // 読むと弾いている最中に 1 回だけ引っ掛かる。扉 (ワイプ) の裏で先に読んでおく。
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
    // 明示の割り当てでも «出ている» ことは条件。畳まれている間まで勝利条件に据えると、
    // まだ動いていないボスの HP を 0 と読んでしまう (FindBossOnBoard の WHY)。
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
        // WHY 体力が読めない相手を «生きている» 側に数えるか: 読めないのは組み方が
        //     壊れているということで、死んだ側へ倒すとその盤面は開始と同時にクリアする
        //     (下で 1 度だけ名指しする)。
        if (!health || health->IsAlive()) ++outAlive;
    }
    return static_cast<int>(bosses.size());
}

inline void GameFlowComponent::SyncBoard(bool fighting) const
{
    // 中央の焼けはボス戦の一部。開幕から作動していると、踏み込む前から
    // «近づけない場所» ができて、部屋の中がどう見えるかが変わってしまう。
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

    // 決着はリザルト画面へ移る前に鳴らす。endDelay の間、画面には結果が出ているのに
    // 音だけシーン遷移まで来ない、という間ができないようにする。
    se::Play(audio, victory ? se::kUiGameClear : se::kUiGameOver);

    // 決着の一撃だけを残す ─ ただし «止める» のではなく «引く»。崩れ落ちる数秒に
    // 戦いの曲が鳴り続けると «まだ戦っている» ことになるが、そこで音を断つと
    // 断った瞬間そのものが出来事として聞こえてしまい、撃破の一撃より目立つ。
    //
    // WHY 下げ切る長さを待ちから引くか: 待ちは倒した相手で変わる (bossEndDelay と
    //     endDelay)。固定の長さにすると、長い方では消えた後の無音が数秒残り、
    //     短い方では画面が変わっても曲がそのままリザルトへ入る。
    //     待ちに合わせておけば、画が切り替わるのと音が尽きるのが同じ瞬間になる。
    bgm::Stop(audio, std::max(m_endRemaining, bgm::kStopFade));

    // 締めの曲は待ちの裏で読んでおく。mp3 の展開は同期なので、リザルトが読んでから
    // 鳴らすと «画は出ているのに音だけ後から来る» になる (battleBgm の Preload と同じ理由)。
    (void)audio.Preload(victory ? bgm::kResultClear : bgm::kResultFailed);

    FadeOutAmbience();
}

inline void GameFlowComponent::FadeOutAmbience()
{
    // 残り時間をそのまま音量にする。0 まで来ると LoopVoice が自分で止める。
    const float remain = m_endDuration > 0.0f
        ? std::clamp(m_endRemaining / m_endDuration, 0.0f, 1.0f)
        : 0.0f;
    m_ambience.Update(*this, se::kEnvArenaAmb.First(), ambienceVolume * remain);
}

inline void GameFlowComponent::RefreshHud()
{
    if (GameObject* text = scene.Find(objectiveTextName)) {
        // 目的は «削り切る» ではなく «弾いて崩し、とどめで脚を落とす»。倒れている間だけ
        // 言い方を変える ─ その 5 秒に何を押すかが、この遊びで一番迷う所だから。
        // WHY キー名を書かないか: 弾きの割り当ては差し替えられる (OPTIONS の parry 行)
        //     し、既定も動く ─ 実際 Q から右クリックへ移した後もここだけ [Q] のまま
        //     残っていた。文言は «何をするか» だけを言い、どのキーかは OPTIONS と
        //     操作案内 (UiHintBar) に任せる。
        // WHY «登れ» をやめたか (2026-09-11): 2026-09-08 に決着をコアへ移したとき
        //     «CLIMB» へ書き換えたが、**登攀そのものが 2026-09-10 に取り下げられ、
        //     決着は脚 4 本へ戻っている** (Docs/climb-core.md)。ここだけ取り残されて、
        //     画面が «存在しない操作» を指示し続けていた。
        const char* objective = "PARRY  >  BREAK  >  EXECUTE";
        // 2 体居る盤面では «どちらかが倒れている» で言い切る。代表の 1 体だけを見ると、
        // 倒れていない方が代表になっているあいだ、開いた窓が画面に出ない。
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

        // 差し替わった瞬間だけ解読を焚く。
        if (line != m_shownObjective) {
            m_shownObjective = line;
            m_decode = std::max(objectiveDecodeSeconds, 0.0f);
            // 解読しない設定なら、ここで 1 度だけ素の文字を置いて終わり。
            if (m_decode <= 0.0f) ui.SetText(text, line);
        }

        if (m_decode <= 0.0f) return;

        // WHY 実時間で数えるか: 教える合図は世界を 0.35 倍まで落とす。スケール時間で
        //     数えると、その間だけ解読が 3 倍近く伸びて «読めない字» が居座る。
        m_decode = std::max(0.0f, m_decode - std::max(Time::unscaledDeltaTime, 0.0f));
        const float total = std::max(objectiveDecodeSeconds, 0.01f);
        const float p     = 1.0f - m_decode / total;

        // 頂点色は UIText.color に掛かる。色は文字列の側で組むので、こちらは白 + α
        // にしておく (UiTextFx.hpp の «絶対色で書く» の約束)。
        ui.SetTextColor(text, { 1.0f, 1.0f, 1.0f, objectiveColor.w });
        ui.SetText(text, textfx::Decode(
            m_shownObjective, p,
            static_cast<std::uint32_t>(m_shownObjective.size()) * 2654435761u,
            static_cast<std::uint32_t>(Time::unscaledTime * 30.0f),
            objectiveColor, objectiveHotColor, objectiveScrambleColor));

        // 解読し切ったら素の文字へ戻す。リッチテキストのまま置きっぱなしにすると、
        // 1 行ぶんのタグを毎フレーム組み直す必要が無いのに組み直し続けることになる。
        if (m_decode <= 0.0f) {
            ui.SetTextColor(text, objectiveColor);
            ui.SetText(text, m_shownObjective);
        }
    }
}

inline void GameFlowComponent::OnUpdate()
{
    // 戦闘が居ないと敵は永遠に減らず、勝利条件が来ない。1 度だけ名指しで止める。
    // WHY 初回フレームで判定しないか: ScriptSystem は GameObject ごとに
    //     OnAwake → OnStart → OnUpdate をまとめて回す。CombatManager がこのスクリプトより
    //     後ろに並んでいると、最初の OnUpdate の時点ではまだ OnStart が走っておらず
    //     Instance() が空になる。そこで確定させると、正しく置いてあるシーンでも
    //     毎回このエラーが出る (実際そうなっていた)。猶予は ManagerWatch が持つ。
    if (m_combatWatch.ShouldReport(Combat() != nullptr)) {
        debug.LogError("GameFlowComponent found no CombatManagerComponent in the scene "
                       "(no damage is applied and the wave never clears).");
    }

    const auto* clock = TimeManagerComponent::Instance();
    if (!m_ending && FindBoss() && !cutscene::HoldsPlayer(fbzz::Time::unscaledTime) &&
        (!clock || (!clock->IsPaused() && !clock->HasOverride())))
        m_elapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
    debugElapsed = m_elapsed;

    // 終わりを決めるのはボスの生死だけ。
    //
    // WHY «居る» と «戦っている» を分けて見るか: 部屋へ入る前のボスは立っているだけで
    //     相手ではない (FindBossOnBoard の WHY)。
    GameObject* boss = FindBoss();
    debugBoss = boss ? boss->name : std::string{};

    // 決着が付いた後まで盤面を開けておくと、崩れているボスの周りで床が焼け続ける。
    SyncBoard(boss != nullptr && !m_ending);

    // ボスが 1 体も名簿に載っていない盤面は «勝ちようがない»。以前ここは静かに
    // 何もせず、«倒してもステージが終わらない» としか画面に出なかった (IBoss.hpp の WHY)。
    if (!m_ending && m_bossWatch.ShouldReport(StageHasBoss(scene))) {
        debug.LogError("GameFlowComponent: no IBoss is registered on this board. "
                       "The stage can never be cleared. Each boss must call "
                       "IBoss::Bind(scene.Self(), this) in OnStart.");
    }

    if (!m_ending) {
        // 生死を持っているのは HP の側 (IBoss.hpp が HP を重ねない理由)。
        // 盤面の全員が倒れて初めて決着。1 体でも立っていれば戦闘は続く。
        int       alive = 0;
        const int count = CountBosses(alive);
        if (count > 1)
            debugBoss += "  (" + std::to_string(alive) + "/" + std::to_string(count) + ")";
        if (count > 0 && alive == 0) BeginEnd(true);

        // WHY 体力が読めないときに «倒した» 扱いしないか: 読めないのは組み方が
        //     壊れているということで、勝ちの条件が «たまたま満たされた» 形で
        //     通ってしまうと、以後この盤面は開始と同時にクリアする。
        //     成立しない側へ倒し (CountBosses)、代わりに 1 度だけ名指しで言う。
        if (boss && !m_warnedNoBossHealth && !scene.GetScript<EnemyHealthComponent>(boss)) {
            m_warnedNoBossHealth = true;
            debug.LogError("GameFlowComponent found the boss but no EnemyHealthComponent "
                           "on it (defeating it can never end the stage).");
        }
        // WHY ボスの居ない盤面に «勝ち» が無いか: 雑魚戦を畳んだので «敵を全部倒す»
        //     という決着が無くなった。ボスの居ないステージは今のところ存在しない。
        //     ここで «敵 0 なら勝ち» を残すと、盤面が空の間ずっと成立してしまう。
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
        // ランク評価の 3 軸のうち、時間以外の 2 つ。攻め (途切れなかった斬撃) と
        // 守り (受けた量) を別々に運ぶ (Docs/game-flow.md「評価とランク」)。
        GameResultState::bestChain     = combat ? combat->BestChain() : 0;
        GameResultState::damageTaken   = combat ? combat->DamageTaken() : 0;
        GameResultState::perfectDodges = combat ? combat->PerfectDodges() : 0;
        GameResultState::parries = combat ? combat->Parries() : 0;
        GameResultState::perfectCadences = combat ? combat->PerfectCadences() : 0;
        m_resultCaptured = true;
    }
    if (!clock || !clock->IsPaused())
        m_endRemaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
    // 曲は AudioManager 側で下がっていくが、環境音は毎フレーム書かないと下がらない。
    FadeOutAmbience();
    if (m_endRemaining > 0.0f) return;


    // WHY 戻り値を見るか: 読み込めないと «決着はついたのに何も起きない» で止まる。
    //     この関数はここまで来ると毎フレーム通るので、黙って捨てると同じ失敗を
    //     延々と繰り返しながら画面には何も出ない、という一番追いにくい形になる。
    //     1 度だけ名指しで言い、以後は試み続ける (アセットを直せばその場で復帰する)。
    // 扉 (ワイプ) で塗ってから切り替える。進めて描くのは ScreenEffectManager。
    // 塗っている最中はここへ毎フレーム来ても何もしない。切り替えに失敗すると扉は
    // 開き直す (Idle へ戻る) ので、また塗り始める ─ 直せばその場で復帰する。
    if (transition::Active()) return;
    if (!transition::Begin(resultScene, true) && !m_warnedNoResultScene) {
        m_warnedNoResultScene = true;
        debugOutcome = "Result scene missing";
        debug.LogError("GameFlowComponent could not load the result scene '" + resultScene
                       + "' (the stage is over but the screen never changes).");
    }
}

} // namespace sandbox
