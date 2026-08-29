/// @file    WaveDirectorComponent.hpp
/// @brief   アリーナ 4 Wave → ボス、という進行を刻む
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY 要るか (Docs/game-flow.md):
///   これまでの GameFlowComponent は «シーンに置いた敵を全滅させたら Result へ» の
///   1 段しかなく、進行のリズムが存在しなかった。難易度カーブも無く、最初から最後まで
///   同じ密度の掃除作業になっていた。
///
/// WHY 難易度を «配置の散らばり» で作るか:
///   反発 (6m) と引力 (12m) は作用する距離が違う。敵が固まっていれば同極を 1 本
///   なぞるだけで全員弾け、散っていれば 1 体ずつ帯電させてアンカーへ集束させることになる。
///   つまり «散らばり» を動かすだけで、有効な手そのものが入れ替わる。背景を 1 枚も
///   足さずに 4 段階のカーブが作れるのはこのためで、敵の種類を増やす必要が無い。
///
/// WHY 進行の «終わり» をここが決めないか:
///   リザルトへ行くかどうかは GameFlowComponent の担当。ここは «今どの Wave か» と
///   «全部終わったか» を答えるだけで、シーン遷移も勝敗も持たない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/ArenaBoundsComponent.hpp>
#include <Scripts/Game/ArenaHazardComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/EnemySupplyComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// Wave 1 つぶんの構成。Docs/game-flow.md「アリーナ：Wave 1〜4」の表がそのまま並ぶ。
///
/// WHY 4 つを平らなフィールドで持つか: 構造体の配列を Inspector へ出す仕組みが無い。
///     4 という数はデザイン上の決定 (1 周 7〜9 分) なので、可変長にする必要も無い。
struct WaveSetup {
    int   mite    = 0;
    int   serpent = 0;
    int   roller  = 0;
    /// 初期配置をどれだけ散らすか [m]。小さいほど固まる = 反発が効く。
    float spread  = 6.0f;
    /// 残りがこの数まで減ったら補充する数。0 で追加供給なし。
    int   trickle = 0;
    /// この Wave から中央ハザードを作動させるか。
    bool  hazard  = false;
};

class WaveDirectorComponent : public Script {
    FBZZ_SCRIPT(WaveDirectorComponent)

public:
    FBZZ_GROUP("Flow")
    FBZZ_FIELD_RANGE(float, startDelay, 2.0f, "Start Delay", 0.0f, 20.0f)
    FBZZ_TOOLTIP("開始してから Wave 1 を出すまで。降りてくる間を取る")
    FBZZ_FIELD_RANGE(float, interWaveSeconds, 3.0f, "Inter-Wave", 0.0f, 15.0f)
    FBZZ_TOOLTIP("Wave の間。極性の残り時間が切れる余白であり、息継ぎでもある")
    FBZZ_FIELD_RANGE(float, trickleSeconds, 4.0f, "Trickle Interval", 0.5f, 20.0f)
    FBZZ_TOOLTIP("追加供給の最短間隔。連続で湧かせない")
    FBZZ_FIELD_RANGE_INT(int, maxAlive, 8, "Max Alive", 1, 32)
    FBZZ_TOOLTIP("同時出現の上限。超えると盤面の極性が読めなくなり、"
                 "«関係を作る» ゲームが乱戦に化ける")

    // ── Wave 1: 何も考えずに弾けるし、ぶつけられる ─────────────────────────
    FBZZ_GROUP("Wave 1 (dense)")
    FBZZ_FIELD_RANGE_INT(int, w1Mite, 4, "Mite", 0, 16)
    FBZZ_FIELD_RANGE_INT(int, w1Serpent, 0, "Serpent", 0, 8)
    FBZZ_FIELD_RANGE_INT(int, w1Roller, 0, "Roller", 0, 8)
    FBZZ_FIELD_RANGE(float, w1Spread, 4.0f, "Spread", 0.5f, 25.0f)
    FBZZ_TOOLTIP("反発半径 (6m) より内側に置く。なぞれば必ず弾ける配置")
    FBZZ_FIELD_RANGE_INT(int, w1Trickle, 0, "Trickle", 0, 16)

    // ── Wave 2: 引いても動かない敵の登場 ────────────────────────────────
    FBZZ_GROUP("Wave 2 (roller)")
    FBZZ_FIELD_RANGE_INT(int, w2Mite, 3, "Mite", 0, 16)
    FBZZ_FIELD_RANGE_INT(int, w2Serpent, 0, "Serpent", 0, 8)
    FBZZ_FIELD_RANGE_INT(int, w2Roller, 1, "Roller", 0, 8)
    FBZZ_FIELD_RANGE(float, w2Spread, 8.0f, "Spread", 0.5f, 25.0f)
    FBZZ_FIELD_RANGE_INT(int, w2Trickle, 2, "Trickle", 0, 16)

    // ── Wave 3: 散った敵は弾けない。集束へ切り替える ────────────────────
    FBZZ_GROUP("Wave 3 (spread + hazard)")
    FBZZ_FIELD_RANGE_INT(int, w3Mite, 4, "Mite", 0, 16)
    FBZZ_FIELD_RANGE_INT(int, w3Serpent, 1, "Serpent", 0, 8)
    FBZZ_FIELD_RANGE_INT(int, w3Roller, 0, "Roller", 0, 8)
    FBZZ_FIELD_RANGE(float, w3Spread, 14.0f, "Spread", 0.5f, 25.0f)
    FBZZ_TOOLTIP("反発半径の外へ散らす。答えが «まとめて付けてアンカーへ引く» に変わる")
    FBZZ_FIELD_RANGE_INT(int, w3Trickle, 2, "Trickle", 0, 16)

    // ── Wave 4: 的にするか転がすかを選ぶ ────────────────────────────────
    FBZZ_GROUP("Wave 4 (rollers)")
    FBZZ_FIELD_RANGE_INT(int, w4Mite, 3, "Mite", 0, 16)
    FBZZ_FIELD_RANGE_INT(int, w4Serpent, 1, "Serpent", 0, 8)
    FBZZ_FIELD_RANGE_INT(int, w4Roller, 2, "Roller", 0, 8)
    FBZZ_FIELD_RANGE(float, w4Spread, 14.0f, "Spread", 0.5f, 25.0f)
    FBZZ_FIELD_RANGE_INT(int, w4Trickle, 3, "Trickle", 0, 16)

    FBZZ_GROUP("Boss")
    FBZZ_REF(GameObject, bossObject, "Boss")
    FBZZ_TOOLTIP("最終 Wave で起こすボス。空なら «Wave 4 を片付けたら終わり» になる")
    // WHY 既定で畳まないか:
    //   Docs/game-flow.md の進行はボスを最終 Wave で出すが、それを既定にすると
    //   Play を押した瞬間にシーンへ置いたボスが消える。ボスの見た目・リグ・攻撃を
    //   確かめたいときに «まず 4 Wave 片付ける» を強いられ、しかも Wave が
    //   何かの理由で片付かないと二度と出てこない。シーンに置いてあるものを
    //   スクリプトが黙って消す方が事故として重い。畳むのは明示的に選ばせる。
    FBZZ_FIELD(bool, hideBossUntilFinal, false, "Hide Until Final")
    FBZZ_TOOLTIP("Wave 1〜4 のあいだボスを畳んでおく。切ると最初から盤面に居る "
                 "(シーンに置いたまま。ボス単体を確かめたいときはこちら)")

    FBZZ_GROUP("HUD")
    FBZZ_FIELD(std::string, waveTextName, "HUD_Wave", "Wave Text")
    FBZZ_TOOLTIP("«WAVE 2 / 4» を出すテキスト。見つからなければ何もしない")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugWave, 0, "Wave")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "Idle", "Phase")
    FBZZ_FIELD_READ_ONLY(int, debugAlive, 0, "Alive")

    [[nodiscard]] static WaveDirectorComponent* Instance() { return s_instance; }

    /// 1 (Wave 1) 〜 4。ボス戦は 5。開始前は 0。
    [[nodiscard]] int CurrentWave() const { return m_wave; }
    /// アリーナの 4 Wave を片付けてボスまで到達したか。
    [[nodiscard]] bool ReachedBoss() const { return m_wave > kWaveCount; }
    /// 進行がまだ続いているか。GameFlow は «敵 0» を勝利にする前にこれを見る。
    [[nodiscard]] bool IsRunning() const { return !ReachedBoss(); }

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    static constexpr int kWaveCount = 4;

    enum class Phase : int { Warmup = 0, Fighting, Interval, Boss };

    [[nodiscard]] WaveSetup SetupOf(int wave) const;
    /// 供給係・ハザード・ボスの «あるべき状態» を毎フレーム押し直す。
    ///
    /// WHY 切り替えの瞬間に 1 度だけ呼ばないか:
    ///   マネージャーはどれも別の GameObject に居るので、どちらの OnStart が先に走るかは
    ///   シーンの並び次第になる。OnStart で 1 度だけ Instance() を引くと、こちらが
    ///   先に走った回だけ «掴めなかった» ことになり、そのまま Play が進む。
    ///   実際それで EnemySupplyComponent が止まらず、倒しても補充が来て Wave 1 が
    ///   永久に片付かず、ボスが最後まで出てこなかった。
    ///   毎フレーム押し直せば、相手が起きた次のフレームから必ず正しい状態になる。
    void SyncManagers();
    void BeginWave(int wave);
    void BeginBoss();
    /// 初期配置を 1 体ずつ置く。散らばりは黄金角の螺旋で作る。
    void PlaceWave(const WaveSetup& setup);
    /// 追加供給。片付ける «直前» に次の思考を始めさせるためのもの。
    void TickTrickle(const WaveSetup& setup, int alive);
    [[nodiscard]] Vector3 PickPoint(float spread, int index) const;
    void RefreshHud();

    static inline WaveDirectorComponent* s_instance = nullptr;

    Phase m_phase   = Phase::Warmup;
    int   m_wave    = 0;
    float m_timer   = 0.0f;
    float m_trickle = 0.0f;
    /// この Wave でまだ補充できる数。使い切ったら «片付ける» に入る。
    int   m_trickleLeft = 0;
    /// 配置に使った螺旋の位相。Wave をまたいで送ることで、同じ場所に置き続けない。
    float m_angle = 0.0f;
    int   m_placed = 0;
};

FBZZ_REFLECT(WaveDirectorComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void WaveDirectorComponent::OnStart()
{
    s_instance = this;

    m_phase       = Phase::Warmup;
    m_wave        = 0;
    m_timer       = std::max(startDelay, 0.0f);
    m_trickle     = 0.0f;
    m_trickleLeft = 0;
    m_angle       = 0.0f;
    m_placed      = 0;
    debugWave     = 0;
    debugPhase    = "Warmup";

    // Wave の読み上げは画面の出来事なので UI バスの 2D。
    se::EnsureSource(scene, "UI");

    // 状態は SyncManagers が毎フレーム押す。ここでは 1 度だけ先に合わせて、
    // 開始の 1 フレームだけボスが見えるのを防ぐ。
    SyncManagers();
    RefreshHud();
}

inline void WaveDirectorComponent::SyncManagers()
{
    const bool bossPhase = (m_phase == Phase::Boss);

    // 補充係が開いてよいのはボス戦だけ。Wave 中に開いていると、倒しても補充が来て
    // «Wave を片付ける» が原理的に終わらない。
    if (auto* supply = EnemySupplyComponent::Instance())
        supply->SetSupplyActive(bossPhase);

    // ハザードは Wave 3 から。開幕から作動していると、中央が最初から使えない盤面になる。
    // (SetHazardActive は変化したときだけ予告を張り直すので、毎フレーム呼んで構わない)
    if (auto* hazard = ArenaHazardComponent::Instance())
        hazard->SetHazardActive(bossPhase || SetupOf(m_wave).hazard);

    if (!hideBossUntilFinal) return;
    if (GameObject* boss = bossObject.Get()) {
        // 変わったときだけ触る。毎フレーム同じ値を書くと、他の何かが畳んだ意図まで
        // こちらが押し戻すことになる。
        if (boss->activeSelf() != bossPhase) boss->SetActive(bossPhase);
    }
}

inline WaveSetup WaveDirectorComponent::SetupOf(int wave) const
{
    switch (wave) {
    case 1: return { w1Mite, w1Serpent, w1Roller, w1Spread, w1Trickle, false };
    case 2: return { w2Mite, w2Serpent, w2Roller, w2Spread, w2Trickle, false };
    case 3: return { w3Mite, w3Serpent, w3Roller, w3Spread, w3Trickle, true  };
    case 4: return { w4Mite, w4Serpent, w4Roller, w4Spread, w4Trickle, true  };
    default: break;
    }
    return {};
}

inline Vector3 WaveDirectorComponent::PickPoint(float spread, int index) const
{
    // 黄金角の螺旋。等分に置くと «次はあそこ» が読め、乱数だと同じ場所に重なる。
    constexpr float kGoldenAngle = 2.39996323f;

    Vector3 center = transform.worldPosition;
    if (auto* bounds = ArenaBoundsComponent::Instance()) center = bounds->Center();

    const float angle  = m_angle + kGoldenAngle * static_cast<float>(index);
    // 螺旋の半径。中心に集めすぎないよう、内側 40% は使わない。
    const float ratio  = 0.4f + 0.6f * std::sqrt(
        static_cast<float>(index + 1) / static_cast<float>(index + 2));
    const float radius = std::max(spread, 0.5f) * ratio;

    return { center.x + std::cos(angle) * radius,
             center.y + 0.4f,
             center.z + std::sin(angle) * radius };
}

inline void WaveDirectorComponent::PlaceWave(const WaveSetup& setup)
{
    auto* supply = EnemySupplyComponent::Instance();
    if (!supply) {
        debug.LogError("WaveDirectorComponent found no EnemySupplyComponent in the scene. "
                       "No enemy can be placed and the run never progresses.");
        return;
    }

    // WHY 種類ごとにまとめて置くか: 同じ機種が固まっていると «この一角は同じ手が効く»
    //     が読める。混ぜて撒くと、どの敵にどの手が要るかを 1 体ずつ確かめることになり、
    //     盤面を «形» として読む遊びにならない。
    const int counts[3] = { setup.mite, setup.serpent, setup.roller };
    int index = 0;
    for (int kind = 0; kind < 3; ++kind) {
        for (int i = 0; i < counts[kind]; ++i) {
            supply->SpawnAt(kind, PickPoint(setup.spread, index));
            ++index;
        }
    }
    m_placed = index;
    m_angle += 1.0f;   // 次の Wave は少しずらして置く
}

inline void WaveDirectorComponent::BeginWave(int wave)
{
    m_wave  = wave;
    m_phase = Phase::Fighting;
    debugWave  = wave;
    debugPhase = "Fighting";

    const WaveSetup setup = SetupOf(wave);
    m_trickleLeft = std::max(setup.trickle, 0);
    m_trickle     = std::max(trickleSeconds, 0.5f);

    // ハザードの作動は SyncManagers が m_wave から毎フレーム押す。ここでは触らない。
    PlaceWave(setup);
    se::Play(audio, se::kUiWaveStart);
    RefreshHud();
}

inline void WaveDirectorComponent::BeginBoss()
{
    m_wave  = kWaveCount + 1;
    m_phase = Phase::Boss;
    debugWave  = m_wave;
    debugPhase = "Boss";

    // ボスを起こすのも、補充係を開くのも、ハザードを点けるのも SyncManagers の仕事。
    // ここでフェーズを進めれば、次のフレームには全部が «ボス戦の状態» へ揃う。
    //
    // WHY ボス戦でもハザードを点けたままにするか: 中央を空けると、ボスの下が
    //     一番安全な場所になって «腹下へ潜った罰» (踏みつけ) が意味を失う。
    SyncManagers();

    se::Play(audio, se::kUiWaveStart);
    RefreshHud();
}

inline void WaveDirectorComponent::TickTrickle(const WaveSetup& setup, int alive)
{
    if (m_trickleLeft <= 0) return;
    if (alive >= std::max(maxAlive, 1)) return;

    m_trickle -= Time::deltaTime;
    if (m_trickle > 0.0f) return;

    // WHY «残りが減ってから» ではなく間隔で出すか: 目的は圧の維持ではなく、
    //     プレイヤーが片付けきる直前に次の思考を始めさせること。減り方を条件にすると、
    //     手早く捌いた人ほど間隔が詰まって «急かされている» だけになる。
    auto* supply = EnemySupplyComponent::Instance();
    if (!supply) return;

    supply->SpawnAt(0, PickPoint(setup.spread, m_placed + m_trickleLeft));
    --m_trickleLeft;
    m_trickle = std::max(trickleSeconds, 0.5f);
}

inline void WaveDirectorComponent::RefreshHud()
{
    GameObject* text = scene.Find(waveTextName);
    if (!text) return;

    if (m_phase == Phase::Boss)      ui.SetText(text, "FINAL");
    else if (m_wave <= 0)            ui.SetText(text, "");
    else ui.SetText(text, "WAVE " + std::to_string(m_wave) + " / " +
                          std::to_string(kWaveCount));
}

inline void WaveDirectorComponent::OnUpdate()
{
    // 掴めなかったマネージャーが後から起きてくる。押し直しは毎フレーム。
    SyncManagers();

    auto* combat = CombatManagerComponent::Instance();
    const int alive = combat ? combat->CountEnemiesAlive() : 0;
    debugAlive = alive;

    switch (m_phase) {
    case Phase::Warmup:
        m_timer -= Time::deltaTime;
        if (m_timer <= 0.0f) BeginWave(1);
        return;

    case Phase::Fighting: {
        const WaveSetup setup = SetupOf(m_wave);
        TickTrickle(setup, alive);
        // 補充を使い切って、盤面も空になったら片付いた。
        if (alive > 0 || m_trickleLeft > 0) return;

        m_phase = Phase::Interval;
        m_timer = std::max(interWaveSeconds, 0.0f);
        debugPhase = "Interval";
        se::Play(audio, se::kUiWaveClear);
        return;
    }

    case Phase::Interval:
        m_timer -= Time::deltaTime;
        if (m_timer > 0.0f) return;
        if (m_wave < kWaveCount) BeginWave(m_wave + 1);
        else                     BeginBoss();
        return;

    case Phase::Boss:
        return;
    }
}

} // namespace sandbox
