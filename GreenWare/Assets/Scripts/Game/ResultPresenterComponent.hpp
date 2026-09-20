/// @file    ResultPresenterComponent.hpp
/// @brief   Result.scene。CLEAR と FAILED を同じ器で出し分ける。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// 画面の作り（Assets/UI/README_Result_Select.md と同じ）:
///   Canvas
///     Common        ─ ＋−マーク / 罫線 / STAGE 01 / ヒーロー語
///     Group_Clear   ─ 採点3項目 ＋ RANK（Group_Rank / Group_NoRank）
///     Group_Failed  ─ ボスHPバー ＋ 経過時間・最大連撃・被ダメージ
///     Actions       ─ Act0..2（Bar / Label / Hint）
///
/// @note 1 つの UIText に流し込まない。勝敗も戦績もランクも 1 本の文字列にすると
///       «どの数字がどの評価軸か» が字面の順番だけになり、達成ラインの併記も、点が
///       入った項目だけを光らせることもできない。項目ごとに別の UIText を持てば
///       Reference（Assets/UI/Reference/Result_*.png）の並びが作れる。
/// @note ノード名で引く。参照フィールドを 40 個並べるとエディタ上で 1 つずつ割り当てる
///       ことになりシーンを作り直すたびに割り当ても作り直しになるため、名前で引いて
///       C++ を触らずに配置を変えられるようにする。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Game/ResultFieldGridComponent.hpp>
#include <Scripts/Title/ScreenDressingComponent.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/UI/StageCatalog.hpp>
#include <Scripts/UI/StageProgressState.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>


using namespace fbzz::scene;
using namespace fbzz::math;   ///< Vector3 / Vector4 (見出しの色ずれで使う)

namespace sandbox {

class ResultPresenterComponent : public Script {
    FBZZ_SCRIPT(ResultPresenterComponent)

public:
    FBZZ_GROUP("流れ")
    FBZZ_FIELD(std::string, retryScene,  "Stage_01",    "RETRY")
    FBZZ_FIELD(std::string, selectScene, "StageSelect", "STAGE SELECT")
    FBZZ_FIELD(std::string, titleScene,  "Title",       "TITLE")
    FBZZ_FIELD(std::string, actorName,   "Player",      "Actor")
    FBZZ_TOOLTIP("右側に出すキャラクター。Victory / DefeatIdle ステートを持つ Animator が要る")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(dotOnColor,  (::fbzz::math::Vector4{ 0.310f, 0.851f, 0.478f, 1.0f }), "Dot On")
    FBZZ_TOOLTIP("点の入った項目。Reference の rgb(79,217,122)")
    FBZZ_FIELD_COLOR(dotOffColor, (::fbzz::math::Vector4{ 0.169f, 0.180f, 0.200f, 1.0f }), "Dot Off")
    FBZZ_FIELD_COLOR(rankSColor,  (::fbzz::math::Vector4{ 0.310f, 0.851f, 0.478f, 1.0f }), "Rank S")
    FBZZ_FIELD_COLOR(rankAColor,  (::fbzz::math::Vector4{ 0.949f, 0.941f, 0.925f, 1.0f }), "Rank A")
    FBZZ_FIELD_COLOR(rankBColor,  (::fbzz::math::Vector4{ 0.604f, 0.592f, 0.569f, 1.0f }), "Rank B")
    FBZZ_FIELD_COLOR(rankCColor,  (::fbzz::math::Vector4{ 0.420f, 0.408f, 0.384f, 1.0f }), "Rank C")

    FBZZ_GROUP("導入")
    FBZZ_FIELD(float, introSpread,  34.0f, "拡がり")
    FBZZ_TOOLTIP("＋と−の写しが離れて始まる距離 (px)。0 にすると演出が消える")
    FBZZ_FIELD(float, introSeconds, 0.52f, "Converge")
    FBZZ_TOOLTIP("2 極が寄り切るまでの秒数")
    FBZZ_FIELD(float, idleBreath,   1.6f,  "Breath")

    FBZZ_GROUP("Select")
    FBZZ_FIELD(float, selectSlide,  10.0f, "滑り")
    FBZZ_TOOLTIP("選択中の行が右へずれる量 (px)")
    FBZZ_FIELD(float, selectGhost,  5.0f,  "Ghost")
    FBZZ_TOOLTIP("選択中の行に出す＋−の色ずれ (px)。0 で消える")
    FBZZ_FIELD(float, selectTravel, 15.0f, "移動軌跡")
    FBZZ_TOOLTIP("選択が行から行へ移る速さ。大きいほど即座に飛ぶ")
    FBZZ_TOOLTIP("寄り切ったあとに残す色ずれ (px)。0 で完全に白へ収める")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(int, previewResult, 0, "プレビュー")
    FBZZ_TOOLTIP("エディタで見た目を確かめる用。0=実際の結果 / 1=CLEAR / 2=FAILED。"
                 "ゲームから来た結果を握り潰すので、確認が済んだら 0 へ戻すこと")
    FBZZ_FIELD_READ_ONLY(std::string, debugCursor, "0", "カーソル")

    void OnStart() override;
    void OnUpdate() override;

private:
    bool m_wipeWriting = false;   ///< 扉を自分で postprocess へ書いたか (transition::Drive)
    static constexpr int kActions = 3;

    [[nodiscard]] GameObject* N(std::string_view name) const { return scene.Find(std::string(name), true); }
    void Text(std::string_view name, const std::string& value) const
    {
        if (GameObject* go = N(name)) ui.SetText(go, value);
    }
    void Show(std::string_view name, bool on) const
    {
        if (GameObject* go = N(name)) go->SetActive(on);
    }
    static std::string Clock(float seconds);
    void FillClear();
    void FillFailed();
    void Submit(int index);

    GameObject* m_act[kActions]   = {};
    GameObject* m_label[kActions] = {};
    GameObject* m_bar[kActions]   = {};
    std::string m_target[kActions];
    /// その行が «どのステージを遊ぶか»。-1 はステージではない行き先 (一覧 / タイトル)。
    int m_targetStage[kActions] = { -1, -1, -1 };
    int   m_cursor  = 0;
    float m_repeat  = 0.0f;
    bool  m_victory = false;
    bool  m_actorStarted = false;

    /// @name 導入演出
    /// @{
    void UpdateIntro(float dt);
    GameObject* m_word      = nullptr;
    GameObject* m_wordPlus  = nullptr;
    GameObject* m_wordMinus = nullptr;
    GameObject* m_rule      = nullptr;
    float m_wordX = 0.0f;   ///< Word の定位置。写しはここからの相対で置く
    float m_intro = 0.0f;   ///< 0 → 1。1 を超えたら «寄り切った» 側の処理へ
    bool  m_snapped = false;
    /// @}

    /// @name 選択の表示
    /// @{
    void UpdateSelection(float dt);
    GameObject* m_ghostPlus  = nullptr;
    GameObject* m_ghostMinus = nullptr;
    float m_labelX[kActions] = {};   ///< Label のローカル定位置。ずらす前の値
    float m_rowY[kActions]   = {};   ///< 行のキャンバス Y。写しを飛ばす先
    float m_barH[kActions]   = {};   ///< Bar の元の高さ。脈打たせる基準
    float m_slide[kActions]  = {};   ///< 行ごとのずれ量。0 → selectSlide へ寄る
    float m_selVis = 0.0f;           ///< なめらかな «いま何行目か»。写しの Y に使う
    std::string m_labelText[kActions];   ///< 写しへ流し込む文字。行と同じでないと縁にならない
    int m_ghostRow = -1;                 ///< 写しへ最後に流した行。毎フレーム書き直さないため
    /// @}
};

FBZZ_REFLECT(ResultPresenterComponent)

inline std::string ResultPresenterComponent::Clock(float seconds)
{
    const int t = static_cast<int>(std::ceil((std::max)(seconds, 0.0f)));
    const int m = t / 60, s = t % 60;
    return (m < 10 ? "0" : "") + std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
}

inline void ResultPresenterComponent::OnStart()
{
    m_actorStarted = false;
    time.SetTimeScale(1.0f);
    StageProgressState::EnsureInit(save);
    StageProgressState::cursor = std::clamp(GameResultState::stageIndex, 0, kStageCount - 1);
    /// @note 差し替えは OnStart の 1 か所に閉じる。以降のすべての分岐は m_victory を見る
    ///       ため、ここだけを偽れば «勝った画面» が丸ごと再現でき、確認のために
    ///       GameResultState を書き換えて戻し忘れる事故が起きない。
    m_victory = previewResult == 1 ? true
              : previewResult == 2 ? false
                                   : GameResultState::victory;

    /// @name 共通。＋−マークと罫線は勝敗で絵そのものが変わる
    Show("Mark_Clear", m_victory);   Show("Mark_Fail", !m_victory);
    Show("Rule_Clear", m_victory);   Show("Rule_Fail", !m_victory);
    Text("Word", m_victory ? "CLEAR" : "FAILED");
    Show("Group_Clear",  m_victory);
    Show("Group_Failed", !m_victory);

    if (m_victory) FillClear(); else FillFailed();

    /// @note 下地の極を勝敗へ寄せる (勝ちは − 寒色側、負けは ＋ 暖色側)。色そのものでは
    ///       なく寄せ方だけを渡す ─ 配色を持つのは向こう (と BladeColors) で、こちらが
    ///       色を書くと配色を変えたときリザルトだけ古い色で残る。負けの画面は «焼けた»
    ///       側へ寄せたく、暖色を勝ちへ割り当てると負けで画面が冷えて «静かに終わった»
    ///       に見えてしまう。
    if (auto* dressing = scene.GetScript<ScreenDressingComponent>())
        dressing->SetPoleBias(m_victory ? -0.65f : 0.75f);

    if (GameObject* actor = N(actorName)) {
        animator.SetSpeed(actor, 0.0f);
    } else {
        debug.LogWarning("ResultPresenter: " + actorName + " が見つからない (キャラクターが出ない)");
    }

    /// @name 見出しの «噛み合い» 演出
    /// @note ＋の赤と−の青が離れた位置から寄ってきて、白い見出しへ収束する。写しを
    ///       2 枚置くのは、1 つの UIText を色替えしても «2 極» にはならないため ─
    ///       離れて始まって重なって終わる運動そのものが極性の説明になる。
    m_word      = N("Word");
    m_wordPlus  = N("Word_Plus");
    m_wordMinus = N("Word_Minus");
    m_rule      = m_victory ? N("Rule_Clear") : N("Rule_Fail");
    if (m_word) m_wordX = m_word->transform.position.x;

    const std::string word = m_victory ? "CLEAR" : "FAILED";
    for (GameObject* ghost : { m_wordPlus, m_wordMinus }) {
        if (!ghost) continue;
        ui.SetText(ghost, word);
        ghost->SetActive(true);
    }
    m_intro   = 0.0f;
    m_snapped = false;
    /// @note 1 フレーム目から «離れた» 状態で出す
    UpdateIntro(0.0f);

    /// @name アクション 3 行。行き先は勝敗で変わる
    /// @note 遊んだステージは台帳から引く。RETRY の行き先は «さっき遊んだ面» で、
    ///       ステージが増えると固定のシーン名では指せないため、どの面だったかは
    ///       StageProgressState::cursor が持つ (選択画面が入れた値)。
    const int played = StageProgressState::cursor;
    const int next   = played + 1;
    const std::string retryTarget =
        StageExists(played) ? std::string(StageAt(played).scene) : retryScene;
    /// @note «次の面» は、実体があって解放済みのときだけ直行する。無ければ選択画面へ戻し、
    ///       ラベルもそちらへ寄せる ─ NEXT STAGE と書いて一覧が出るのは嘘になる。
    const bool hasNext = next < StageProgressState::kCount && StageExists(next)
                      && StageProgressState::stages[next].unlocked;

    const char* clearLabels[kActions] = { hasNext ? "NEXT STAGE" : "STAGE SELECT", "RETRY",
                                          "STAGE SELECT" };
    static constexpr const char* kFailLabels[kActions] = { "RETRY", "STAGE SELECT", "TITLE" };
    const std::string clearTargets[kActions] = {
        hasNext ? std::string(StageAt(next).scene) : selectScene, retryTarget, selectScene };
    const std::string failTargets[kActions]  = { retryTarget, selectScene, titleScene };
    /// @note 行き先が «ステージそのもの» の行だけ、遊ぶ面の番号を持ち替える。
    const int clearStages[kActions] = { hasNext ? next : -1, played, -1 };
    const int failStages[kActions]  = { played, -1, -1 };

    for (int i = 0; i < kActions; ++i) {
        m_act[i] = N("Act" + std::to_string(i));
        if (!m_act[i]) { debug.LogWarning("ResultPresenter: Act" + std::to_string(i) + " が無い"); continue; }
        m_label[i]  = ui.Find(m_act[i], "Label");
        m_bar[i]    = ui.Find(m_act[i], "Bar");
        m_target[i] = m_victory ? clearTargets[i] : failTargets[i];
        m_targetStage[i] = m_victory ? clearStages[i] : failStages[i];
        m_labelText[i] = m_victory ? clearLabels[i] : kFailLabels[i];
        if (m_label[i]) ui.SetText(m_label[i], m_labelText[i]);
        /// @note ヒントは «どこから再開するか» なので、失敗した周にしか意味がない。
        if (GameObject* hint = ui.Find(m_act[i], "Hint")) {
            hint->SetActive(!m_victory && i == 0);
            if (!m_victory && i == 0) ui.SetText(hint, "BOSS 戦の開始から");
        }
    }
    /// @note 行の定位置を控える。ずらしたあとの値を基準にしないよう、必ず動かす前に取る。
    m_ghostPlus  = N("Act_Ghost_Plus");
    m_ghostMinus = N("Act_Ghost_Minus");
    for (int i = 0; i < kActions; ++i) {
        if (!m_act[i]) continue;
        const float rowY = m_act[i]->transform.position.y;
        if (m_label[i]) {
            m_labelX[i] = m_label[i]->transform.position.x;
            m_rowY[i]   = rowY + m_label[i]->transform.position.y;
        }
        if (m_bar[i]) m_barH[i] = m_bar[i]->transform.scale.y;
        m_slide[i] = 0.0f;
    }
    for (GameObject* g : { m_ghostPlus, m_ghostMinus })
        if (g) g->SetActive(selectGhost > 0.01f);

    m_cursor   = 0;
    m_selVis   = 0.0f;
    m_ghostRow = -1;

    se::EnsureSource(scene, "UI");
    /// @note 勝ち負けで «締め» を鳴らし分ける (どちらも 1 度きり PlayOnce)。ここでも
    ///       鳴らし直すのは、曲がシーンをまたいで鳴り続けるため (LoadScene は voice を
    ///       止めない)。BGM の枠は 1 本きりなので、ここで宣言することが前の曲を
    ///       断つことでもある。
    bgm::PlayOnce(audio, m_victory ? bgm::kResultClear : bgm::kResultFailed);
    se::Play(audio, se::kUiResult);
    /// @note ランク音は «評価が出た» ことの合図。出していないときは鳴らさない。
    if (GameResultState::RankAvailable())
        se::Play(audio, se::RankBank(GameResultState::RankLabel()[0]));

    /// @note 自己ベストは表示した «後» に更新する。先に更新すると、今回の記録が
    ///       そのままベストとして併記され、更新できたかどうかが読めない。
    GameResultState::CommitBest();
    if (m_victory && previewResult == 0) {
        if (!StageProgressState::Commit(StageProgressState::cursor, save))
            debug.LogWarning("Result: 進行データを保存できませんでした ("
                             + StageProgressState::ResolvePath() + ")");
    }
}

inline void ResultPresenterComponent::FillClear()
{
    const int score = GameResultState::Score();
    const int pts[3] = {
        GameResultState::TimePoints(GameResultState::clearSeconds),
        GameResultState::TechniquePoints(),
        GameResultState::DamagePoints(GameResultState::damageTaken),
    };
    const std::string value[3] = {
        Clock(GameResultState::clearSeconds),
        GameResultState::TechniqueText(GameResultState::TechniquePoints()),
        GameResultState::DamageText(GameResultState::damageTaken),
    };
    const StageRecord& rec = StageProgressState::stages[StageProgressState::cursor];
    const std::string best[3] = {
        rec.HasCurrentScore() && rec.bestSeconds > 0.0f ? Clock(rec.bestSeconds) : "--",
        rec.HasCurrentScore() ? GameResultState::TechniqueText(rec.bestTechnique) : "--",
        rec.HasCurrentScore() && rec.leastDamage >= 0 ? GameResultState::DamageText(rec.leastDamage) : "--",
    };
    /// @note 被ダメージは «少ない方» が更新。他の 2 軸と向きが逆。
    const bool isNew[3] = {
        !rec.HasCurrentScore() || rec.bestSeconds <= 0.0f || GameResultState::clearSeconds < rec.bestSeconds,
        !rec.HasCurrentScore() || GameResultState::TechniquePoints() > rec.bestTechnique,
        !rec.HasCurrentScore() || rec.leastDamage < 0 || GameResultState::damageTaken < rec.leastDamage,
    };

    for (int r = 0; r < 3; ++r) {
        const std::string p = "Clear_Row" + std::to_string(r) + "_";
        /// @note 見出しと達成ラインはシーンに焼かず採点表から流す。雑魚を畳んだ後も
        ///       «押し込み撃破» の行が残っていたのは、ここが固定文字だったため。
        Text(p + "Name", GameResultState::Axis(r).name);
        Text(p + "Th",   GameResultState::Axis(r).thresholds);
        Text(p + "Value", value[r]);
        Text(p + "Pts",   std::to_string(pts[r]));
        /// @note 更新した行は BEST 欄を今回の値に置き換えて、NEW を出す。
        Text(p + "Best",  isNew[r] ? value[r] : best[r]);
        Show(p + "New",   isNew[r]);
        for (int k = 0; k < 3; ++k) {
            if (GameObject* dot = N(p + "Dot" + std::to_string(k)))
                ui.SetTextColor(dot, k < pts[r] ? dotOnColor : dotOffColor);
        }
    }

    /// @note クリアした挑戦を採点する。リトライ前の失敗は持ち越さない。
    const bool rated = GameResultState::RankAvailable();
    Show("Group_Rank",   rated);
    Show("Group_NoRank", !rated);
    if (rated) {
        const char rank = GameResultState::RankLabel()[0];
        Text("Clear_Rank_Letter", std::string(1, rank));
        Text("Clear_Rank_Pts", std::to_string(score) + " / 9");
        if (GameObject* go = N("Clear_Rank_Letter")) {
            ui.SetTextColor(go, rank == 'S' ? rankSColor : rank == 'A' ? rankAColor
                              : rank == 'B' ? rankBColor : rankCColor);
        }
    }
}

inline void ResultPresenterComponent::FillFailed()
{
    const int remain = static_cast<int>(std::round(GameResultState::bossHpRemain01 * 100.0f));
    const int hp = GameResultState::bossHealthRemaining;
    const int phase  = GameResultState::bossPhase >= 2 ? 2 : 1;

    Text("Fail_Lbl_Reach",  "到達  PHASE " + std::to_string(phase));
    Text("Fail_Boss_Value", "撃破まで 残り " + std::to_string(remain) + " %");
    Text("Fail_Boss_Sub", std::to_string(GameResultState::bossHealthTotal) + " 中 " + std::to_string(hp));
    /// @note 倒れたフェーズだけ赤くする。突破したフェーズは «通った» 色へ落とす。
    /// @note rgb(255,106,92)
    const ::fbzz::math::Vector4 kHere { 1.0f, 0.416f, 0.361f, 1.0f };
    const ::fbzz::math::Vector4 kDone { 0.365f, 0.416f, 0.388f, 1.0f };
    const ::fbzz::math::Vector4 kYet  { 0.200f, 0.212f, 0.235f, 1.0f };
    if (GameObject* go = N("Fail_Ph1")) ui.SetTextColor(go, phase == 1 ? kHere : kDone);
    if (GameObject* go = N("Fail_Ph2")) ui.SetTextColor(go, phase == 2 ? kHere : kYet);

    /// @note 満ちている側が «削った量»。PNG は左右に 48px の余白を含むので、
    ///       その分を足してから比を作らないと、削り 0 のときに余白ぶんが残る。
    if (GameObject* fill = N("HP_Fill")) {
        const float inner = 820.0f, pad = 48.0f;
        const float damaged = inner * (1.0f - GameResultState::bossHpRemain01);
        ui.SetImageFillAmount(fill, (pad + damaged) / (inner + pad * 2.0f));
    }

    Text("Fail_Row1_Name",  GameResultState::Axis(1).name);
    Text("Fail_Row2_Name",  GameResultState::Axis(2).name);
    Text("Fail_Row0_Value", Clock(GameResultState::clearSeconds));
    Text("Fail_Row1_Value", GameResultState::TechniqueText(GameResultState::TechniquePoints()));
    Text("Fail_Row2_Value", GameResultState::DamageText(GameResultState::damageTaken));
}

/// @brief 見出しの導入。＋と−の写しが外から寄ってきて白い本体へ収束し、
///        収束した瞬間に罫線が伸び、背景の場がひと突きされる。
/// @note 3 つを 1 つの関数で回す。«寄り切った瞬間» は 3 つに共通の 1 点で、
///       別々に時刻を持たせると必ずずれるため、t を 1 本だけ進めて全部そこから引く。
inline void ResultPresenterComponent::UpdateIntro(float dt)
{
    const float span = introSeconds > 0.01f ? introSeconds : 0.01f;
    if (m_intro < 1.0f) {
        m_intro += dt / span;
        if (m_intro > 1.0f) m_intro = 1.0f;
    }
    const float t = m_intro;

    /// @note OutCubic。終わり際がゆっくり詰まるので «吸い寄せられて止まる» に見える。
    const float e   = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const float gap = introSpread * (1.0f - e);

    /// @note 寄り切ったあとは完全な静止にせず、わずかに息をさせる。
    ///       0 にすると «画像を貼った» ように見えて、画面が止まって感じられる。
    const float breath = t >= 1.0f
        ? idleBreath * std::sin(time.UnscaledTime() * 1.9f) * 0.5f
        : 0.0f;

    /// @note 写しの濃さ。寄っている間は濃く、収束したら薄い縁として残る。
    const float ghostA = t >= 1.0f ? 0.22f : 0.30f + 0.55f * (1.0f - e);

    if (m_wordPlus) {
        Vector3 p = m_wordPlus->transform.position;
        p.x = m_wordX - gap - breath;
        m_wordPlus->transform.position = p;
        ui.SetTextColor(m_wordPlus, Vector4{ kColorRight.x, kColorRight.y, kColorRight.z, ghostA });
    }
    if (m_wordMinus) {
        Vector3 p = m_wordMinus->transform.position;
        p.x = m_wordX + gap + breath;
        m_wordMinus->transform.position = p;
        ui.SetTextColor(m_wordMinus, Vector4{ kColorLeft.x, kColorLeft.y, kColorLeft.z, ghostA });
    }

    /// @note 本体は «2 極が重なってきたぶんだけ» 現れる。最初から白があると、
    ///       寄ってくる 2 枚が «飾り» に見えて、噛み合った結果に見えない。
    if (m_word) {
        const float a = e * e;
        ui.SetTextColor(m_word, Vector4{ 0.956863f, 0.949020f, 0.933333f, a });
    }

    /// @note 罫線は収束より少しだけ遅れて伸びる。同時だと «全部いっぺんに出た» になる。
    if (m_rule) {
        const float r = (t - 0.45f) / 0.55f;
        ui.SetImageFillAmount(m_rule, r <= 0.0f ? 0.0f : (r >= 1.0f ? 1.0f : r * (2.0f - r)));
    }

    if (t >= 1.0f && !m_snapped) {
        m_snapped = true;
        /// @note 背景の場をひと突き
        ResultFieldGridComponent::Pulse(1.0f);
    }
}

/// @brief 選択中の行の見せ方。«いまここを押す» が一目で判るようにする。
/// @note 写しは行ごとに持たない。3 行ぶん置くと «全部の行が光っている» 絵になり選択の
///       合図にならないため、1 組だけ持って行から行へ渡らせ移動そのものを合図にする。
/// @note 明るさだけで差を付けない。色の差は «押せる / 押せない» とも読めてしまうため、
///       動きと明度と色ずれの 3 つを同じ行へ重ねる。
inline void ResultPresenterComponent::UpdateSelection(float dt)
{
    /// @note なめらかな行番号。行間を渡っていく途中が見えるので «どこから来たか» が残る。
    const float k = 1.0f - std::exp(-(std::max)(selectTravel, 0.1f) * dt);
    m_selVis += (static_cast<float>(m_cursor) - m_selVis) * k;

    const float t     = time.UnscaledTime();
    /// @note 0..1
    const float pulse = 0.5f + 0.5f * std::sin(t * 4.2f);

    for (int i = 0; i < kActions; ++i) {
        const bool on = (i == m_cursor);

        /// @note ずれは行ごとに追う。選択が外れた行も «戻る» ので、切り替えが硬くならない。
        m_slide[i] += ((on ? selectSlide : 0.0f) - m_slide[i]) * k;

        if (GameObject* bar = m_bar[i]) {
            ui.SetImageTexture(bar, on ? "Assets/UI/Result/Res_Focus_on.png"
                                       : "Assets/UI/Result/Res_Focus_dim.png");
            /// @note 選択中の芯だけ、上下に伸び縮みしながら明滅する。
            Vector3 sc = bar->transform.scale;
            sc.y = m_barH[i] * (on ? 1.0f + 0.16f * pulse : 1.0f);
            bar->transform.scale = sc;
            ui.SetImageColor(bar, on
                ? Vector4{ 1.0f, 1.0f, 1.0f, 0.78f + 0.22f * pulse }
                : Vector4{ 1.0f, 1.0f, 1.0f, 1.0f });
        }

        if (GameObject* label = m_label[i]) {
            Vector3 p = label->transform.position;
            p.x = m_labelX[i] + m_slide[i];
            label->transform.position = p;
            ui.SetTextColor(label, on ? Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                      : Vector4{ 0.463f, 0.455f, 0.439f, 1.0f });
        }
    }

    /// @name 選択中の行に付く＋−の縁
    /// @note 見出しと同じ言い方。あちらが «噛み合って決まった» なら、こちらは
    ///       «まだ噛み合っていない» ので、左右に開いたまま脈を打たせる。
    if (!m_ghostPlus && !m_ghostMinus) return;
    if (selectGhost <= 0.01f) return;

    /// @note 挟んでいる 2 行の間を実測値で補間する。行間が等しいと決め打つと、
    ///       あとで 1 行だけ高さを変えたときに写しだけ取り残される。
    const int   lo  = (std::max)(0, (std::min)(kActions - 1, static_cast<int>(std::floor(m_selVis))));
    const int   hi  = (std::min)(kActions - 1, lo + 1);
    const float mix = m_selVis - static_cast<float>(lo);
    const float rowY  = m_rowY[lo]  + (m_rowY[hi]  - m_rowY[lo])  * mix;
    const float baseL = m_labelX[lo] + (m_act[lo] ? m_act[lo]->transform.position.x : 0.0f) + m_slide[lo];
    const float baseH = m_labelX[hi] + (m_act[hi] ? m_act[hi]->transform.position.x : 0.0f) + m_slide[hi];
    const float baseX = baseL + (baseH - baseL) * mix;

    const float gap = selectGhost * (0.55f + 0.45f * pulse);
    /// @note 行を渡っている間は薄める。移動中に濃いと «2 行選んでいる» ように見える。
    const float travel = std::abs(static_cast<float>(m_cursor) - m_selVis);
    const float a      = 0.34f * (1.0f - (std::min)(travel, 1.0f) * 0.6f);

    /// @note 文字は行が変わったときだけ流し直す。毎フレーム SetText すると
    ///       グリフのレイアウトを毎回やり直すことになる。
    if (m_ghostRow != m_cursor) {
        m_ghostRow = m_cursor;
        if (m_ghostPlus)  ui.SetText(m_ghostPlus,  m_labelText[m_cursor]);
        if (m_ghostMinus) ui.SetText(m_ghostMinus, m_labelText[m_cursor]);
    }

    if (m_ghostPlus) {
        m_ghostPlus->transform.position  = Vector3{ baseX - gap, rowY, 0.0f };
        ui.SetTextColor(m_ghostPlus,  Vector4{ kColorRight.x,  kColorRight.y,  kColorRight.z,  a });
    }
    if (m_ghostMinus) {
        m_ghostMinus->transform.position = Vector3{ baseX + gap, rowY, 0.0f };
        ui.SetTextColor(m_ghostMinus, Vector4{ kColorLeft.x, kColorLeft.y, kColorLeft.z, a });
    }
}

inline void ResultPresenterComponent::OnUpdate()
{
    /// @note リザルトはヒットストップやスローの影響を受けない。
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    /// @note 扉 (ワイプ)。塗っている / 剥がしている最中は入力を受けない。
    if (transition::Drive(dt, scene, postprocess, m_wipeWriting, false)) return;

    /// @note 初回Controllerロードはステートを初期化する。入場完了後に一度だけ頭出しする。
    if (!m_actorStarted && !transition::Active()) {
        if (GameObject* actor = N(actorName); actor && !animator.GetCurrentState(actor).empty()) {
            animator.SetSpeed(actor, 1.0f);
            animator.Play(actor, m_victory ? "Victory" : "DefeatIdle");
            m_actorStarted = true;
        }
    }

    UpdateIntro(dt);

    /// @name カーソル（マウス / パッド）
    /// @note GameCursorComponent が ui.SetPointer で 1 本のポインターへ畳んでいるので、
    ///       ここは «どの行の上に居るか» を UIButton から受け取るだけで済む。カーソルを
    ///       先に見るのは、スティックが «移動» と «カーソル» の両方を動かすため ─
    ///       カーソルが行の上に居るなら、それが最後に示された意思。
    int hovered = -1;
    for (int i = 0; i < kActions; ++i)
        if (m_act[i] && (ui.IsHovered(m_act[i]) || ui.IsPressed(m_act[i]))) hovered = i;
    if (hovered >= 0 && hovered != m_cursor) {
        m_cursor = hovered;
        audio.PlayOneShot(uinav::kMove);
    }

    /// @name キーとパッドの上下
    const float v = input.GetMoveAxis().y;
    m_repeat -= dt;
    if (std::abs(v) < 0.4f) m_repeat = 0.0f;
    else if (m_repeat <= 0.0f) {
        m_cursor = (m_cursor + (v < 0.0f ? 1 : kActions - 1)) % kActions;
        m_repeat = 0.22f;
        audio.PlayOneShot(uinav::kMove);
    }
    debugCursor = std::to_string(m_cursor);

    UpdateSelection(dt);

    /// @note 押した行へ行く。キーで決定したときは、いまカーソルが乗っている行。
    for (int i = 0; i < kActions; ++i)
        if (m_act[i] && ui.WasClicked(m_act[i])) { Submit(i); return; }
    if (input.GetActionDown("Submit")) Submit(m_cursor);
    else if (input.GetActionDown("Cancel")) Submit(m_victory ? kActions - 1 : 1);
}

inline void ResultPresenterComponent::Submit(int index)
{
    if (index < 0 || index >= kActions) return;
    const std::string& target = m_target[index];
    if (target.empty()) return;

    /// @note 次の面へ直行するときは «遊んでいる面» を持ち替える。ここを忘れると、
    ///       その周のリザルトが 1 つ前の行へ記録を書き、リトライも前の面へ戻る。
    if (m_targetStage[index] >= 0) StageProgressState::cursor = m_targetStage[index];

    audio.PlayOneShot(uinav::kConfirm);
    if (!transition::Begin(target))
        debug.LogError("ResultPresenter: シーンへの扉を開けません -> " + target);
}

} // namespace sandbox
