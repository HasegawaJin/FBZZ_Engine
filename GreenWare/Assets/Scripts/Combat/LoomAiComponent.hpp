/// @file    LoomAiComponent.hpp
/// @brief   ボス 3「ポラリティ・ルーム」の状態機械。連撃・弾き・崩し・とどめ
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ⚠ 骨だけ。設計は Docs/boss-loom.md。モデル / リグ / アニメ / VFX は無い。
///   ここに入っているのは «時刻と判定と配線» で、絵は後から差す前提になっている。
///   絵が無くても 4 連が振られ、弾けて、崩れて、腕がもげるところまでは動く。
///
/// WHY 絵より先に時刻を作るか (Docs/boss-loom.md「作る順」):
///   このボスの新しさは «連撃のどこに居るか» を読ませることで、それは拍の間隔
///   (0.62 秒) と弾きの窓 (0.22 秒) の比でしか決まらない。絵を先に揃えると、
///   読めない原因が «モーションが悪いのか拍が悪いのか» に割れて追えなくなる。
///
/// WHY 1 発ごとに «弾ける手» を出すか:
///   連撃の 1 拍は CombatManager へ PlayerHitKind::Parryable で渡す。弾かれた側は
///   OnParried が受け、その拍だけ潰して次の拍へ進む ─ 連撃は止まらない。
///   止めてしまうと «1 回弾けば安全» になり、連続弾きが働かない。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 打腕の本数。部位の数であり、連撃の最大の長さでもある。
inline constexpr int kLoomArmCount = 4;

class LoomAiComponent : public Script {
    FBZZ_SCRIPT(LoomAiComponent)

public:
    FBZZ_GROUP("戦闘開始")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD(bool, engageOnStart, true, "Engage On Start")
    FBZZ_TOOLTIP("false にすると BossRoomTriggerComponent が起こすまで眠る")
    FBZZ_FIELD_RANGE(float, descendSeconds, 2.4f, "Descend", 0.0f, 8.0f)
    FBZZ_TOOLTIP("天井の輪から降りてくる尺。登場カメラの尺と揃えること")

    FBZZ_GROUP("Beat (弾ける手)")
    FBZZ_FIELD_RANGE(float, beatWindup, 0.46f, "溜め", 0.05f, 2.0f)
    FBZZ_TOOLTIP("腕が光ってから振り下ろすまで。弾きの窓 0.22 秒より長いこと")
    FBZZ_FIELD_RANGE(float, beatInterval, 0.62f, "Interval (4 arms)", 0.2f, 2.0f)
    FBZZ_TOOLTIP("腕 4 本のときの拍の間隔。腕が減るごとに intervalStep ずつ縮む")
    FBZZ_FIELD_RANGE(float, beatIntervalStep, 0.07f, "Interval Step", 0.0f, 0.3f)
    FBZZ_TOOLTIP("腕が 1 本減るごとに拍を縮める量。短くなるぶんを速さで返す")
    FBZZ_FIELD_RANGE(float, finisherHold, 0.34f, "Finisher Hold", 0.0f, 1.5f)
    FBZZ_TOOLTIP("締めの前だけ拍を飛ばす溜め。連打で流しているとここで外す")
    FBZZ_FIELD_RANGE(float, beatRadius, 4.2f, "半径", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE_INT(int, beatDamage, 1, "ダメージ", 0, 20)
    FBZZ_FIELD_RANGE_INT(int, finisherDamage, 2, "Damage (finisher)", 0, 20)
    FBZZ_FIELD_RANGE(float, beatRecover, 1.15f, "復帰", 0.0f, 5.0f)
    FBZZ_TOOLTIP("連撃を振り切った後の隙。ここが斬りに行く時間")

    FBZZ_GROUP("Shuttle (跳ぶ手)")
    FBZZ_FIELD_RANGE(float, shuttleWindup, 0.85f, "溜め", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, shuttleSeconds, 1.30f, "移動軌跡", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, shuttleRadius, 3.0f, "半径", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE(float, shuttleHeight, 1.1f, "クリアランス高さ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("この高さより上に居れば当たらない。跳んで越える手なので «上» が要る")
    FBZZ_FIELD_RANGE_INT(int, shuttleDamage, 2, "ダメージ", 0, 20)

    FBZZ_GROUP("Curtain (走る手)")
    FBZZ_FIELD_RANGE(float, curtainWindup, 1.15f, "溜め", 0.05f, 4.0f)
    FBZZ_FIELD_RANGE(float, curtainSeconds, 1.60f, "保持", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, curtainRadius, 6.5f, "半径", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE_INT(int, curtainDamage, 2, "ダメージ", 0, 20)
    FBZZ_FIELD_RANGE(float, curtainTick, 0.45f, "Damage Interval", 0.05f, 2.0f)
    FBZZ_TOOLTIP("居座っている間の刻み。毎フレームだと一瞬触れただけで溶ける")

    FBZZ_GROUP("Rhythm")
    FBZZ_FIELD_RANGE(float, idleSeconds, 1.05f, "待機", 0.0f, 5.0f)
    FBZZ_TOOLTIP("手と手の間。短いと «休みが無い»、長いと間延びする")
    FBZZ_FIELD_RANGE(float, tempo, 1.0f, "テンポ", 0.25f, 3.0f)
    FBZZ_TOOLTIP("全部の尺に掛かる。手触りを丸ごと速く / 遅くする唯一のつまみ")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Dormant", "状態")
    FBZZ_FIELD_READ_ONLY(int, debugArms, kLoomArmCount, "腕")
    FBZZ_FIELD_READ_ONLY(int, debugBeat, 0, "Beat")
    FBZZ_FIELD_READ_ONLY(std::string, debugLastHit, "-", "直前のヒット")
    FBZZ_FIELD_READ_ONLY(std::string, debugReaction, "-", "反応")

    // ---- IBoss が読む口 (LoomBossComponent が中継する) ----

    [[nodiscard]] bool IsEngaged() const { return m_state != State::Dormant; }
    /// 硬直中。連撃を振り切った後の隙と、倒れている間。
    [[nodiscard]] bool IsStaggered() const
    {
        return m_state == State::BeatRecover || m_state == State::Toppled;
    }
    [[nodiscard]] bool IsToppled() const { return m_state == State::Toppled; }
    /// 残りの打腕。進行の物差し。
    [[nodiscard]] int ArmsRemaining() const { return m_arms; }
    /// 腕 2 本以下で第 2 段。フェーズ変数は持たない (サーペントと同じ形)。
    [[nodiscard]] int CurrentPhase() const { return m_arms <= 2 ? 2 : 1; }

    /// 崩しゲージが満ちた。BossBreakComponent::onBreak から呼ばれる。
    void Topple(float seconds);
    /// 一撃を弾かれた。その拍だけ潰し、連撃は止めない。
    void OnParried(const Vector3& hitPoint);
    /// 倒れている間の とどめ。腕を 1 本落とせたら true。
    bool Execute(GameObject* part, const Vector3& from);

    void OnStart() override;
    void OnUpdate() override;

private:
    enum class State {
        Dormant,        ///< まだ相手ではない
        Descend,        ///< 天井の輪から降りてくる
        Idle,           ///< 次の手を選ぶ
        BeatWindup,     ///< 腕が光る。ここから弾きの窓が意味を持つ
        Beat,           ///< 振り下ろす。当たるかどうかはこの 1 フレーム
        BeatHold,       ///< 締めの前だけ拍を飛ばす溜め
        BeatRecover,    ///< 振り切った後の隙。斬りに行く時間
        ShuttleWindup,
        Shuttle,
        CurtainWindup,
        Curtain,
        Toppled,        ///< 崩れて晒している。とどめが通る
        Dead,
    };

    void Enter(State next);
    void TickDescend(float dt);
    void TickIdle(float dt);
    void TickBeatWindup(float dt);
    void TickBeat(float dt);
    void TickBeatHold(float dt);
    void TickBeatRecover(float dt);
    void TickShuttleWindup(float dt);
    void TickShuttle(float dt);
    void TickCurtainWindup(float dt);
    void TickCurtain(float dt);
    void TickToppled(float dt);

    /// 連撃を組み直す。腕の本数がそのまま拍の数になる。
    void BeginString();
    /// 次の拍へ。締めの前だけ溜めを挟む。
    void AdvanceBeat();
    /// 振り上げへ入る。1 拍目だけ長く見せ、2 拍目以降は拍の間隔そのものになる。
    void EnterBeatWindup();

    /// 今の拍の間隔。腕が減るほど短い (Docs/boss-loom.md「進行は連撃の長さで見える」)。
    [[nodiscard]] float BeatInterval() const
    {
        const int lost = std::max(kLoomArmCount - m_arms, 0);
        return std::max(beatInterval - beatIntervalStep * static_cast<float>(lost), 0.15f);
    }
    /// 今の拍が締めか。
    [[nodiscard]] bool IsFinisher() const { return m_beat >= m_beatsTotal - 1; }

    [[nodiscard]] GameObject* Player() const { return m_player.Resolve(scene); }
    void RefreshPlayer();
    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] BossBreakComponent* Break() const
    {
        return scene.GetScript<BossBreakComponent>();
    }
    /// 満ちたら倒れる、を 1 度だけ結ぶ。
    void EnsureBreakHook();

    /// 半径のなかに居るプレイヤーを叩く。当たったかどうかを debugLastHit へ写す。
    PlayerHitResult HitPlayerInSphere(const Vector3& center, float radius, int amount,
                                      PlayerHitKind kind, float minHeight = -1.0f);

    static inline const char* StateName(State s)
    {
        switch (s) {
        case State::Dormant:        return "Dormant";
        case State::Descend:        return "Descend";
        case State::Idle:           return "Idle";
        case State::BeatWindup:     return "BeatWindup";
        case State::Beat:           return "Beat";
        case State::BeatHold:       return "BeatHold";
        case State::BeatRecover:    return "BeatRecover";
        case State::ShuttleWindup:  return "ShuttleWindup";
        case State::Shuttle:        return "Shuttle";
        case State::CurtainWindup:  return "CurtainWindup";
        case State::Curtain:        return "Curtain";
        case State::Toppled:        return "Toppled";
        case State::Dead:           break;
        }
        return "Dead";
    }

    State     m_state      = State::Dormant;
    float     m_timer      = 0.0f;
    /// 今の状態が終わるまでの尺。入るときに決める (拍ごとに変わるため)。
    float     m_duration   = 0.0f;
    float     m_toppleLeft = 0.0f;
    float     m_curtainElapsed = 0.0f;
    int       m_arms       = kLoomArmCount;
    int       m_beat       = 0;      ///< 今の拍 (0 始まり)
    int       m_beatsTotal = kLoomArmCount;
    /// この拍は弾かれたか。判定には使わない (上の ⚠ を参照) ─ 絵を差すときに
    /// «この腕は弾き返された» を知るための記録。
    bool      m_beatParried = false;
    /// 手の巡回。連撃 → 梭 → 連撃 → 幕 の順で回す。
    int       m_cycle      = 0;
    bool      m_breakHooked = false;
    Vector3   m_target     = {};     ///< 予兆を出した時点のプレイヤーの位置
    EntityRef m_player;

    /// 予兆の «時刻»。床のデカールも部位発光もこの 1 つから引く。
    BossTelegraphCue m_cue;
};

FBZZ_REFLECT(LoomAiComponent)

// ---------------------------------------------------------------------------

inline void LoomAiComponent::OnStart()
{
    RefreshPlayer();
    m_arms       = kLoomArmCount;
    m_beatsTotal = kLoomArmCount;
    Enter(engageOnStart ? State::Descend : State::Dormant);
}

inline void LoomAiComponent::RefreshPlayer()
{
    // 毎フレーム取り直す。プレイヤーが作り直される構成でも繋がり直る。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag))
        m_player = EntityRef{ player->GetID() };
}

inline bool LoomAiComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline void LoomAiComponent::EnsureBreakHook()
{
    if (m_breakHooked) return;
    auto* brk = Break();
    if (!brk) return;
    // 満ちたら倒れる。長さはゲージの側が持つ (ボスごとに違ってよい値なのでそちらへ)。
    brk->onBreak = [this](float seconds) { Topple(seconds); };
    m_breakHooked = true;
}

inline void LoomAiComponent::Enter(State next)
{
    m_state    = next;
    m_timer    = 0.0f;
    m_duration = 0.0f;   // 入った側が決める。0 のままなら «尺を持たない» 状態
    debugState = StateName(next);
    m_cue.Reset();
}

inline void LoomAiComponent::OnUpdate()
{
    // WHY 尺ではなく dt に tempo を掛けるか (BossAiComponent と同じ形):
    //   尺の方を割ると、比較する箇所すべてで割り算が要る。dt を伸縮させれば
    //   Inspector の秒数は «tempo 1.0 のときの秒» という 1 つの意味に固定できる。
    const float dt = std::max(time.DeltaTime(), 0.0f) * std::max(tempo, 0.0f);
    RefreshPlayer();
    EnsureBreakHook();

    if (!IsAlive()) {
        if (m_state != State::Dead) Enter(State::Dead);
        return;
    }

    debugArms = m_arms;
    debugBeat = m_beat;
    m_timer += dt;

    switch (m_state) {
    case State::Descend:       TickDescend(dt);       break;
    case State::Idle:          TickIdle(dt);          break;
    case State::BeatWindup:    TickBeatWindup(dt);    break;
    case State::Beat:          TickBeat(dt);          break;
    case State::BeatHold:      TickBeatHold(dt);      break;
    case State::BeatRecover:   TickBeatRecover(dt);   break;
    case State::ShuttleWindup: TickShuttleWindup(dt); break;
    case State::Shuttle:       TickShuttle(dt);       break;
    case State::CurtainWindup: TickCurtainWindup(dt); break;
    case State::Curtain:       TickCurtain(dt);       break;
    case State::Toppled:       TickToppled(dt);       break;
    case State::Dormant:
    case State::Dead:          break;
    }
}

inline void LoomAiComponent::TickDescend(float dt)
{
    (void)dt;
    // TODO(絵): 天井の輪から降りる動き。BossCameraDirectorComponent の «登場» と尺を揃える。
    if (m_timer >= descendSeconds) Enter(State::Idle);
}

inline void LoomAiComponent::TickIdle(float dt)
{
    (void)dt;
    if (m_timer < idleSeconds) return;

    // 連撃 → 梭 → 連撃 → 幕 の 4 拍で回す。
    //
    // WHY 乱数で選ばないか: このボスの読みは «連撃の何拍目か» で、手の選択そのものは
    //     読ませたい対象ではない。乱数にすると «次に何が来るか» の不安が連撃の
    //     数え上げに割り込む。決まった順に回せば、覚えた先に必ず答えがある
    //     (Docs/企画書.md「手ごとに、正解の動作が違う」)。
    switch (m_cycle % 4) {
    case 1:  Enter(State::ShuttleWindup); break;
    case 3:  Enter(State::CurtainWindup); break;
    default: BeginString();               break;
    }
    ++m_cycle;
}

inline void LoomAiComponent::BeginString()
{
    // 腕の数がそのまま拍の数。もいだぶん連撃が短くなる。
    m_beatsTotal = std::max(m_arms, 1);
    m_beat       = 0;
    EnterBeatWindup();
}

inline void LoomAiComponent::EnterBeatWindup()
{
    Enter(State::BeatWindup);
    // WHY 1 拍目だけ長いか: 連撃の «始まり» は読ませる必要がある。2 拍目以降は
    //     «次が来ること» が既に分かっているので、間隔がそのまま振り上げになる。
    //     ここを全部 beatWindup にすると、拍の間隔が windup で決まってしまい、
    //     腕が減っても速くならない。
    m_duration    = (m_beat == 0) ? std::max(beatWindup, 0.05f) : BeatInterval();
    m_beatParried = false;
}

inline void LoomAiComponent::TickBeatWindup(float dt)
{
    m_cue.Tick(m_duration > 0.0f ? m_timer / m_duration : 1.0f, dt, true);
    // TODO(絵): m_beat 番目の腕を光らせる。締めは 2 本同時 (Docs/boss-loom.md)。

    if (m_timer < m_duration) return;
    if (GameObject* player = Player()) m_target = player->transform.worldPosition;
    Enter(State::Beat);
}

inline void LoomAiComponent::TickBeat(float dt)
{
    (void)dt;
    // 振り下ろしは 1 フレームで解決する。判定を尺で持つと «もう避けたのに当たる» が出る。
    const int amount = IsFinisher() ? finisherDamage : beatDamage;

    // ⚠ 弾かれたかどうかを «叩く前» に見てはいけない。
    //   OnParried はこの呼び出しの «中» で走る:
    //     HitPlayerInSphere → CombatManager::HitPlayer → PlayerComponent::ReceiveHit
    //       → PlayerParryComponent::OnParried → IBoss::OnParried → ここの OnParried
    //   つまり «弾かれたから判定を出さない» は書けない。常に振って、
    //   通ったかどうかは返り値と OnParried が決める (BossAiComponent の踏みつけと同じ形)。
    (void)HitPlayerInSphere(m_target, beatRadius, amount, PlayerHitKind::Parryable);

    AdvanceBeat();
}

inline void LoomAiComponent::AdvanceBeat()
{
    ++m_beat;
    if (m_beat >= m_beatsTotal) { Enter(State::BeatRecover); return; }

    // 締めの «直前» だけ拍を飛ばす。等間隔を崩す唯一の場所 ─ 連打で流していると
    // ここで必ず外す (Docs/boss-loom.md「WHY 締めだけ拍を飛ばすか」)。
    if (m_beat == m_beatsTotal - 1 && finisherHold > 0.0f) { Enter(State::BeatHold); return; }
    EnterBeatWindup();
}

inline void LoomAiComponent::TickBeatHold(float dt)
{
    (void)dt;
    // TODO(絵): 腕を上げたまま止める。«来ない» ことが見えている必要がある。
    if (m_timer >= finisherHold) EnterBeatWindup();
}

inline void LoomAiComponent::TickBeatRecover(float dt)
{
    (void)dt;
    // ここが IsStaggered。斬りに行く時間 (Docs/break-parry.md)。
    if (m_timer >= beatRecover) Enter(State::Idle);
}

inline void LoomAiComponent::TickShuttleWindup(float dt)
{
    m_cue.Tick(shuttleWindup > 0.0f ? m_timer / shuttleWindup : 1.0f, dt, true);
    // TODO(絵): 床に Line の帯 (BossAttackKind::Sweep)。梭が走る道を先に見せる。
    if (m_timer >= shuttleWindup) Enter(State::Shuttle);
}

inline void LoomAiComponent::TickShuttle(float dt)
{
    (void)dt;
    // 跳んで越える手。shuttleHeight より上に居れば当たらない。
    // WHY 高さで判定するか: 弾けない手であることを «刀が届かない低さ» で言う。
    //     半径だけで見ると、跳んでいても足が拾われて «跳んだのに当たった» になる。
    (void)HitPlayerInSphere(transform.worldPosition, shuttleRadius, shuttleDamage,
                            PlayerHitKind::Unblockable, shuttleHeight);
    if (m_timer >= shuttleSeconds) Enter(State::Idle);
}

inline void LoomAiComponent::TickCurtainWindup(float dt)
{
    m_cue.Tick(curtainWindup > 0.0f ? m_timer / curtainWindup : 1.0f, dt, true);
    // TODO(絵): 床に Circle (BossAttackKind::Erupt)。張られる区画を先に見せる。
    if (m_timer < curtainWindup) return;
    if (GameObject* player = Player()) m_target = player->transform.worldPosition;
    m_curtainElapsed = 0.0f;
    Enter(State::Curtain);
}

inline void LoomAiComponent::TickCurtain(float dt)
{
    // 居座っている間だけ刻んで入る。走って外へ出るのが答え。
    m_curtainElapsed += dt;
    if (m_curtainElapsed >= curtainTick) {
        m_curtainElapsed = 0.0f;
        (void)HitPlayerInSphere(m_target, curtainRadius, curtainDamage,
                                PlayerHitKind::Unblockable);
    }
    if (m_timer >= curtainSeconds) Enter(State::Idle);
}

inline void LoomAiComponent::TickToppled(float dt)
{
    m_toppleLeft -= dt;
    if (m_toppleLeft <= 0.0f) Enter(State::Idle);
}

// ---------------------------------------------------------------------------

inline void LoomAiComponent::Topple(float seconds)
{
    if (!IsAlive() || m_state == State::Toppled) return;
    m_toppleLeft  = std::max(seconds, 0.5f);
    debugReaction = "Toppled";
    // TODO(絵): 吊りが緩んで腕が垂れる。ToppleVfxComponent と BossCameraDirector の «転倒»。
    Enter(State::Toppled);
}

inline void LoomAiComponent::OnParried(const Vector3& hitPoint)
{
    (void)hitPoint;
    if (!IsAlive()) return;

    switch (m_state) {
    case State::BeatWindup:
    case State::Beat:
        // その拍だけ潰す。連撃は止めない ─ 止めると «1 回弾けば安全» になり、
        // 連続弾き (+15% × 4) が働かない (Docs/boss-loom.md)。
        m_beatParried = true;
        debugReaction = IsFinisher() ? "Parried (finisher)" : "Parried (beat)";
        // TODO(絵): 弾かれた腕が跳ね上がる。ReactToHit 相当。
        break;

    default:
        // 弾ける手は連撃だけ。ここへ来るのは配線の間違いなので残しておく。
        debugReaction = "Parried (?)";
        break;
    }
}

inline bool LoomAiComponent::Execute(GameObject* part, const Vector3& from)
{
    (void)part;
    (void)from;
    // とどめが通るのは倒れている間だけ。判定は PlayerParryComponent が持っているが、
    // 二重に守る ─ ここが緩むと «立っている腕がもげる» が作れてしまう。
    if (m_state != State::Toppled || m_arms <= 0) return false;

    --m_arms;
    debugArms     = m_arms;
    debugReaction = "Executed";
    // TODO(絵): part の腕を畳んで BossPartDebrisComponent 相当へ渡す。

    // 入った瞬間に起きる (Docs/break-parry.md「とどめ」)。
    if (m_arms <= 0) {
        // 最後の 1 本。進行の終わりは EnemyHealthComponent が決めるので、そちらを空にする。
        if (auto* health = scene.GetScript<EnemyHealthComponent>())
            health->ApplyDamage(health->MaxHealth());
        Enter(State::Dead);
    } else {
        Enter(State::Idle);
    }
    return true;
}

// ---------------------------------------------------------------------------

inline PlayerHitResult LoomAiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                                          int amount, PlayerHitKind kind,
                                                          float minHeight)
{
    GameObject* player = Player();
    if (!player || amount <= 0) return PlayerHitResult::Ignored;

    const Vector3 playerPos = player->transform.worldPosition;
    Vector3 toPlayer = playerPos - center;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();

    char note[80] = {};
    if (distance > radius) {
        std::snprintf(note, sizeof(note), "%s %.1f/%.1fm out", debugState.c_str(), distance, radius);
        debugLastHit = note;
        return PlayerHitResult::Ignored;
    }
    // 跳んで越える手だけが高さを持つ。負なら «高さを見ない»。
    if (minHeight >= 0.0f && (playerPos.y - center.y) >= minHeight) {
        std::snprintf(note, sizeof(note), "%s cleared %.2fm", debugState.c_str(),
                      playerPos.y - center.y);
        debugLastHit = note;
        return PlayerHitResult::Ignored;
    }

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        debug.LogError("LoomAiComponent found no CombatManagerComponent in the scene. "
                       "Loom attacks deal no damage.");
        return PlayerHitResult::Ignored;
    }
    // 押しはボスの位置から外へ。«ルームに弾かれた» が正しい向き。
    const Vector3 source = transform.worldPosition;
    const PlayerHitResult result = combat->HitPlayer(player, amount, &source, kind);

    const char* word = result == PlayerHitResult::Damaged ? "hit"
                     : result == PlayerHitResult::Parried ? "PARRIED"
                     : result == PlayerHitResult::Dodged  ? "dodged" : "blocked";
    std::snprintf(note, sizeof(note), "%s %.1f/%.1fm %s", debugState.c_str(),
                  distance, radius, word);
    debugLastHit = note;
    return result;
}

} // namespace sandbox
