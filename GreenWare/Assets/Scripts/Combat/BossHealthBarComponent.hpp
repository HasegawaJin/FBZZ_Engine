/// @file    BossHealthBarComponent.hpp
/// @brief   画面上部のボス体力バー。誰と戦っていて、あと何割かを 1 本で出す
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY プレイヤーのバーと同じ作りにするか:
///   PlayerHealthBarComponent と同じく «シーンに置いた実体をスクリプトが更新する» 形。
///   位置も太さも絵合わせで何度も触るので、ランタイムで組むと Canvas Editor で
///   掴めなくなる (あちらの冒頭の WHY)。敵の頭上バーだけがランタイム生成なのは、
///   対象ごとに 1 枚要るから。ボスは «画面に固定の 1 枚» なので置く側。
///
/// WHY 極を «バーの色» ではなく名前の側に出すか:
///   ボスは塗られる的になったので (BossPolarityCoreComponent の Player Charged)、
///   «今どちらの極が乗っているか» は撃ち込めるかどうかを決める情報になった。
///   ただしバー本体は残り HP を表しており、そこへ極性色を混ぜると «赤いのは
///   ＋だからか、瀕死だからか» が読めなくなる。量はバー、極は文字、と分ける。
///
/// WHY ボットが居ないときに «隠す» か:
///   Main 以外のシーンや、ボスを置いていない検証用の配置でも同じ HUD を使う。
///   0 のまま出しっぱなしにすると «倒し切ったのにバーが残っている» に見える。
///
/// WHY 減り方を敵の頭上バーと揃えるか:
///   EnemyHealthBarComponent の Drain (遅れて追いつく帯) と同じ理屈が、ボスでは
///   もっと強く効く。ボスへ通る 1 発は «帯電した雑魚を叩き込んだ» という数十秒の
///   組み立ての成果で、盤面で一番貴重な出来事なのに、長さが飛ぶだけだと «入った»
///   ことしか分からない。削れた量が数フレーム残れば «どれだけ入ったか» まで返る。
///   数値も挙動もあちらと同じ名前で持つ (片方だけ違う減り方をすると、同じ画面に
///   出ている 2 種類のバーが別の規則で動いているように見える)。
#pragma once

#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossHealthBarComponent : public Script {
    FBZZ_SCRIPT(BossHealthBarComponent)

public:
    FBZZ_GROUP("Boss")
    // WHY 型で探すだけにしないか:
    //   FindObjectsOfType<IBoss>() は «盤面に居るボス» を型から引く汎用の口で、
    //   ボスが 1 体しか居ないこの盤面ではそれで足りる。ただしこの参照が外れると
    //   症状は «バーが出ない» だけになり、UI 側を疑って探し回ることになる
    //   (WaveDirectorComponent が同じ理由でボスを Ref で持っている)。
    //   明示の割り当てを先に見て、空のときだけ型で探す。
    FBZZ_REF(GameObject, bossObject, "Boss")
    FBZZ_TOOLTIP("体力を出す相手。未設定なら IBoss を実装したオブジェクトを盤面から探す")

    FBZZ_GROUP("HUD")
    // 未設定なら名前で拾う。シーンを作り直しても既定の構成なら動く。
    FBZZ_REF(GameObject, bossFill, "Boss Fill")
    FBZZ_TOOLTIP("残量で塗り潰す UIImage。Fill Origin は Left にしておく")
    FBZZ_REF(GameObject, bossDrain, "Boss Drain")
    FBZZ_TOOLTIP("遅れて追いつく帯。Fill と同じ矩形・同じ Fill Origin で、"
                 "Sort Order だけ Fill より下にする。未設定でもバーは動く")
    FBZZ_REF(GameObject, bossBackground, "Boss Background")
    FBZZ_REF(GameObject, bossName, "Boss Name")
    FBZZ_TOOLTIP("ボス名と極性記号を出す UIText。未設定でも動作する")

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "Background")
    FBZZ_FIELD_COLOR(fullColor,  (Vector4{ 0.95f, 0.62f, 0.18f, 1.00f }), "Full")
    FBZZ_TOOLTIP("満タン側の色。プレイヤーのバー (青緑) と混ざらない色にすること")
    FBZZ_FIELD_COLOR(emptyColor, (Vector4{ 0.95f, 0.20f, 0.15f, 1.00f }), "Empty")
    FBZZ_FIELD_COLOR(neutralNameColor, (Vector4{ 0.62f, 0.66f, 0.72f, 1.00f }), "Name (No Polarity)")
    FBZZ_TOOLTIP("無極のときのボス名の色。極が乗ると 12.2 の極性色へ切り替わる")

    FBZZ_GROUP("Drain")
    FBZZ_FIELD_COLOR(drainColor, (Vector4{ 1.00f, 0.95f, 0.75f, 1.00f }), "Drain Color")
    FBZZ_TOOLTIP("削れた区間に残す色。バー本体より明るくすること "
                 "(暗いと «減った» ではなく «背景が見えた» と読める)")
    FBZZ_FIELD_RANGE(float, drainHold, 0.26f, "Drain Hold", 0.0f, 2.0f)
    FBZZ_TOOLTIP("被弾してから追従を始めるまでの秒数。0 で即座に追いつく。"
                 "雑魚 (0.18) より長いのは、1 発の重みが違うぶん «見せる時間» も要るため")
    FBZZ_FIELD_RANGE(float, drainSpeed, 0.55f, "Drain Speed", 0.05f, 10.0f)
    FBZZ_TOOLTIP("追従の速さ (残量割合 / 秒)。0.55 なら満タンから空まで約 1.8 秒。"
                 "バーが長いほど同じ割合でも «動いて見える» 距離が伸びるので、雑魚より遅くする")

    FBZZ_GROUP("Charged Pulse")
    // WHY 帯電中だけ明滅させるか: 極が乗っている数秒がプレイヤーの撃ち込み窓で、
    //     そこを逃すと仕込んだ列が無駄になる。バーの残量とは別の情報なので、
    //     色ではなく «動き» で出す (12.4 の «状態の変化は必ず画面で返す»)。
    FBZZ_FIELD_RANGE(float, chargedPulseDepth, 0.22f, "Pulse Depth", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, chargedPulseHz, 3.2f, "Pulse Hz", 0.0f, 12.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRatio, 0.0f, "Ratio")
    FBZZ_FIELD_READ_ONLY(float, debugDrain, 0.0f, "Drain")
    FBZZ_FIELD_READ_ONLY(bool, debugVisible, false, "Visible")
    FBZZ_FIELD_READ_ONLY(std::string, debugBoss, "", "Boss Object")
    FBZZ_TOOLTIP("実際に読んでいる相手。空のままならボスを見つけられていない")

    void OnStart() override;
    void OnLateUpdate() override;

private:
    // 追従帯が «追いつき切った» とみなす残差。1 ピクセルに満たない差のために
    // 撃破後のバーを画面へ残し続けないための床。
    static constexpr float kDrainSettled = 0.002f;

    static constexpr const char* kFillName       = "HUD_BossFill";
    static constexpr const char* kDrainName      = "HUD_BossDrain";
    static constexpr const char* kBackgroundName = "HUD_BossBackground";
    static constexpr const char* kNameName       = "HUD_BossName";

    [[nodiscard]] GameObject* Resolve(const Ref<GameObject>& reference, const char* name) const;
    /// 盤面のボス。倒れていても «居る» ので、生死は呼ぶ側が見る。
    [[nodiscard]] GameObject* FindBoss() const;
    /// バーを隠す。
    void Hide();
    /// 遅れて追いつく帯の残量を進める。戻り値が帯の塗り潰し量。
    /// 式は EnemyHealthBarComponent::AdvanceDrain と同じもの。
    float AdvanceDrain(float ratio);

    EntityRef m_fill;
    EntityRef m_drain;
    EntityRef m_background;
    EntityRef m_name;

    // 追従帯の現在値と、被弾を検出するための前フレームの残量。
    float m_drainRatio    = 1.0f;
    float m_lastRatio     = 1.0f;
    float m_holdRemaining = 0.0f;
    /// 「ボスが見つからない」を 1 度だけ言うためのラッチ。
    /// WHY 要るか: 見つからないと Hide() が毎フレーム走るだけで、画面には «何も出ない»
    ///     としか現れない。UI を疑って探すことになるので、探す先をここで名指しする。
    bool m_warnedNoBoss = false;
};

FBZZ_REFLECT(BossHealthBarComponent)


inline GameObject* BossHealthBarComponent::Resolve(const Ref<GameObject>& reference,
                                                   const char* name) const
{
    if (GameObject* object = reference.Get()) return object;
    return scene.Find(name);
}

inline void BossHealthBarComponent::OnStart()
{
    m_warnedNoBoss = false;
    debugBoss.clear();

    // 参照の解決は Play 開始時に 1 度だけ。毎フレーム名前で探すと、見つからない構成で
    // 静かに全シーン走査を続けることになる。
    GameObject* fill = Resolve(bossFill, kFillName);
    if (!fill) {
        debug.LogError("BossHealthBarComponent: boss fill UIImage not found "
                       "(assign Boss Fill, or name it HUD_BossFill in the scene).");
        return;
    }
    m_fill = EntityRef{ fill->GetID() };

    // 帯は欠けていても «量» は読める。無い構成でもバーは動かす (雑魚と違い、
    // こちらは 3 枚をシーンに置く形なので、1 枚足し忘れただけで HUD が死ぬのは重い)。
    if (GameObject* drain = Resolve(bossDrain, kDrainName))
        m_drain = EntityRef{ drain->GetID() };
    if (GameObject* background = Resolve(bossBackground, kBackgroundName))
        m_background = EntityRef{ background->GetID() };
    if (GameObject* label = Resolve(bossName, kNameName))
        m_name = EntityRef{ label->GetID() };

    // 追従帯は «今の残量» から始める。満タンから始めると、DLL リロードや
    // 途中参加の構成で «開始と同時に大ダメージが入った» という嘘が 1 回流れる。
    GameObject* boss = FindBoss();
    const auto* health = boss ? scene.GetScript<EnemyHealthComponent>(boss) : nullptr;
    m_drainRatio    = health ? health->Normalized() : 1.0f;
    m_lastRatio     = m_drainRatio;
    m_holdRemaining = 0.0f;

    Hide();
}

inline float BossHealthBarComponent::AdvanceDrain(float ratio)
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (ratio >= m_drainRatio) {
        // 回復と初期化。遅らせる理由がないので即座に合わせる。
        m_drainRatio    = ratio;
        m_holdRemaining = 0.0f;
    } else {
        // 新しく減った瞬間だけ保持時間を入れ直す。連続で入れても毎回「溜め」が入る。
        if (ratio < m_lastRatio) m_holdRemaining = std::max(drainHold, 0.0f);
        if (m_holdRemaining > 0.0f)
            m_holdRemaining = std::max(0.0f, m_holdRemaining - dt);
        else
            m_drainRatio = std::max(ratio, m_drainRatio - std::max(drainSpeed, 0.05f) * dt);
    }

    m_lastRatio = ratio;
    return m_drainRatio;
}

inline GameObject* BossHealthBarComponent::FindBoss() const
{
    // 出てくる前のボスは «居ない»。畳まれている間もバーを出すと、Wave を戦っている
    // あいだじゅう «まだ見ぬ相手の満タンの体力» が画面上部に居座る。
    if (GameObject* assigned = bossObject.Get())
        return assigned->activeInHierarchy() ? assigned : nullptr;

    return FindBossOnBoard(scene);
}

inline void BossHealthBarComponent::Hide()
{
    debugVisible = false;
    debugRatio   = 0.0f;

    // WHY 表示スイッチで消すか (アルファ 0 ではなく):
    //   アルファで消すと «消えている» が色の値でしか表せない。誰かが色を書き戻した
    //   瞬間に出てしまうし、逆に何かの拍子に enabled が落ちていると、こちらが色を
    //   戻しても二度と出てこない (出ない側に倒れる不具合は原因が画面に出ない)。
    //   表示は表示のスイッチで持つ。幅を 0 にしないのは、次に出るときへ向けて
    //   «枠» の寸法を保つため。
    if (GameObject* fill = m_fill.Resolve(scene))       ui.SetImageEnabled(fill, false);
    if (GameObject* drain = m_drain.Resolve(scene))     ui.SetImageEnabled(drain, false);
    if (GameObject* back = m_background.Resolve(scene)) ui.SetImageEnabled(back, false);
    if (GameObject* label = m_name.Resolve(scene))      ui.SetTextEnabled(label, false);
}

inline void BossHealthBarComponent::OnLateUpdate()
{
    GameObject* fill = m_fill.Resolve(scene);
    if (!fill) return;

    GameObject* boss = FindBoss();
    const auto* health = boss ? scene.GetScript<EnemyHealthComponent>(boss) : nullptr;
    debugBoss = boss ? boss->name : std::string{};

    // 出ていないのと、居ないのと、倒し切ったのは別の話。
    //   出ていない … 最終 Wave まで畳まれている正しい状態。黙って隠す
    //   居ない     … 組み方の間違い。畳まれたものすら 1 体も無いときだけ名指しで言う
    //   倒し切った … 下で 0 まで減らしてから引く
    if (!boss || !health) {
        if (!m_warnedNoBoss) {
            // 割り当てがあるなら «まだ出ていないだけ»。全走査はそこで打ち切る。
            const bool authored = bossObject.IsAssigned()
                               || !scene.FindObjectsOfType<IBoss>().empty();
            if (!authored || boss) {
                m_warnedNoBoss = true;
                debug.LogError(boss
                    ? "BossHealthBarComponent found the boss but no EnemyHealthComponent on it "
                      "(the bar has no health to read)."
                    : "BossHealthBarComponent found no boss in the scene "
                      "(assign Boss, or give the boss object a script implementing IBoss).");
            }
        }
        Hide();
        return;
    }
    // WHY 倒れてすぐ引かないか:
    //   最後の 1 発だけがバーに出ないと、«倒した» のが自分の一撃だったのか、
    //   時間切れや別の何かだったのかが画面から分からなくなる。ボスの HP が減るのは
    //   帯電した雑魚を叩き込んだときだけなので、この最後の 1 発こそ一番返したい。
    //   0 まで «減り切る» のを見せ、追従帯が追いついてから引く (畳むまでの猶予は
    //   BossDeathVfxComponent の崩壊が持っているので、この間バーだけが浮くことはない)。
    const bool  alive = health->IsAlive();
    const float ratio = alive ? health->Normalized() : 0.0f;
    const float drain = AdvanceDrain(ratio);
    debugRatio = ratio;
    debugDrain = drain;

    if (!alive && drain <= kDrainSettled) {
        Hide();
        return;
    }
    debugVisible = true;

    const Vector4 base = emptyColor + (fullColor - emptyColor) * ratio;

    // 帯電中だけ明滅させる。撃ち込める窓が開いていることを、残量とは別の軸で返す。
    //
    // WHY 倒れた後は無極として扱うか: 明滅と極性記号は «まだ撃ち込める» の合図。
    //     0 まで減り切るのを見せている数百ミリ秒のあいだも出し続けると、決着が
    //     «まだ続いている» に見える。倒れた時点でこの軸は黙らせる。
    const auto* bossScript = scene.GetScript<IBoss>(boss);
    const Polarity polarity = (alive && bossScript) ? bossScript->CurrentPolarity()
                                                    : Polarity::None;

    float pulse = 1.0f;
    if (polarity != Polarity::None && chargedPulseDepth > 0.0f) {
        // 実時間で回す。ヒットストップ中に止まると、撃ち込んだ瞬間の一番見せたい
        // フレームだけが静止画になる。
        const float phase = Time::unscaledTime * chargedPulseHz * TWO_PI;
        pulse = 1.0f + std::sin(phase) * chargedPulseDepth;
    }

    // アルファは動かさない。透けると背景の明暗でバーの読みが変わる。
    const Vector4 color{ base.x * pulse, base.y * pulse, base.z * pulse, base.w };

    if (GameObject* back = m_background.Resolve(scene)) {
        ui.SetImageEnabled(back, true);
        ui.SetImageColor(back, backgroundColor);
    }

    // 帯は «さっきまであった量» なので、本体より下 (Sort Order を小さく) に敷いて
    // 減った区間だけをはみ出させる。重なり順を決めるのはシーンの Sort Order で、
    // ここの呼び出し順ではない (背景 0 / 帯 1 / 本体 2)。
    // WHY 帯だけ明滅させないか: 明滅は «撃ち込める窓が開いている» を表す語で、
    //     帯が表しているのは «直前に入った量»。同じ動きを 2 つの意味に使うと、
    //     どちらの合図なのかが読めなくなる。
    if (GameObject* drainObject = m_drain.Resolve(scene)) {
        ui.SetImageEnabled(drainObject, true);
        ui.SetImageFillAmount(drainObject, drain);
        ui.SetImageColor(drainObject, drainColor);
    }

    // 表示は毎フレーム押し直す。Hide() が落としたスイッチを戻すのはここだけで、
    // «バーが一度も出ない» の芽をこの 1 行に閉じ込めておく。
    ui.SetImageEnabled(fill, true);
    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, color);

    if (GameObject* label = m_name.Resolve(scene)) {
        ui.SetTextEnabled(label, true);
        std::string text = bossScript ? bossScript->BossName() : "BOSS";
        // 極性記号を添える。塗られる的になった以上、«今どちらが乗っているか» は
        // 撃ち込めるかどうかそのもの。無極のときは何も足さない。
        if (polarity != Polarity::None) {
            text += "  ";
            text += PolaritySymbol(polarity);
        }
        // フェーズは 2 つしかないので «P2 に入った» ことだけ分かればよい。
        if (bossScript && bossScript->CurrentPhase() >= 2) text += "   PHASE 2";

        ui.SetText(label, text);
        ui.SetTextColor(label, polarity == Polarity::None ? neutralNameColor
                                                          : PolarityColor(polarity));
    }
}

} // namespace sandbox
