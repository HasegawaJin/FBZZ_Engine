/// @file    BladeSteelComponent.hpp
/// @brief   刀身そのものを攻撃状態で焼く。刃文・走る帯・縁を書く層
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持つ。
/// BladeComponent と PlayerParryComponent へ問い合わせるだけで、どちらの挙動にも触らない。
///
/// WHY BladeChargeGlow と分けるか:
///   あちらが書くのは芯 (M_SwordCore) と刻印 (M_SwordMark) の «自発光の強さ» で、
///   材質は SkinnedPBR ─ 光るか光らないかしかない。こちらが書くのは刀身
///   (M_SwordBlade / SkinnedBladeSteel) の刃文・帯・縁で、扱う量も材質も別。
///   1 つに混ぜると «芯だけ光らせたい» と «刃だけ焼きたい» が同じスロット探索を
///   奪い合い、どちらかが必ず相手の材質へ書こうとして黙って捨てられる。
///
/// WHY 4 つの状態を «熱» という 1 本の軸へ集めるか:
///   溜め・振り・連撃・弾きはそれぞれ別の出来事だが、画面に出る場所は同じ刀身 1 本。
///   別々の色と別々の見せ方を割り当てると、4 つが重なったフレームで刀が
///   «何色でもない明るい棒» になる。強さ (熱) は足し合わせても意味が壊れず、
///   上限へ着いたことは色ではなく白 (whiteHeat) で言う。
///
/// WHY 振りだけ «位置» を持つか:
///   溜めも連撃も «量» なので明るさで足りるが、振りは方向のある出来事で、
///   量にすると «振った» と «溜めた» が同じ顔になる。柄から切先へ抜ける帯は
///   刃の向きそのものを描くので、量では出せない情報を 1 つだけ持つ。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/BladeComponent.hpp>
#include <Scripts/Player/PlayerParryComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// SkinnedBladeSteel.hlsl の cbuffer 変数名。
//
// WHY 名前を 1 箇所へ置くか: MaterialInstance はシェーダーのリフレクション名を
//     検証して、合わなければ «黙って» 書き込みを捨てる。綴りが散ると、直した
//     つもりの片方だけが効かないという形で壊れる (GlowMaterial.hpp と同じ判断)。
inline constexpr MaterialPropertyId kHamonHeatId     { "hamonHeat" };
inline constexpr MaterialPropertyId kWhiteHeatId     { "whiteHeat" };
inline constexpr MaterialPropertyId kSweepPosId      { "sweepPos" };
inline constexpr MaterialPropertyId kSweepIntensityId{ "sweepIntensity" };
inline constexpr MaterialPropertyId kRimIntensityId  { "rimIntensity" };

class BladeSteelComponent : public Script {
    FBZZ_SCRIPT(BladeSteelComponent)

public:
    FBZZ_GROUP("Blade Steel — 刃文")
    FBZZ_FIELD_RANGE(float, steelHamonIdle, 0.0f, "待機", 0.0f, 4.0f)
    FBZZ_TOOLTIP("何も起きていないときの焼き。0 で «普段はただの鋼»。"
                 "少し点けると常時 «焼きの入った刀» になるが、溜めの伸びはそのぶん狭くなる")
    FBZZ_FIELD_RANGE(float, steelHamonCharged, 2.4f, "満溜め", 0.0f, 12.0f)
    FBZZ_TOOLTIP("溜め切ったときの焼き。ブルームのしきい値 (4.0) より下に置くこと ─ "
                 "ここで既に滲んでいると、下の Full Flash が «跨いだ» ことを言えない")
    FBZZ_FIELD_RANGE(float, steelHamonFlash, 5.2f, "Full Flash", 0.0f, 20.0f)
    FBZZ_TOOLTIP("満溜めに «届いた瞬間» だけの焼き。しきい値を越えるので刃が滲む")
    FBZZ_FIELD_RANGE(float, steelHamonFlashSeconds, 0.14f, "Full Flash Time", 0.0f, 0.6f)
    // WHY 溜め比をそのまま使わないか: 震え・パッド・歪み・芯の発光はすべて比の 2 乗で
    //     立ち上がる (DriveCharge / BladeChargeGlow)。刃だけ線形にすると、前半は
    //     «焼けているのに何も起きない»、終盤は «手だけ来て刃が追いつかない» になる。
    FBZZ_FIELD_RANGE(float, steelHamonCurve, 2.0f, "Curve", 0.5f, 4.0f)

    FBZZ_GROUP("Blade Steel — 振り")
    FBZZ_FIELD_RANGE(float, steelSweepSeconds, 0.16f, "帯の時間 [秒]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("柄から切先へ抜けるまでの時間。0 で帯を出さない。"
                 "斬撃の発生 (Startup) より短くすること ─ 長いと «当たった後で刃が光る»")
    FBZZ_FIELD_RANGE(float, steelSweepIntensity, 3.2f, "帯の明るさ", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, steelSweepHeat, 1.4f, "帯が置く熱", 0.0f, 12.0f)
    FBZZ_TOOLTIP("帯が抜けたあと刃文へ残る熱。振るたびに刃が温まる分で、"
                 "連撃を続けているあいだ刀が冷めない理由になる")

    FBZZ_GROUP("Blade Steel — 連撃")
    FBZZ_FIELD_RANGE(float, steelComboHeat, 0.35f, "1 段あたりの熱", 0.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, steelComboTemperature, 0.20f, "1 段あたりの白", 0.0f, 1.0f)
    FBZZ_TOOLTIP("段が進むほど焼き色を白へ寄せる量。«もっと橙» では上限が読めないので、"
                 "上へ行くほど色を捨てて明るさだけにする")
    FBZZ_FIELD_RANGE(float, steelFinisherTemperature, 0.85f, "締めの白", 0.0f, 1.0f)

    FBZZ_GROUP("Blade Steel — 弾き / とどめ")
    FBZZ_FIELD_RANGE(float, steelGuardHeat, 0.9f, "構えている間", 0.0f, 8.0f)
    FBZZ_TOOLTIP("弾きの窓が開いているあいだの焼き。«今なら弾ける» を刀で言う層で、"
                 "窓の長さがそのまま見える")
    FBZZ_FIELD_RANGE(float, steelParryFlash, 4.0f, "弾いた", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, steelJustParryFlash, 7.6f, "読み切って弾いた", 0.0f, 24.0f)
    FBZZ_TOOLTIP("Just Parry の閃光。普通の弾きより明確に上へ置くこと ─ "
                 "同じ明るさだと «読み切った» が手触り (feel 1.6 倍) にしか無くなる")
    FBZZ_FIELD_RANGE(float, steelParryFlashSeconds, 0.22f, "弾きの残り [秒]", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, steelExecuteFlash, 8.4f, "とどめ", 0.0f, 24.0f)
    FBZZ_FIELD_RANGE(float, steelExecuteFlashSeconds, 0.45f, "とどめの残り [秒]", 0.0f, 2.0f)
    // WHY 拍に乗った一振りでも刃を光らせるか: 拍の合図が音だけだと、音を切って遊ぶ人には
    //     «今のは乗った» が届かない。目が向いている刃そのものが一瞬白むと、見るだけで分かる。
    FBZZ_FIELD_RANGE(float, steelBeatFlash, 3.2f, "拍に乗った", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, steelBeatFlashSeconds, 0.12f, "拍の残り [秒]", 0.0f, 1.0f)

    FBZZ_GROUP("Blade Steel — 縁")
    FBZZ_FIELD_RANGE(float, steelRimReady, 1.4f, "臨戦の縁", 0.0f, 8.0f)
    FBZZ_TOOLTIP("満溜め / Flux を持っているあいだ刀身の輪郭が灯る強さ。"
                 "刃文が «どこが刃か» を言うのに対し、こちらは «次の一振りが重い» を言う")

    FBZZ_GROUP("Blade Steel — 書き込むスロット")
    FBZZ_FIELD(std::string, steelMatch, "SwordBlade", "Match")
    FBZZ_TOOLTIP("この文字列を .mat のパスに含むスロットへ書く")

    FBZZ_FIELD_READ_ONLY(int, debugSteelSlots, 0, "Blade Slots")
    FBZZ_FIELD_READ_ONLY(float, debugSteelHeat, 0.0f, "Heat")

    /// 溜め・振り・段はここから読む。注入されないと何もしない。
    void SetBlades(BladeComponent* blades) { m_blades = blades; }
    /// 弾き・とどめ。無くても刃文と帯は動く (弾きの層だけが黙る)。
    void SetParry(PlayerParryComponent* parry) { m_parry = parry; }

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// 書き込む 1 スロット。元の値を控えて、熱が引いたら必ず戻す。
    struct Slot {
        EntityRef target;
        uint32_t  slot = 0;
        float     baseHeat = 0.0f;
        float     baseTemperature = 0.0f;
        float     baseSweep = -1.0f;
        float     baseSweepIntensity = 0.0f;
        float     baseRim = 0.0f;
    };

    /// 刀 1 振りぶんの状態。
    struct Blade {
        std::vector<Slot> slots;
        /// 帯の進み [秒]。0 未満で «走っていない»。
        float sweep = -1.0f;
        /// 帯が置いていった熱の残り [0,1]。
        float afterHeat = 0.0f;
        /// 今フレーム書き込んだか。戻し忘れを 1 枚の札で防ぐ。
        bool  writing = false;
    };

    [[nodiscard]] static std::size_t HandIndex(HandSide hand)
    { return hand == HandSide::Right ? 0u : 1u; }

    /// その手の刀の «刀身» スロットを集め直す。抜刀で後から現れるので、
    /// 空のあいだは毎フレーム探しに行く (見つかった時点で止まる)。
    void Collect(HandSide hand);
    /// 1 振りへ書く。heat / temperature / rim が全部 0 で帯も走っていなければ、
    /// 控えた元の値へ戻して忘れる。
    void Drive(HandSide hand, float heat, float temperature, float rim);
    [[nodiscard]] bool Matches(const std::string& path) const;

    BladeComponent*       m_blades = nullptr;
    PlayerParryComponent* m_parry  = nullptr;
    Blade m_hands[2];

    /// 前フレームの溜め比と «振っていたか»。立ち上がりの 1 フレームを取るため。
    float m_lastRatio   = 0.0f;
    bool  m_wasSwinging = false;
    /// 前フレームまでに見た振り出しの番号。変わったフレームが «振った瞬間»。
    int   m_lastSwingSerial = 0;
    /// 満溜めに届いた合図の残り [秒]。溜めているのは常に片手 (または Flux で両手)。
    float m_fullFlash   = 0.0f;
    /// 弾き / とどめの閃光の残り [秒] と、その強さ。
    float m_eventFlash        = 0.0f;
    float m_eventFlashSeconds = 0.0f;
    float m_eventLevel        = 0.0f;
    /// 前フレームまでの回数。差が «今フレームに起きた» を意味する。
    int m_lastParries    = 0;
    int m_lastJust       = 0;
    int m_lastExecutions = 0;
};

FBZZ_REFLECT(BladeSteelComponent)


inline void BladeSteelComponent::OnStart()
{
    for (Blade& blade : m_hands) {
        blade.slots.clear();
        blade.sweep     = -1.0f;
        blade.afterHeat = 0.0f;
        blade.writing   = false;
    }
    m_lastRatio   = 0.0f;
    m_wasSwinging = false;
    m_lastSwingSerial = m_blades ? m_blades->SwingSerial() : 0;
    m_fullFlash   = 0.0f;
    m_eventFlash  = 0.0f;
    m_eventLevel  = 0.0f;
    // WHY 0 から始めず今の回数を控えるか: 弾きの回数はステージを跨いでも
    //     リセットされない。0 から始めると、シーンを読み直した最初のフレームに
    //     «これまでの全部» が一度に起きたことになって刀が白く飛ぶ。
    m_lastParries    = m_parry ? m_parry->ParryCount()     : 0;
    m_lastJust       = m_parry ? m_parry->JustParryCount() : 0;
    m_lastExecutions = m_parry ? m_parry->ExecuteCount()   : 0;
    debugSteelSlots = 0;
    debugSteelHeat  = 0.0f;
}

inline bool BladeSteelComponent::Matches(const std::string& path) const
{
    return !steelMatch.empty() && !path.empty()
        && path.find(steelMatch) != std::string::npos;
}

inline void BladeSteelComponent::Collect(HandSide hand)
{
    Blade& blade = m_hands[HandIndex(hand)];
    blade.slots.clear();

    GameObject* sword = scene.Find(SwordObjectName(hand));
    if (!sword) return;

    auto* materials = sword->GetComponent<MaterialComponent>();
    if (!materials) return;

    // スロット 0 は MaterialComponent 自身、1 以降が extraSlots。番号ではなく
    // .mat のパスで選ぶ ─ FBX を差し替えて submesh の順が変わっても、番号は
    // 無効にならないので «鋼でない所へ書いている» に気付けない。
    const std::size_t count = materials->SlotCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (!Matches(materials->RawSlotAt(i).materialPath)) continue;

        Slot entry{ EntityRef{ sword->GetID() }, static_cast<uint32_t>(i) };
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (!instance.IsValid()) continue;
        // 刀身以外の材質が同じ名前で引っ掛かっても、SkinnedBladeSteel の変数を
        // 持たない限りここで落ちる (書いても捨てられるスロットを抱えない)。
        if (!instance.HasProperty(kHamonHeatId)) continue;
        (void)instance.TryGetFloat(kHamonHeatId,      entry.baseHeat);
        (void)instance.TryGetFloat(kWhiteHeatId,    entry.baseTemperature);
        (void)instance.TryGetFloat(kSweepPosId,       entry.baseSweep);
        (void)instance.TryGetFloat(kSweepIntensityId, entry.baseSweepIntensity);
        (void)instance.TryGetFloat(kRimIntensityId,   entry.baseRim);
        blade.slots.push_back(entry);
    }
}

inline void BladeSteelComponent::Drive(HandSide hand, float heat, float temperature, float rim)
{
    Blade& blade = m_hands[HandIndex(hand)];

    const bool running = blade.sweep >= 0.0f && steelSweepSeconds > 0.0f;
    const bool want = heat > 0.0f || temperature > 0.0f || rim > 0.0f || running;
    if (!want && !blade.writing) return;   // 書いていないなら戻すものも無い

    // 帯は柄の少し手前から切先の少し先まで抜ける。端で丁度 0/1 に止めると、
    // 出る瞬間と消える瞬間に «刃の端で光が湧く» が見えてしまう。
    float sweepPos = -1.0f;
    if (running) {
        const float t = Clamp01(blade.sweep / Max(steelSweepSeconds, 1.0e-3f));
        sweepPos = Lerp(-0.15f, 1.15f, t);
    }

    for (const Slot& entry : blade.slots) {
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (!instance.IsValid()) continue;
        if (want) {
            instance.SetFloat(kHamonHeatId,      heat);
            instance.SetFloat(kWhiteHeatId,    Clamp01(temperature));
            instance.SetFloat(kSweepPosId,       sweepPos);
            instance.SetFloat(kSweepIntensityId, running ? Max(steelSweepIntensity, 0.0f) : 0.0f);
            instance.SetFloat(kRimIntensityId,   rim);
        } else {
            instance.SetFloat(kHamonHeatId,      entry.baseHeat);
            instance.SetFloat(kWhiteHeatId,    entry.baseTemperature);
            instance.SetFloat(kSweepPosId,       entry.baseSweep);
            instance.SetFloat(kSweepIntensityId, entry.baseSweepIntensity);
            instance.SetFloat(kRimIntensityId,   entry.baseRim);
        }
    }
    blade.writing = want;
}

inline void BladeSteelComponent::OnUpdate()
{
    if (!enabled || !m_blades) return;

    const float dt = Max(Time::deltaTime, 0.0f);

    // 抜刀で後から現れる。空のあいだだけ探しに行き、見つかったら止まる。
    const HandSide hands[] = { HandSide::Right };
    int found = 0;
    for (const HandSide hand : hands) {
        if (m_hands[HandIndex(hand)].slots.empty()) Collect(hand);
        found += static_cast<int>(m_hands[HandIndex(hand)].slots.size());
    }
    debugSteelSlots = found;

    // ── 振った瞬間 ──────────────────────────────────────────────────────────
    // 帯は一撃につき 1 本。振っている «あいだ» 流し続けると、硬直のあいだも
    // 走り続けて «次が振れる» と読み違える。
    //
    // WHY «振っているか» の立ち上がりではなく振り出しの番号で取るか (2026-09-12):
    //     連撃では硬直が明けたフレームのうちに次の段が振り出されるので、IsSwinging は
    //     一度も false にならない。立ち上がりで取っていた頃は、帯が 1 段目にしか走らず
    //     2 段目以降の刃は冷たいままだった。
    const bool swinging = m_blades->IsSwinging();
    const int  serial   = m_blades->SwingSerial();
    if (serial != m_lastSwingSerial) {
        m_lastSwingSerial = serial;
        // 拍に乗った振り出しは刃が一瞬白む。弾き・とどめの閃光が走っている間はそちらに譲る。
        if (m_blades->IsOnBeatSwing() && m_eventFlash <= 0.0f
            && steelBeatFlash > 0.0f && steelBeatFlashSeconds > 0.0f) {
            m_eventLevel        = steelBeatFlash;
            m_eventFlashSeconds = steelBeatFlashSeconds;
            m_eventFlash        = steelBeatFlashSeconds;
        }
        const BladeSide swung = m_blades->SwingSide();
        // 溜め斬りは両刀 (Katana_Slash_Dual)。片方だけ走らせると、回っている
        // 2 本のうち 1 本だけ刃が冷たいままになる。
        if (m_blades->IsCharged()) {
            for (Blade& blade : m_hands) blade.sweep = 0.0f;
        } else if (swung != BladeSide::None) {
            m_hands[HandIndex(HandOf(swung))].sweep = 0.0f;
        }
    }
    m_wasSwinging = swinging;

    // ── 溜め ────────────────────────────────────────────────────────────────
    const float     ratio   = Clamp01(m_blades->ChargeRatio());
    const BladeSide holding = m_blades->ChargingSide();
    const bool      reached = ratio >= 1.0f && m_lastRatio < 1.0f;
    m_lastRatio = ratio;

    if (reached) m_fullFlash = Max(steelHamonFlashSeconds, 0.0f);
    m_fullFlash = Max(m_fullFlash - dt, 0.0f);

    const float charged = std::pow(ratio, Max(steelHamonCurve, 0.01f))
                        * Max(steelHamonCharged, 0.0f);
    const float fullFlash = steelHamonFlashSeconds > 0.0f
        ? Max(steelHamonFlash, 0.0f) * (m_fullFlash / Max(steelHamonFlashSeconds, 1.0e-3f))
        : 0.0f;

    // ── 弾き / とどめ ───────────────────────────────────────────────────────
    // WHY 回数の差で取るか: どちらも 1 フレームの出来事で、通知を受け取る形にすると
    //     更新順によって取り逃がす。差なら «今フレームに起きたか» が順に依らない。
    if (m_parry) {
        const int parries    = m_parry->ParryCount();
        const int justs      = m_parry->JustParryCount();
        const int executions = m_parry->ExecuteCount();
        // とどめ > 読み切り > 弾き。同じフレームに 2 つ起きたら重い方だけを出す ─
        // 足すと «とどめ + 弾き» のフレームだけ 2 段跳ねて、そこだけ白飛びする。
        float level   = 0.0f;
        float seconds = 0.0f;
        if (executions > m_lastExecutions) {
            level   = Max(steelExecuteFlash, 0.0f);
            seconds = Max(steelExecuteFlashSeconds, 0.0f);
        } else if (justs > m_lastJust) {
            level   = Max(steelJustParryFlash, 0.0f);
            seconds = Max(steelParryFlashSeconds, 0.0f);
        } else if (parries > m_lastParries) {
            level   = Max(steelParryFlash, 0.0f);
            seconds = Max(steelParryFlashSeconds, 0.0f);
        }
        if (level > 0.0f && seconds > 0.0f) {
            m_eventLevel        = level;
            m_eventFlashSeconds = seconds;
            m_eventFlash        = seconds;
        }
        m_lastParries    = parries;
        m_lastJust       = justs;
        m_lastExecutions = executions;
    }
    m_eventFlash = Max(m_eventFlash - dt, 0.0f);
    const float eventFlash = m_eventFlashSeconds > 0.0f
        ? m_eventLevel * (m_eventFlash / Max(m_eventFlashSeconds, 1.0e-3f))
        : 0.0f;
    const bool guarding = m_parry && m_parry->IsParryActive();

    // ── 連撃 ────────────────────────────────────────────────────────────────
    // 段は振っているあいだだけ読む。硬直で段が残っていても刀は冷めていくので、
    // «続いている» の絵は帯が置いた熱 (afterHeat) が持つ。
    const int   step      = swinging ? Max(m_blades->ComboStep(), 0) : 0;
    const bool  finisher  = swinging && m_blades->IsFinisherSwing();
    const float comboHeat = static_cast<float>(step) * Max(steelComboHeat, 0.0f);
    const float comboTemp = finisher
        ? Max(steelFinisherTemperature, 0.0f)
        : static_cast<float>(step) * Max(steelComboTemperature, 0.0f);

    float highest = 0.0f;
    for (const HandSide hand : hands) {
        Blade& blade = m_hands[HandIndex(hand)];

        if (blade.sweep >= 0.0f) {
            blade.sweep += dt;
            if (steelSweepSeconds <= 0.0f || blade.sweep > steelSweepSeconds) {
                blade.sweep = -1.0f;
                // 抜け切った帯は刃へ熱を置いていく。ここで初めて «温まった» が残る。
                blade.afterHeat = 1.0f;
            }
        }
        // 置かれた熱は帯と同じ時間で引く。長く残すと、連撃の間に冷めきらず
        // «振っていないのに焼けている» が定常になって、溜めとの差が消える。
        blade.afterHeat = steelSweepSeconds > 0.0f
            ? Max(blade.afterHeat - dt / Max(steelSweepSeconds * 3.0f, 1.0e-3f), 0.0f)
            : 0.0f;

        // 溜めているのは片手だけ。Flux (押していない満溜め) はまだどちらを振るか
        // 決まっていないので両手 ─ BladeChargeGlow の芯の色と同じ扱いにする。
        const bool charging = holding != BladeSide::None
                            ? (HandOf(holding) == hand) : (ratio > 0.0f);

        // 熱は «最も強いもの» を採る。足すと溜め切って振った瞬間だけ 2 段跳ねる。
        float heat = Max(steelHamonIdle, 0.0f);
        if (charging) heat = Max(heat, Max(charged, fullFlash));
        heat = Max(heat, blade.afterHeat * Max(steelSweepHeat, 0.0f));
        heat = Max(heat, comboHeat);
        if (guarding) heat = Max(heat, Max(steelGuardHeat, 0.0f));
        heat = Max(heat, eventFlash);

        // 白へ寄せるのは «上限に着いた» ときだけ。溜め切り・締め・弾きの閃光が
        // それに当たる。橙のまま明るくしても «もっと熱い» にしかならない。
        float temperature = comboTemp;
        if (charging && fullFlash > 0.0f)
            temperature = Max(temperature, Clamp01(fullFlash / Max(steelHamonFlash, 1.0e-3f)));
        if (eventFlash > 0.0f)
            temperature = Max(temperature, Clamp01(eventFlash / Max(m_eventLevel, 1.0e-3f)));

        // 縁は «次の一振りが重い» の札。Flux は押していなくても立つ。
        const float rim = (charging && (ratio >= 1.0f || m_blades->HasFlux()))
                        ? Max(steelRimReady, 0.0f) : 0.0f;

        Drive(hand, heat, temperature, rim);
        highest = Max(highest, heat);
    }
    debugSteelHeat = highest;
}

} // namespace sandbox
