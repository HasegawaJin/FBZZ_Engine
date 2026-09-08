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

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/ArenaHazardComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

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
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")
    FBZZ_FIELD_RANGE(float, endDelay, 0.8f, "End Delay", 0.0f, 5.0f)
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

    // WHY 環境音を進行側が持つか:
    //   «この場所に居る» はステージが始まってから終わるまで続く状態で、始まりと
    //   終わりを知っているのは進行そのもの。空の GameObject を 1 つ置いて鳴らす形にすると、
    //   シーンからそれを消しただけで無音になり、消えたことに誰も気付けない。
    FBZZ_GROUP("Ambience")
    FBZZ_FIELD_RANGE(float, ambienceVolume, 0.30f, "Arena", 0.0f, 1.0f)
    FBZZ_TOOLTIP("アリーナの環境音の音量。0 で鳴らさない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "経過")
    FBZZ_FIELD_READ_ONLY(std::string, debugBoss, "", "ボスのオブジェクト")
    FBZZ_TOOLTIP("勝利条件として見ている相手。空ならボスがまだ眠っている")
    FBZZ_FIELD_READ_ONLY(std::string, debugOutcome, "-", "Outcome")

    void OnStart() override;
    void OnUpdate() override;
    void OnDisable() override { m_ambience.Stop(*this); }

private:
    void BeginEnd(bool victory);
    void RefreshHud();

    /// 勝利条件として見るボス。この盤面で終わりを決めるのはこの相手の生死だけ。
    ///
    /// WHY 1 体だけ返すか (以前は全 IBoss を走査していた): 盤面に置くボスは 1 体で、
    ///     «2 体目が居たら両方倒す» は一度も要件になっていない。にもかかわらず
    ///     «居るか» と «倒したか» が別々に走査していたため、片方だけ通る状態
    ///     (見つかるのに倒したと判定されない) が起こりうる形になっていた。
    ///     勝ち負けを決める問いは 1 つの答えから引く。
    [[nodiscard]] GameObject* FindBoss() const;

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
    bool m_ending = false;
    bool m_victory = false;
    float m_endRemaining = 0.0f;
    float m_elapsed = 0.0f;
    /// アリーナの環境音。進行の一発ものと同じ口から出すと、鳴らすたびに環境音が切れる。
    se::LoopVoice m_ambience;
};

FBZZ_REFLECT(GameFlowComponent)

inline void GameFlowComponent::OnStart()
{
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
    m_endRemaining = std::max(victory && FindBoss() ? bossEndDelay : endDelay, 0.0f);
    debugOutcome = victory ? "Victory" : "Defeat";

    // 決着はリザルト画面へ移る前に鳴らす。endDelay の間、画面には結果が出ているのに
    // 音だけシーン遷移まで来ない、という間ができないようにする。
    se::Play(audio, victory ? se::kUiGameClear : se::kUiGameOver);

    // 決着の一撃だけを残す。環境音を敷いたままだと «まだ続いている» 場所の上に
    // 終わりの合図が乗ることになり、区切りとして聞こえない。
    m_ambience.Stop(*this);
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
        const char* objective = "PARRY  >  BREAK  >  EXECUTE A LEG";
        if (!m_ending)
            if (GameObject* boss = FindBoss())
                if (const auto* iboss = IBoss::Of(boss))
                    if (iboss->IsToppled()) objective = "EXECUTE!  GET CLOSE TO A LEG";
        ui.SetText(text, m_ending ? (m_victory ? "AREA CLEAR" : "SYSTEM DOWN") : objective);
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

    if (!m_ending) m_elapsed += Time::deltaTime;
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
        if (StageHasBoss(scene)) {
            // boss が空なのは «まだ眠っている» 状態。始まるまで何も決めない。
            //
            // 生死を持っているのは HP の側 (IBoss.hpp が HP を重ねない理由)。
            //
            // WHY 体力が読めないときに «倒した» 扱いしないか: 読めないのは組み方が
            //     壊れているということで、勝ちの条件が «たまたま満たされた» 形で
            //     通ってしまうと、以後この盤面は開始と同時にクリアする。
            //     成立しない側へ倒し、代わりに 1 度だけ名指しで言う。
            const auto* health = boss ? scene.GetScript<EnemyHealthComponent>(boss) : nullptr;
            if (boss && !health) {
                if (!m_warnedNoBossHealth) {
                    m_warnedNoBossHealth = true;
                    debug.LogError("GameFlowComponent found the boss but no EnemyHealthComponent "
                                   "on it (defeating it can never end the stage).");
                }
            } else if (health && !health->IsAlive()) {
                BeginEnd(true);
            }
        }
        // WHY ボスの居ない盤面に «勝ち» が無いか: 雑魚戦を畳んだので «敵を全部倒す»
        //     という決着が無くなった。ボスの居ないステージは今のところ存在しない。
        //     ここで «敵 0 なら勝ち» を残すと、盤面が空の間ずっと成立してしまう。
    }
    RefreshHud();

    if (!m_ending) return;
    m_endRemaining -= Time::deltaTime;
    if (m_endRemaining > 0.0f) return;

    const auto* combat = Combat();
    GameResultState::victory = m_victory;
    GameResultState::clearSeconds = m_elapsed;
    GameResultState::defeatedEnemies = combat ? combat->Kills() : 0;
    // ランク評価の 3 軸のうち、時間以外の 2 つ。攻め (途切れなかった斬撃) と
    // 守り (受けた量) を別々に運ぶ (Docs/game-flow.md「評価とランク」)。
    GameResultState::bestChain     = combat ? combat->BestChain() : 0;
    GameResultState::damageTaken   = combat ? combat->DamageTaken() : 0;
    GameResultState::perfectDodges = combat ? combat->PerfectDodges() : 0;

    // WHY 戻り値を見るか: 読み込めないと «決着はついたのに何も起きない» で止まる。
    //     この関数はここまで来ると毎フレーム通るので、黙って捨てると同じ失敗を
    //     延々と繰り返しながら画面には何も出ない、という一番追いにくい形になる。
    //     1 度だけ名指しで言い、以後は試み続ける (アセットを直せばその場で復帰する)。
    // 扉 (ワイプ) で塗ってから切り替える。進めて描くのは ScreenEffectManager。
    // 塗っている最中はここへ毎フレーム来ても何もしない。切り替えに失敗すると扉は
    // 開き直す (Idle へ戻る) ので、また塗り始める ─ 直せばその場で復帰する。
    if (transition::Active()) return;
    if (!transition::Begin(resultScene) && !m_warnedNoResultScene) {
        m_warnedNoResultScene = true;
        debugOutcome = "Result scene missing";
        debug.LogError("GameFlowComponent could not load the result scene '" + resultScene
                       + "' (the stage is over but the screen never changes).");
    }
}

} // namespace sandbox
