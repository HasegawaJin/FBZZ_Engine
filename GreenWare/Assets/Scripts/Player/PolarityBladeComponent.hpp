/// @file    PolarityBladeComponent.hpp
/// @brief   双剣。斬った相手に極を乗せ、振っている間は自分もその極を帯びる
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY ここにダメージが «少しだけ» あるか:
///   「銃は敵を倒さない」という前提は残しているが、剣を振って手応えが返らないのは
///   弾に威力が無いことよりはるかに強い違和感になる。斬撃には小さいダメージを入れ、
///   それでも «斬るのは極を乗せるため» が最短ルートであり続ける量に留める
///   (Mite の HP 100 に対して 25 = 4 回)。倒す最短の手は今も衝突である。
///
/// WHY 振った瞬間に自分も極を帯びるか:
///   近接にしただけでは「近づいて斬る」ゲームになり、極性システムは盤面の飾りに落ちる。
///   斬撃が自分の極を切り替えるスイッチを兼ねていれば、«どちらの剣で斬るか» が
///   «触れた相手をどちらへ弾くか» の選択にもなり、二刀を使い分ける理由が生まれる。
///
/// WHY 攻撃が自分の足を動かさないか:
///   踏み込みも、纏いから来る引力・斥力も廃した。攻撃は «押した瞬間に斬れる» ことだけを
///   返し、立ち位置は最後まで移動入力が持つ。攻撃が体を運ぶと、間合いを詰めたのも
///   離れたのもプレイヤーの判断ではなくなり、被弾も «避けられなかった» ではなく
///   «連れて行かれた» になる。動かすのは相手だけ (PlayerPolarityComponent の体当たり)。
///
/// WHY 状態を «纏う側» に持たせないか:
///   «いつ帯びるか» を決めるのはこちら (連撃の状態を知っている)、«帯びている間どうなるか»
///   を決めるのは PlayerPolarityComponent。混ぜると、纏いの話に攻撃の状態機械が入り込む。
///
/// WHY モーション無しでも動くように書くか:
///   クリップが 1 本も無くても «斬る → 極が乗る → 相手が弾かれる» という芯は全部
///   成立するので、絵より先に手触りを確かめられる形にしておく。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartPolarityComponent.hpp>
#include <Scripts/Combat/BossPolarityRigComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerPolarityComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/BodyShake.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Vfx/SlashArcComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PolarityBladeComponent : public Script {
    FBZZ_SCRIPT(PolarityBladeComponent)

public:
    // PlayerComponent が注入する。数値を Inspector 側へ複製しない。
    fbzz::Asset<PolarityTuning> tuning{};

    // ── 斬撃モーション ───────────────────────────────────────────────────────
    //
    // WHY «当たる瞬間» をクリップ側の秒数で持つか (本作で 9 割を占める噛み合わせ):
    //   判定が出る時刻は tuning->bladeStartup ただ 1 つが決める。モーションの側に
    //   もう 1 つ «振り抜く時刻» を置くと、片方を触るたびに «斬ったのに当たらない»
    //   / «当たったのにまだ振りかぶっている» が生まれ、どちらの数字が悪いのか
    //   画面から切り分けられない。
    //   ここに置く Hit Time は «そのクリップの何秒目が斬り抜けか» という、
    //   クリップ固有の事実だけ。再生速度は Hit Time / bladeStartup で毎回導出するので、
    //   判定とモーションの一致は «合わせる» ものではなく構造的に保証される。
    //   数値の出どころは Docs/player-motions.md の実測フレーム (30fps)。
    //
    // WHY 空でも動くように書くか: クリップが 1 本も無くても «斬る → 極が乗る →
    //     自分が引かれる / 弾かれる» という芯は全部成立する。絵より先に手触りを
    //     確かめられる形を残しておく (Docs/development-plan.md の作業順)。
    // WHY 加算ではなく Override か:
    //   加算は «基準ポーズからの差分» を足す仕組みで、素材の側が差分として作られて
    //   いることを前提にする。斬撃クリップは振りかぶりから振り抜きまでを持つ
    //   «完成したポーズ» なので、走りの上体へ足すと 2 つの姿勢が重なって崩れる。
    //   上半身は斬撃が丸ごと持ち、脚だけロコモーションに残すのが正しい。
    FBZZ_GROUP("Motion")
    FBZZ_FIELD(std::string, slashLayerName, "Attack", "Slash Layer")
    FBZZ_TOOLTIP("斬撃を差し込む Override レイヤー。常駐ステートは Katana_Stance")
    FBZZ_FIELD_RANGE(float, slashLayerGain, 1.0f, "Layer Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("振っている間の上半身の «置き換え量»。1 で斬撃がそのまま出る。"
                 "下げるとロコモーションが透けて残る (Override なので 1 より上は効かない)")

    // 1・2 段目。どちらの剣で斬るかはプレイヤーが選ぶので、段ではなく極で引く。
    FBZZ_FIELD_FILE(slashRightClipFile,
        "guid:f4b66683b4b00dd2699acea0edb05d70|Library/Baked/eb4f77f1e5395fbec0a9271e4462e095/anims/Katana_Slash_R.anim",
        "Right Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashRightClipName, "Katana_Slash_R", "Right Clip Name")
    FBZZ_FIELD_FILE(slashLeftClipFile,
        "guid:df86ad597c33fe11e376bde87f62a4bf|Library/Baked/3c44e89bba0ce00b16bfcc71d0d09089/anims/Katana_Slash_L.anim",
        "Left Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashLeftClipName, "Katana_Slash_L", "Left Clip Name")
    // Katana_Slash_R/L は 24F / 0.80s。斬り抜けは f11-13 の中央 = f12。
    FBZZ_FIELD_RANGE(float, slashHitTime, 12.0f / 30.0f, "Slash Hit Time", 0.02f, 2.0f)
    FBZZ_TOOLTIP("Right/Left クリップの何秒目が «斬り抜け» か。ここが判定の瞬間に合う")

    // 3 段目 (フィニッシュ)。両刀を交差させて斬るので、左右どちらの入力でも同じ。
    FBZZ_FIELD_FILE(slashFinisherClipFile,
        "guid:12deb7da34415ea7eae58fcc71945ab1|Library/Baked/68621dfe4a66ccf3f74cfd49ba4c7075/anims/Katana_Slash_Dual.anim",
        "Finisher Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, slashFinisherClipName, "Katana_Slash_Dual", "Finisher Clip Name")
    // Katana_Slash_Dual は 32F / 1.07s。交差の瞬間が f14。
    FBZZ_FIELD_RANGE(float, slashFinisherHitTime, 14.0f / 30.0f, "Finisher Hit Time", 0.02f, 2.0f)

    FBZZ_FIELD_RANGE(float, slashFadeIn,  0.05f, "Slash Fade In",  0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, slashFadeOut, 0.12f, "Slash Fade Out", 0.0f, 0.5f)
    FBZZ_TOOLTIP("次の段が始まると前の段はこの秒数で引く。長いと連撃が «残像» になる")

    FBZZ_GROUP("Feel")
    FBZZ_FIELD_RANGE(float, hitStop, 0.22f, "Hitstop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬った瞬間の止め。**衝突 (引力の激突) より必ず弱くすること。**"
                 "同じ強さにすると、盤面で一番大きい出来事が «斬ったのと同じ重さ» になる")
    FBZZ_FIELD_RANGE(float, hitShake, 0.18f, "Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hitRumble, 0.35f, "Rumble (hit)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingRumble, 0.12f, "Rumble (swing)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空振りにも返す軽い手応え。0 にすると «入力が拾われていない» に見える")

    // ── 溜め ────────────────────────────────────────────────────────────────
    FBZZ_GROUP("Charge")
    FBZZ_FIELD_RANGE(float, chargeShake, 0.003f, "Body Shake", 0.0f, 0.05f)
    FBZZ_TOOLTIP("満溜めでの縦の伸び幅 (素の大きさに対する比)。足元が原点なので、"
                 "0.003 で手のあたりが 4mm ほど ─ «動いた» とは気づかず "
                 "«力んでいる» とだけ伝わる量。0.01 を超えると伸縮が目に見える")
    FBZZ_FIELD_RANGE(float, chargeShakeLateral, 0.0f, "Body Shake (lateral)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("縦に伸びたぶん横をどれだけ締めるか (縦に対する比)。"
                 "0 で幅は 1 mm も変わらない。上げるほど «体が膨らんでいる» が見えてくる")
    FBZZ_FIELD_RANGE(float, chargeShakeHz, 24.0f, "Body Shake Hz", 1.0f, 60.0f)
    FBZZ_TOOLTIP("震えの速さ。幅を詰めるほど速い方が «震え» に残る "
                 "(遅くて小さいと、ただ気づかれない)。遅いと «脈打っている» に見える")
    FBZZ_FIELD_RANGE(float, chargeRumble, 0.55f, "Rumble", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargeDistort, 0.30f, "Distort", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜めている間の画面の歪み。手前 (体) の震えに対する «余波» なので薄く")
    FBZZ_FIELD_RANGE(float, chargeVoiceVolume, 0.45f, "Charge Loop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("溜めの唸り。音程が溜め比で上がるので、耳だけで満溜めが分かる")
    FBZZ_FIELD_RANGE(float, chargedHitStop, 0.45f, "Hitstop (charged)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargedShake, 0.70f, "Shake (charged)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargedRumble, 0.90f, "Rumble (charged)", 0.0f, 1.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugCharge, 0.0f, "Charge")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "Idle", "Phase")
    FBZZ_FIELD_READ_ONLY(int, debugCombo, 0, "Combo")
    FBZZ_FIELD_READ_ONLY(int, debugLastHits, 0, "Last Hits")
    FBZZ_FIELD(bool, drawDebugArc, false, "Draw Arc")

    // ── 参照する側の問い合わせ ───────────────────────────────────────────────
    /// 今どちらかの剣を振っている最中か。移動側が足を鈍らせるのに読める。
    [[nodiscard]] bool IsSwinging() const { return m_phase != Phase::Idle; }
    /// 溜めの進み [0,1]。HUD と剣の発光が読める。
    [[nodiscard]] float ChargeRatio() const
    { return Clamp01(m_charge / Max(tuning->bladeChargeFull, 0.01f)); }
    /// 今出している一撃が溜め斬りか。
    [[nodiscard]] bool IsCharged() const { return m_charged; }
    /// 今振っている剣の極。振っていなければ None。
    [[nodiscard]] Polarity SwingPolarity() const { return m_polarity; }
    /// 連撃の段数 (0 起点)。剣の発光や UI が読む。
    [[nodiscard]] int ComboStep() const { return m_combo; }

    void SetAimComponent(PlayerAimComponent* aim) { m_aim = aim; }
    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }
    void SetPolarity(PlayerPolarityComponent* polarity) { m_playerPolarity = polarity; }
    void SetSlashArc(SlashArcComponent* arc) { m_slashArc = arc; }

    void OnStart()  override;
    void OnUpdate() override;
    /// 持続する手触り (震え・唸り・パッド・歪み) を残したまま消えないため。
    void OnDestroy() override { StopChargeFeel(); }

private:
    /// 発生 → 判定 → 硬直。«振っている» はこの 2 段のあいだ続く。
    enum class Phase : int { Idle = 0, Startup, Recovery };

    void ReadInput();
    void BeginSwing(Polarity polarity);
    /// 溜め斬りを出す。今の段や硬直を中断して、こちらが上書きする。
    void BeginCharged(Polarity polarity, float ratio);
    /// 溜めている間の手触りを毎フレーム流す。溜めていなければ 1 度だけ後始末する。
    void DriveCharge();
    /// 震え・唸り・歪み・パッドを全部止める。
    void StopChargeFeel();
    /// 満溜めに届いた瞬間を 1 度だけ返す。
    void NotifyChargeFull();
    /// 今の段に対応する斬撃クリップを Slot へ差し込む。クリップが空なら何もしない。
    void PlaySlashMotion(Polarity polarity);
    /// Slot のフェード量をそのままレイヤー weight へ流す。毎フレーム呼ぶ。
    void DriveSlashLayerWeight();
    /// この振りが連撃の最終段か。段数は «これから振る» 段 (m_combo) で数える。
    [[nodiscard]] bool IsFinisher() const { return (m_combo + 1) >= ComboLength(); }
    /// 今の段の発生 [秒]。判定の時刻もモーションの速さもここ 1 つから引く。
    [[nodiscard]] float StartupSeconds() const
    {
        if (m_charged) return tuning->bladeChargedStartup;
        return IsFinisher() ? tuning->bladeFinisherStartup : tuning->bladeStartup;
    }
    /// 扇の中に居る対象すべてを斬る。
    void ResolveHit();
    /// 1 体ぶんの処理。極を乗せて、少し削る。
    void HitOne(GameObject& object, PolarityTargetComponent& target);
    /// この一振りが与える量。溜め比で通常と溜め斬りの間を取る。
    [[nodiscard]] int SlashDamage() const;
    /// ロック対象が射程の外なら、発生のあいだで詰める。届かない相手へは何もしない。
    void DashToTarget();
    /// 斬った刃の極と «その部位» の極を突き合わせ、通る量の倍率を返す。
    /// 逆極なら打ち消して 1.0、同極なら弾かれて Same Polarity x。
    /// 塗られる側の部位 (蛇の節) はここで極を乗せて 1.0 を返す。
    ///
    /// WHY 手応え (止め・弾き・音) は 1 振りに 1 回しか出さないか:
    ///     溜め斬りは 4 部位へ同時に当たる。部位ごとに弾くと 1 回の入力で 4 回
    ///     押し返され、当たった数だけ体が飛ぶ。倍率は部位ごと、返しは 1 回。
    [[nodiscard]] float PartDamageScale(BossPartPolarityComponent& part, GameObject* bossRoot);
    /// 斬る向き。狙っている相手が居ればそちらへ、居なければカメラの前方へ。
    [[nodiscard]] Vector3 SwingDirection() const;
    /// 今フレーム狙っている相手 (居なければ nullptr)。
    [[nodiscard]] GameObject* AimTarget() const;

    [[nodiscard]] PlayerAimComponent* Aim() const
    { return m_aim ? m_aim : scene.GetScript<PlayerAimComponent>(); }


    /// 連撃の段数。1 を下回らせない。
    [[nodiscard]] int ComboLength() const
    { return std::max(tuning->bladeComboLength, 1); }

    PlayerAimComponent*        m_aim            = nullptr;
    PlayerControllerComponent* m_controller     = nullptr;
    PlayerPolarityComponent*   m_playerPolarity = nullptr;
    SlashArcComponent*         m_slashArc       = nullptr;

    Phase    m_phase    = Phase::Idle;
    float    m_timer    = 0.0f;
    Polarity m_polarity = Polarity::None;
    /// 連撃の段数。bladeComboLength で 0 へ戻る。
    int      m_combo    = 0;
    /// 連鎖が途切れる時刻 [Time::time]。過ぎたら段数を 0 へ戻す。
    float    m_comboExpire = 0.0f;
    /// 発生 / 硬直の最中に入った入力。硬直が明けた瞬間に出す。
    ///
    /// WHY 溜めておくか: 連撃は «硬直が明けるフレームちょうどに押す» ゲームではない。
    ///     押した入力を捨てると、繋げようとするほど手数が減るという逆立ちが起きる。
    Polarity m_buffered = Polarity::None;
    /// 振り出しで決めた «斬る向き»。判定の扇も体の向きもここ 1 つから引く。
    ///
    /// WHY 振り始めに固定するか: 振っている最中にカメラを回しても斬る先が変わらない。
    ///     追従させると、当たる範囲が振り抜きの瞬間まで決まらず «どこを斬ったのか» を
    ///     後から説明できなくなる。
    Vector3  m_swingDirection = Vector3::ZERO;

    /// 押しっぱなしにしている極 (None なら押していない)。溜めはこの極で出る。
    Polarity m_holding = Polarity::None;
    /// 押してからの秒数と、そのうち «溜まった» 秒数 (Charge Delay を超えた分)。
    float    m_hold   = 0.0f;
    float    m_charge = 0.0f;
    /// 満溜めの合図を 1 度だけ返すため。
    bool     m_chargeFull = false;
    /// 手触りを止め忘れないための番人。溜めをどこで打ち切っても後始末が 1 回走る。
    bool     m_chargeFeel = false;
    /// 今出している一撃が溜め斬りか。判定も絵も音もここで分岐する。
    bool     m_charged = false;
    /// この一振りで «極の突き合わせ» の手応え (止め・弾き・音) を既に返したか。
    bool     m_matchResolved = false;
    /// この一振りが最終段だったか。振り出しで決めて、硬直が明けるまで持つ。
    ///
    /// WHY IsFinisher() を都度呼ばないか: あちらは «これから振る段» を見るので、
    ///     判定が出た後 (m_combo が進んだ後) に呼ぶと答えが入れ替わる。
    ///     硬直中の申告 ─ ボスが差し込みを決める材料 ─ がそこで嘘になる。
    bool     m_swingIsFinisher = false;
    /// 離した瞬間の溜め比 [0,1]。振り終わるまで固定する。
    float    m_chargedRatio = 0.0f;

    shake::BodyShake m_shake;
    se::LoopVoice    m_chargeVoice;
};

FBZZ_REFLECT(PolarityBladeComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PolarityBladeComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PolarityBladeComponent requires PolarityTuning.fzdata "
                       "(PlayerComponent injects it).");
        enabled = false;
        return;
    }

    m_phase       = Phase::Idle;
    m_timer       = 0.0f;
    m_polarity    = Polarity::None;
    m_combo       = 0;
    m_comboExpire = 0.0f;
    m_buffered    = Polarity::None;
    m_swingDirection = Vector3::ZERO;
    m_holding     = Polarity::None;
    m_hold        = 0.0f;
    m_charge      = 0.0f;
    m_chargeFull  = false;
    m_chargeFeel  = false;
    m_charged     = false;
    m_chargedRatio = 0.0f;

    // 震わせる描画ノードと、その素の大きさをここで覚える。
    m_shake.Ensure(*this);

    // 溜めの唸りは «鳴り続けるもの»。主 voice で鳴らすと、その音量と音程が
    // 同じ体から出る斬撃音にもそのまま掛かる。
    m_chargeVoice.SetKey("BladeCharge");
    m_chargeVoice.SetOutput("SE", 0.0f);

    // Override レイヤーは «置き換え» なので、振っていない状態は必ず 0 から始める。
    // Play 前に Inspector で weight を上げたまま入ると、上半身が構えで固まったまま
    // 走り出すことになり、原因がスクリプト側に見えない。
    if (!slashLayerName.empty()) {
        animator.StopSlot(slashLayerName, 0.0f);
        animator.SetLayerWeight(slashLayerName, 0.0f);
    }

    // 斬撃の音はプレイヤー本人の位置で鳴る。減衰を掛ける相手が自分自身なので 2D。
    se::EnsureSource(scene);

    // 「破ってはいけない設計上の制約」を Play 開始時に検算する。
    // 破っていると 2 体目を斬り終える前に 1 体目の極が切れ、2 体を 1 つの衝突に
    // まとめられない。破綻の仕方が「なんとなく繋がらない」なので、明示しないと気付けない。
    const float shortest = tuning->ShortestDuration();
    const float margin   = tuning->TimingMargin(shortest);
    if (!tuning->SatisfiesTimingConstraint(shortest)) {
        debug.LogError("PolarityTuning breaks the timing constraint: shortest duration "
                       "must exceed one combo set + windup. Charged targets expire "
                       "before a second one can be cut.");
    } else if (margin < 0.5f) {
        debug.LogWarning("PolarityTuning timing margin is thin. Linking two cuts into "
                         "one collision is close to impossible for an average player.");
    }
}

inline GameObject* PolarityBladeComponent::AimTarget() const
{
    auto* aim = Aim();
    return aim ? aim->CurrentTarget() : nullptr;
}

inline Vector3 PolarityBladeComponent::SwingDirection() const
{
    // 基準は必ずカメラの前方。TPS では «画面の奥» が振る向きとして最も素直で、
    // ここだけは補正の値に関わらず動かない。
    Vector3 aimed = Vector3::ZERO;
    if (GameObject* camera = scene.GetMainCameraObject()) {
        Vector3 forward = camera->transform.forward;
        forward.y = 0.0f;
        if (forward.LengthSq() > EPSILON) aimed = forward.Normalized();
    }
    if (aimed.LengthSq() <= EPSILON) {
        Vector3 facing = transform.worldRotation * Vector3::FORWARD;
        facing.y = 0.0f;
        aimed = facing.NormalizedOr(Vector3::FORWARD);
    }

    // ロック対象へ寄せるのはあくまで補助。どれだけ寄せるかは bladeAimAssist ただ 1 つ
    // が決める (0 で完全に手動)。
    //
    // WHY 二択にしないか: «相手を向く / 向かない» の切り替えにすると、
    //     枠が付いた瞬間に振る向きが飛ぶ。比で混ぜれば «少しだけ手伝う» が選べて、
    //     0 と 1 の間に手触りの居場所ができる。
    const float assist = Clamp01(tuning->bladeAimAssist);
    if (assist <= 0.0f) return aimed;

    GameObject* target = AimTarget();
    if (!target) return aimed;

    Vector3 delta = target->transform.worldPosition - transform.worldPosition;
    delta.y = 0.0f;
    if (delta.LengthSq() <= EPSILON) return aimed;

    const Vector3 blended = Vector3::Lerp(aimed, delta.Normalized(), assist);
    return blended.NormalizedOr(aimed);
}

// WHY 押した «瞬間» を溜めに使わないか:
//   長押しで溜まる形にすると、押してから何も起きない時間が必ず生まれる。近接で
//   最も大事なのは «押したら斬れる» で、そこを溜めに明け渡すと全部の一撃が鈍る。
//   押した瞬間は今までどおり斬り、«その手を離さずにいる» ことが溜めになる形にすると、
//   斬ってからそのまま力を溜める 1 続きの動作として手に馴染む。
inline void PolarityBladeComponent::ReadInput()
{
    const float dt = Max(Time::deltaTime, 0.0f);

    // 右 = ＋ / 左 = −。左右の入力と左右の剣を一致させる。
    Polarity pressed = Polarity::None;
    if (input.GetActionDown(actions::kEmitPlus))  pressed = Polarity::Plus;
    else if (input.GetActionDown(actions::kEmitMinus)) pressed = Polarity::Minus;

    if (pressed != Polarity::None) {
        // 押し替えたら溜めはやり直し。2 本の剣ぶんの溜めを同時に持たない。
        m_holding    = pressed;
        m_hold       = 0.0f;
        m_charge     = 0.0f;
        m_chargeFull = false;

        // 振れるなら即座に、振れないなら溜めておく。
        if (m_phase == Phase::Idle) BeginSwing(pressed);
        else                        m_buffered = pressed;
    }

    if (m_holding == Polarity::None) return;

    if (input.GetAction(m_holding == Polarity::Plus ? actions::kEmitPlus
                                                    : actions::kEmitMinus)) {
        m_hold += dt;
        m_charge = Max(m_hold - Max(tuning->bladeChargeDelay, 0.0f), 0.0f);
        if (!m_chargeFull && m_charge > 0.0f && ChargeRatio() >= 1.0f) NotifyChargeFull();
        return;
    }

    // 離した。溜まっていれば溜め斬り、溜まっていなければ何もしない
    // (押した瞬間の通常斬りが既に出ている)。
    //
    // WHY 下限を置くか: 溜めが 1 フレームでも乗れば出す形にすると、少し長く押しただけの
    //     通常斬りが «ほとんど威力の無い溜め斬り» に化けて中断される。押し方の揺らぎで
    //     出る技が変わるのが一番読めない。ここを越えるまでは «ただの通常斬り» でいい。
    constexpr float kMinChargeRatio = 0.25f;
    const Polarity polarity = m_holding;
    const float    ratio    = ChargeRatio();
    const bool     charged  = ratio >= kMinChargeRatio;

    m_holding    = Polarity::None;
    m_hold       = 0.0f;
    m_charge     = 0.0f;
    m_chargeFull = false;
    StopChargeFeel();

    if (charged) BeginCharged(polarity, ratio);
}

inline void PolarityBladeComponent::BeginSwing(Polarity polarity)
{
    // 連鎖が途切れていれば 1 段目から。
    if (Time::time > m_comboExpire) m_combo = 0;

    // 通常斬りへ戻す。溜め斬りの直後にここへ来ると、発生もクリップも溜めのままになる。
    m_charged      = false;
    m_chargedRatio = 0.0f;

    m_polarity = polarity;
    m_phase    = Phase::Startup;
    m_swingIsFinisher = IsFinisher();
    m_matchResolved   = false;
    m_timer    = Max(StartupSeconds(), 0.0f);
    m_buffered = Polarity::None;

    m_swingDirection = SwingDirection();

    // 振り出しで詰める。判定が出る頃には間合いの内側に居る。
    DashToTarget();

    // 振り «始めた» 瞬間から自分もその極を帯びる。当ててからでは、
    // 空振りした一振りだけ極が乗らず «どちらの剣を振ったか» が絵に出ない。
    if (m_playerPolarity)
        m_playerPolarity->Charge(polarity, Max(tuning->bladeChargeSeconds, 0.0f));

    PlaySlashMotion(polarity);

    // 振り出しの «ヒュッ»。当たったかどうかとは別に、振ったこと自体を返す。
    // WHY 段を渡すか: 1 段目と締めが同じ音だと、連撃のどこに居るかが耳で追えない。
    se::Play(audio, se::BladeSwing(polarity, m_combo));

    // 空振りにも軽い手応えを返す。無反応だと «入力が拾われていない» に見える。
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.0f, swingRumble, 0.05f);

    debugPhase = "Startup";
    debugCombo = m_combo;
}

inline void PolarityBladeComponent::BeginCharged(Polarity polarity, float ratio)
{
    m_charged      = true;
    m_chargedRatio = Clamp01(ratio);
    m_polarity     = polarity;
    m_phase        = Phase::Startup;
    // 溜め斬りは段を持たない。«最終段» としては数えない。
    m_swingIsFinisher = false;
    m_matchResolved   = false;
    m_timer        = Max(StartupSeconds(), 0.0f);
    m_buffered     = Polarity::None;
    // 溜めで区切る。段を持ち越すと «溜めたのに 2 段目の絵» が出る。
    m_combo        = 0;
    // 溜め斬りはその場で全周を薙ぐ。向きは弧の絵と体の向きにだけ効く。
    m_swingDirection = SwingDirection();

    // 溜め斬りの後は長く帯びる。染めた盤面をそのまま次の一手に使える時間にする。
    if (m_playerPolarity)
        m_playerPolarity->Charge(polarity, Max(tuning->bladeChargedSelfCharge, 0.0f));

    PlaySlashMotion(polarity);

    // 溜め斬りは通常の «ヒュッ» とは別の音。同じにすると、溜めた一振りが
    // 通常斬りに埋もれて «溜めた意味» が耳から消える。
    se::Play(audio, se::BladeChargeSlash(polarity));

    // 振り出しの重さ。ここで返さないと、溜めた手応えが «当たったとき» まで来ない。
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(chargedRumble * m_chargedRatio, chargedRumble * 0.5f * m_chargedRatio, 0.12f);

    debugPhase = "Charged";
    debugCombo = m_combo;
}

inline void PolarityBladeComponent::NotifyChargeFull()
{
    m_chargeFull = true;

    // «もう溜まった» を耳と手に返す。画面を見ていなくても離す時が分かるようにする。
    se::Play(audio, se::kBladeChargeUpFull);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.0f, 0.9f, 0.06f);
}

// WHY 比の 2 乗で立ち上げるか:
//   線形に強めると、溜め始めた瞬間から震えていて «いつ満ちるのか» が読めない。
//   2 乗にすると前半はほとんど動かず終盤で一気に来るので、
//   «そろそろ» が画面と手から分かる。
inline void PolarityBladeComponent::DriveCharge()
{
    const float ratio = m_holding != Polarity::None && m_charge > 0.0f ? ChargeRatio() : 0.0f;
    debugCharge = ratio;

    if (ratio <= 0.0f) {
        StopChargeFeel();
        return;
    }
    m_chargeFeel = true;

    const float weight = ratio * ratio;

    m_shake.Update(*this, chargeShake * weight, Clamp01(chargeShakeLateral),
                   chargeShakeHz * (0.6f + 0.4f * ratio));

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Sustain(RumbleChannel::BladeCharge, chargeRumble * weight,
                     chargeRumble * weight * 0.4f);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(chargeDistort * weight);

    // 音程が溜め比で上がる。満溜めの合図 (NotifyChargeFull) が鳴る前から、
    // «あと少し» が耳だけで分かる。
    //
    // WHY kPolarityChargedLoop ではなく専用素材か:
    //   あちらは «敵が帯びている» 持続音で、鳴っている主体が違う。加えてこの素材は
    //   振幅の揺れを chargeShakeHz (24Hz) に合わせてあるので、画面の震えと
    //   音のうねりが同じ周期で来る。別の周期どうしを重ねると、速い方が遅い方を
    //   «ずれている» ように聞かせてしまう。
    m_chargeVoice.Update(*this, se::kBladeChargeUpLoop.First(),
                         chargeVoiceVolume * ratio, 0.7f + 0.8f * ratio);

    // 足を鈍らせる。溜めながら全速で走れると «こらえている» が絵から消える。
    if (m_controller)
        m_controller->RequestMoveSpeedScale(
            Lerp(1.0f, Max(tuning->bladeChargeMoveScale, 0.05f), weight));
}

inline void PolarityBladeComponent::StopChargeFeel()
{
    if (!m_chargeFeel) return;
    m_chargeFeel = false;

    m_shake.Stop(*this);
    m_chargeVoice.Stop(*this);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->StopSustain(RumbleChannel::BladeCharge);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(0.0f);
}

inline void PolarityBladeComponent::PlaySlashMotion(Polarity polarity)
{
    // 溜め斬りは両刀を交差させる最終段のクリップを流用する。振り抜きが 1 度きりで
    // 左右の区別が無い動きは、この 1 本だけが持っている。
    const bool finisher = IsFinisher() || m_charged;

    const std::string& clipFile = finisher ? slashFinisherClipFile
                                : polarity == Polarity::Plus ? slashRightClipFile
                                                             : slashLeftClipFile;
    const std::string& clipName = finisher ? slashFinisherClipName
                                : polarity == Polarity::Plus ? slashRightClipName
                                                             : slashLeftClipName;
    if (clipFile.empty()) return;

    // 斬り抜けが判定の瞬間にちょうど来る速さ。bladeStartup を Inspector で縮めれば
    // モーションも同じだけ速くなるので、両者がずれる余地が無い。
    const float hitTime = Max(finisher ? slashFinisherHitTime : slashHitTime, 0.01f);
    const float speed   = hitTime / Max(StartupSeconds(), 0.01f);

    // weight は DriveSlashLayerWeight が Slot のフェードから毎フレーム決める。
    // ここで立てると、フェードインが始まる前に上半身が 1 フレームだけ構えへ飛ぶ。
    animator.PlaySlot(slashLayerName, clipFile, clipName,
                      slashFadeIn, slashFadeOut, speed, /*loop=*/false);
}

// WHY レイヤー weight を Slot の重みに追従させるか:
//   Override レイヤーの最終的な被せ量は layer.weight × ボーンの mask weight で、
//   Slot 自身のフェード量はそこに掛からない (AnimatorSystem::ApplyAnimationLayers)。
//   weight を 1 に固定すると、振り始めた瞬間に上半身が «構え» へ飛び、
//   振り終わりも同じだけ唐突に戻る。Slot の重みをそのまま流せば、
//   フェードの時間を Inspector の Fade In / Out 2 つだけで決められる。
//
//   振っていない間 weight が 0 になるのも同じ仕組みで賄える。0 のとき
//   ApplyAnimationLayers はこのレイヤーを丸ごと飛ばすので、Idle の呼吸も
//   Run_F の前傾もそのまま出る。
inline void PolarityBladeComponent::DriveSlashLayerWeight()
{
    if (slashLayerName.empty()) return;
    const float slot = Clamp01(animator.GetSlotWeight(slashLayerName));
    animator.SetLayerWeight(slashLayerName, slot * Clamp01(slashLayerGain));
}

inline void PolarityBladeComponent::ResolveHit()
{
    const Vector3 origin    = transform.worldPosition;
    const Vector3 direction = m_swingDirection;

    // 溜め斬りだけ «全周»。威力を上げるだけの溜めは «強い通常斬り» でしかなく、
    // 盤面の読みが増えない。周り全部に極を乗せる一撃にすると、溜めは damage を出す手
    // ではなく «盤面を一度に染める手» になり、いつ溜めるかが極性の判断そのものになる。
    const float rangeScale = m_charged
        ? Lerp(1.0f, Max(tuning->bladeChargedRangeScale, 1.0f), m_chargedRatio)
        : 1.0f;
    const float range = Max(tuning->bladeRange, 0.1f) * rangeScale;
    // 扇の «半角» の余弦。合計角度を 2 で割る。全周は -1 (どの向きでも通る)。
    const float halfCos = m_charged
        ? -1.0f
        : std::cos(ToRad(Clamp(tuning->bladeAngleDegrees, 10.0f, 360.0f) * 0.5f));

    const bool finisher = IsFinisher() || m_charged;

    int hits = 0;
    for (GameObject* object : scene.FindObjectsOfType<PolarityTargetComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* target = scene.GetScript<PolarityTargetComponent>(object);
        if (!target) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON) continue;

        // WHY 体の太さを足すか: 判定は中心どうしの距離で測っている。Serpent は
        //     全長 4.5m あるので、中心が射程の外でも胴は目の前にある。
        //     «見えているのに当たらない» が一番読めない失敗になる。
        const float reach = range + bodybounds::RadiusWorld(*object);
        if (distanceSq > reach * reach) continue;

        const float distance = std::sqrt(distanceSq);
        if (Vector3::Dot(delta / distance, direction) < halfCos) continue;

        HitOne(*object, *target);
        ++hits;
    }

    // ボスの部位。盤面のコマ (PolarityTargetComponent) とは別系統なので扇をもう一度通す。
    //
    // WHY 1 つのループにまとめないか: 両者は «極を持つ» ことしか共通していない。
    //     混ぜるには基底クラスを 1 枚挟むことになり、盤面の側 (引力・持続・中和) が
    //     部位の都合を知る形になる。同じ扇を 2 度通す方が、依存の向きが増えない。
    int         parts   = 0;
    int         painted = 0;
    GameObject* struck  = nullptr;
    for (GameObject* object : scene.FindObjectsOfType<BossPartPolarityComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* part = scene.GetScript<BossPartPolarityComponent>(object);
        if (!part) continue;

        Vector3 delta = object->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq < EPSILON) continue;

        // 部位はレンダラーを持たないので bodybounds が使えない。太さは本人が申告する。
        const float reach = range + Max(part->hitRadius, 0.0f);
        if (distanceSq > reach * reach) continue;

        const float distance = std::sqrt(distanceSq);
        if (Vector3::Dot(delta / distance, direction) < halfCos) continue;

        // 芝居も本体側。突き合わせる相手と固める相手は同じなので 1 度だけ引いておく。
        GameObject* root = BossHitboxRigComponent::BossRootOf(object);
        if (!struck) struck = root;

        // 部位の極と刃の極を突き合わせる。塗られる側の節はここで極が乗る。
        const int dealt = static_cast<int>(SlashDamage() * PartDamageScale(*part, root));
        (void)part->Damage(dealt);

        // 部位ごとに極を持つボスは、本体の体力もここから削る (本体を直接斬る道は
        // 塞いである)。削ることと倒すことが別々の作業にならないよう 1 本に通す。
        if (part->selfDriven && root)
            if (auto* combat = CombatManagerComponent::Instance())
                (void)combat->DamageEnemyDirect(root, dealt);
        // 斬った脚が «効いている» を返す。押す向きはプレイヤーから部位への水平方向
        // ── 斬撃の扇の向きだと、横をすり抜けた一撃でも正面へ押すことになる。
        if (auto* rig = scene.GetScript<BossPolarityRigComponent>())
            rig->Flinch(part->legSuffix, object->transform.worldPosition,
                        delta / distance, m_charged);
        // 当たった «瞬間» をその場に出す。環は一発の演出なので、鳴らし続けると
        // «当たっている» ではなく «何かが爆ぜ続けている» に見える。
        if (auto* rings = PolarityRingComponent::Instance())
            rings->Burst(object->transform.worldPosition, 2.0f, m_polarity);
        if (!part->selfDriven) ++painted;
        ++parts;
        ++hits;
    }
    // «極が乗った» 音は塗ったときだけ。ボスの脚は自分で極を持っているので、
    // 合っていたか外したかは kNeutralize / kPolarityRepulse の側が言う。
    // 溜め斬りは複数の節へまとめて乗るので、1 部位ごとに鳴らすと同じ音が重なる。
    if (painted > 0) se::Play(audio, se::kPolarityInfect);

    debugLastHits = hits;

    // 軌跡は当たっても外れても出す。空振りだけ何も走らないと、外したことより先に
    // «入力が拾われていない» に見える。当たったかどうかは白熱の有無で返す。
    //
    // WHY 扇をそのまま渡すか: 弧が届く先と角度を軌跡側にもう 1 組持たせると、
    //     片方を触るたびに «光っているのに当たらない» が生まれる。上で判定に使った
    //     range と、halfCos を作った角度と同じ値を渡す ─ 溜め斬りは射程が伸びて
    //     全周になるので、その一振りの実測値でなければ揃わない。
    if (m_slashArc) {
        const float arcDegrees = m_charged
            ? 360.0f
            : Clamp(tuning->bladeAngleDegrees, 10.0f, 360.0f);
        m_slashArc->Play(origin, direction, m_polarity, finisher, hits > 0,
                         range, arcDegrees);
    }

    // 溜め斬りは当たらなくても «薙いだ» ことを返す。全周に届く一撃で何も起きないと、
    // 溜めていた時間ごと «無かったこと» になる。環は届いた範囲そのものなので嘘がない。
    if (m_charged) {
        if (auto* rings = PolarityRingComponent::Instance())
            rings->Burst(origin, range, m_polarity);
        // 当たったときの揺れは下でまとめて出す。ここは空振りぶんだけ。
        if (hits == 0)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Shake(Clamp01(chargedShake * m_chargedRatio * 0.6f));
    }

    if (hits > 0) {
        // 斬撃の止めは «弱»。衝突 (引力の激突) の強い止めと混ぜてはいけない。
        // 溜め斬りだけはその上限を超える ─ 溜めた時間が «重さ» として返る唯一の場所で、
        // ここを通常と同じにすると溜める理由が手触りから消える。
        const float stopStrength = m_charged
            ? Lerp(Clamp01(hitStop), Clamp01(chargedHitStop), m_chargedRatio)
            : Clamp01(hitStop);
        if (auto* stop = HitstopManagerComponent::Instance()) {
            stop->Hit(stopStrength);
            // 世界の止めとは別に、当事者の芝居だけを固める。振り抜いた腕と斬られた
            // 体が «食い込んで止まる» ことで «当たった» が出る ─ 全体の止めを
            // 深くしてこれを作ろうとすると、カメラも粒子も一緒に固まってテンポが先に壊れる。
            stop->FreezeAnimation(scene.Self(), stopStrength);
            if (struck) stop->FreezeAnimation(struck, stopStrength);
        }
        if (auto* shake = CameraShakeManagerComponent::Instance())
            shake->Shake(m_charged ? Clamp01(chargedShake) : Clamp01(hitShake));
        if (auto* pad = RumbleManagerComponent::Instance()) {
            const float strength = m_charged ? Clamp01(chargedRumble) : Clamp01(hitRumble);
            pad->Rumble(strength, strength * 0.6f, m_charged ? 0.18f : 0.07f);
        }
        // 締めの一撃だけ重い音。連撃が «終わった» ことを、画面を見ずに判るようにする。
        // 溜め斬りは段を持たないので、常に締め扱いでいい (BeginCharged が m_combo=0 にする)。
        const bool heavy = m_charged || IsFinisher();
        se::Play(audio, heavy ? se::BladeHitFinish(m_polarity) : se::BladeHit(m_polarity),
                 m_charged ? 1.0f + 0.4f * m_chargedRatio : 1.0f);
    }

    // 判定そのものを線で出す。軌跡と重ねて «影が一致しているか» を目で確かめるためで、
    // 数字を突き合わせても «傾けた弧が水平にどこまで届いているか» は読めない。
    // 体の太さ (bodybounds) を足す前の素の扇なので、大きい敵はこの線の外でも当たる。
    if (drawDebugArc) {
        const Vector4 color   = PolarityColor(m_polarity);
        const float   degrees = m_charged
            ? 360.0f
            : Clamp(tuning->bladeAngleDegrees, 10.0f, 360.0f);
        const float half  = ToRad(degrees * 0.5f);
        const Vector3 right =
            Vector3::Cross(Vector3::UP, direction).NormalizedOr(Vector3::RIGHT);

        constexpr int kFanSegments = 24;
        Vector3 previous = origin;
        for (int i = 0; i <= kFanSegments; ++i) {
            const float t     = static_cast<float>(i) / static_cast<float>(kFanSegments);
            const float angle = Lerp(half, -half, t);
            const Vector3 edge = origin
                + (direction * std::cos(angle) + right * std::sin(angle)) * range;
            if (i > 0) debug.DrawLine(previous, edge, color);
            previous = edge;
        }
        // 扇の両縁。全周では中心と縁を結ぶ線が向きの表示にしかならないので出さない。
        if (degrees < 300.0f) {
            debug.DrawLine(origin,
                (direction * std::cos(half) + right * std::sin(half)) * range + origin, color);
            debug.DrawLine(origin,
                (direction * std::cos(half) - right * std::sin(half)) * range + origin, color);
        }
    }
}

inline void PolarityBladeComponent::HitOne(GameObject& object,
                                           PolarityTargetComponent& target)
{
    // 極を乗せる。中和・上書き・付与の判定は極性システム側が 1 箇所で持っている。
    (void)target.Apply(m_polarity);

    // 削る。立っていても倒れていても通る。
    //
    // WHY 立っている間も通すか: 斬撃は «極を乗せるため» の手だったので、削れるのを
    //     転倒中だけに絞ってあった。その結果、戦闘時間の大半でプレイヤーが一番多く押す
    //     入力が何も返さない状態になっていた。«斬れば削れる» を土台に戻し、どこを削ったかは
    //     部位ごとの体力 (BossPartPolarityComponent) が受け持つ。
    // 部位ごとに極を持つボス (四足) は、削るのが部位の側の仕事。ここで本体へ直接
    // 通すと «色を読まずに胴を斬るだけ» が最短になり、部位の色分けが飾りに落ちる。
    if (scene.GetScript<BossPolarityRigComponent>(&object)) return;

    if (auto* combat = CombatManagerComponent::Instance())
        (void)combat->DamageEnemyDirect(&object, SlashDamage());
}

inline void PolarityBladeComponent::DashToTarget()
{
    // 溜め斬りはその場で全周を薙ぐ手なので詰めない。踏み込むと «溜めて突っ込む»
    // という別の技になり、全周である意味が消える。
    if (m_charged || !m_controller) return;

    const float dashRange = Max(tuning->bladeDashRange, 0.0f);
    if (dashRange <= 0.0f) return;

    GameObject* target = AimTarget();
    if (!target) return;

    Vector3 delta = target->transform.worldPosition - transform.worldPosition;
    delta.y = 0.0f;
    const float distance = delta.Length();
    if (distance < EPSILON) return;

    // 届く距離の測り方は判定 (ResolveHit) とまったく同じにする。別々に持つと
    // «踏み込んだのに当たらない» / «届いているのに踏み込む» が両方起きる。
    const float reach = Max(tuning->bladeRange, 0.0f) + bodybounds::RadiusWorld(*target);
    const float gap   = distance - reach;
    if (gap <= 0.0f || gap > dashRange) return;

    const float travel = gap + Max(tuning->bladeDashDepth, 0.0f);
    // 発生のあいだで払いきる速さ。上限で頭を打つので、遠いほど速く滑ることはない。
    const float seconds = Max(StartupSeconds(), 0.02f);
    const float speed   = std::min(travel / seconds, Max(tuning->bladeDashSpeed, 0.0f));
    if (speed <= 0.0f) return;

    m_controller->Knockback(delta / distance, speed, seconds);
}

inline float PolarityBladeComponent::PartDamageScale(BossPartPolarityComponent& part,
                                                     GameObject* bossRoot)
{
    // 塗られる側 (蛇の節) は今までどおり。極を乗せるのが仕事で、突き合わせは無い。
    if (!part.selfDriven) {
        part.Apply(m_polarity);
        return 1.0f;
    }

    const Polarity worn = part.Current();
    // 打ち消した直後は無極。どちらの剣でも満額で通る ─ 読み切った数秒の報酬。
    if (worn == Polarity::None || m_polarity == Polarity::None) return 1.0f;

    if (worn != m_polarity) {
        part.Neutralize();
        if (!m_matchResolved) {
            m_matchResolved = true;
            se::Play(audio, se::kNeutralize);
            // 合わせた «瞬間» を止めで返す。通常の斬撃より一段深くして、
            // «正しい剣だった» を手で分かるようにする。
            if (auto* stop = HitstopManagerComponent::Instance())
                stop->Hit(Clamp01(hitStop * 1.6f));
        }
        return 1.0f;
    }

    // 同極。通る量が落ち、こちらが弾かれる。
    if (!m_matchResolved) {
        m_matchResolved = true;

        const float bounce = Max(tuning->bladeBounceSpeed, 0.0f);
        if (bounce > 0.0f && m_controller && bossRoot) {
            Vector3 away = transform.worldPosition - bossRoot->transform.worldPosition;
            away.y = 0.0f;
            // 弾かれる長さは硬直と揃える。硬直より長いと «動けるのに勝手に下がる» になり、
            // 短いと «弾かれた» が絵に残らない。
            m_controller->Knockback(away.NormalizedOr(-m_swingDirection), bounce,
                                    Max(tuning->bladeRecovery, 0.05f));
        }
        se::Play(audio, se::kPolarityRepulse);
        if (auto* pad = RumbleManagerComponent::Instance())
            pad->Rumble(0.55f, 0.3f, 0.12f);
    }

    return Clamp01(tuning->bladeSamePolarityScale);
}

inline int PolarityBladeComponent::SlashDamage() const
{
    const int base = std::max(tuning->bladeDamage, 0);
    if (!m_charged) return base;
    return static_cast<int>(Lerp(static_cast<float>(base),
                                 static_cast<float>(std::max(tuning->bladeChargedDamage, 0)),
                                 m_chargedRatio));
}

inline void PolarityBladeComponent::OnUpdate()
{
    if (!enabled || !tuning) return;

    const float dt = Max(Time::deltaTime, 0.0f);

    // 硬直が明けてもクリップは振り抜きの途中に居る。段の状態機械とは切り離して、
    // Slot が畳まれるまで毎フレーム面倒を見る。
    DriveSlashLayerWeight();

    ReadInput();
    // 溜めは段の状態機械とは別に走る。振っている最中でも硬直中でも溜まり続け、
    // 離した瞬間だけがどちらにも割り込む。
    DriveCharge();

    // 振り出しから斬り抜けまで、体は刃と同じ向きを向く。吸い付き補正を 0 にすると
    // 斬る向きはカメラの正面になるので、ここで体を連れて行かないと «体は敵を向いて
    // いるのに刃は画面の奥へ抜ける» という食い違いが残る。
    if (m_phase == Phase::Startup && m_controller)
        m_controller->RequestFacing(m_swingDirection);

    switch (m_phase) {
    case Phase::Startup:
        m_timer -= dt;
        if (m_timer > 0.0f) break;
        ResolveHit();
        m_phase   = Phase::Recovery;
        // 最終段だけ長い硬直。«止めた» ことが手に返らないと、繋げる意味が出ない。
        m_timer   = m_charged      ? Max(tuning->bladeChargedRecovery, 0.02f)
                  : IsFinisher()   ? Max(tuning->bladeRecovery, 0.02f)
                                   : Max(tuning->bladeComboRecovery, 0.02f);
        // 溜め斬りは段を進めない。溜めが «連撃の 4 段目» になると、
        // 溜めるかどうかの判断が «繋がっているか» に飲み込まれる。
        m_combo   = m_charged ? 0 : (m_combo + 1) % ComboLength();
        // 硬直が明けてからも猶予がある。押しっぱなしで繋がらない長さに留める。
        m_comboExpire = Time::time + m_timer + Max(tuning->bladeComboWindow, 0.0f);
        debugPhase = "Recovery";
        break;

    case Phase::Recovery:
        m_timer -= dt;
        if (m_timer > 0.0f) break;
        m_phase    = Phase::Idle;
        m_polarity = Polarity::None;
        m_charged  = false;
        m_chargedRatio = 0.0f;
        debugPhase = "Idle";
        // 溜めておいた入力をここで出す。捨てると繋げようとするほど手数が減る。
        if (m_buffered != Polarity::None) BeginSwing(m_buffered);
        break;

    case Phase::Idle:
        if (Time::time > m_comboExpire && m_combo != 0) {
            m_combo    = 0;
            debugCombo = 0;
        }
        break;
    }

    // ボスが読む «今なにをしているか»。最終段の硬直が一番長いので、そこが差し込みどころ。
    playeraction::Publish(m_phase != Phase::Idle,
                          m_phase != Phase::Idle && m_swingIsFinisher,
                          m_phase == Phase::Recovery,
                          m_holding != Polarity::None ? ChargeRatio() : 0.0f,
                          Time::time);
}

} // namespace sandbox
