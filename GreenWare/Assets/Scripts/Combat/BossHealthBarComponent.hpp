/// @file    BossHealthBarComponent.hpp
/// @brief   ボスの残り部位と、敵ごとの崩し・とどめの猶予を表示する。
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// @note シーンに置いた実体をスクリプトが更新する作り (PlayerHealthBarComponent と同型):
///       位置/太さを絵合わせで頻繁に触るため、ランタイム生成だと Canvas Editor で掴めない。
/// @note ボット不在時は隠す (0 のまま出しっぱなしだと倒し切ったのに残って見える)。減り方は
///       敵の頭上バー (Drain) と同じ名前・規則で揃える (片方だけ違うと画面内で規則が割れる)。
#pragma once

#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossHealthBarComponent : public Script {
    FBZZ_SCRIPT(BossHealthBarComponent)

public:
    FBZZ_GROUP("ボス")
    /// @note 明示の割り当てを先に見て、空のときだけ IBoss 名簿で探す。ボス側が名乗り忘れると
    ///       症状は「バーが出ない」だけになり、UI 側を疑って探し回ることになるため。
    FBZZ_REF(GameObject, bossObject, "ボス")
    FBZZ_TOOLTIP("体力を出す相手。未設定なら IBoss を実装したオブジェクトを盤面から探す")

    FBZZ_GROUP("HUD")
    /// 未設定なら名前で拾う。シーンを作り直しても既定の構成なら動く。
    FBZZ_REF(GameObject, bossFill, "Boss Fill")
    FBZZ_TOOLTIP("残量で塗り潰す UIImage。Fill Origin は Left にしておく")
    FBZZ_REF(GameObject, bossDrain, "Boss Drain")
    FBZZ_TOOLTIP("遅れて追いつく帯。Fill と同じ矩形・同じ Fill Origin で、"
                 "Sort Order だけ Fill より下にする。未設定でもバーは動く")
    FBZZ_REF(GameObject, bossBackground, "Boss Background")
    FBZZ_REF(GameObject, bossName, "Boss Name")
    FBZZ_TOOLTIP("ボス名を出す UIText")
    FBZZ_FIELD(std::string, materialFillParam, "fillRatio", "Fill Ratio Param")
    FBZZ_FIELD(std::string, materialColorParam, "fillColor", "Fill Color Param")

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "Background")
    FBZZ_FIELD_COLOR(fullColor,  (Vector4{ 0.95f, 0.62f, 0.18f, 1.00f }), "Full")
    FBZZ_TOOLTIP("満タン側の色。プレイヤーのバー (青緑) と混ざらない色にすること")
    FBZZ_FIELD_COLOR(emptyColor, (Vector4{ 0.95f, 0.20f, 0.15f, 1.00f }), "Empty")
    FBZZ_FIELD_COLOR(neutralNameColor, (Vector4{ 0.62f, 0.66f, 0.72f, 1.00f }), "Name (No BladeSide)")
    FBZZ_TOOLTIP("無極のときのボス名の色。極が乗ると 12.2 の極性色へ切り替わる")

    /// 倒れている間 (Docs/presentation.md)。残り部位とは別に攻める猶予を表示する
    /// 琥珀色はとどめを狙える時間にだけ使う。
    FBZZ_GROUP("Toppled")
    FBZZ_FIELD_COLOR(toppledColor,    (Vector4{ 1.00f, 0.84f, 0.30f, 1.00f }), "Toppled")
    FBZZ_TOOLTIP("倒れている間の色。琥珀は «今が攻め時» の色 (予兆の色と同じ語)")
    FBZZ_FIELD_RANGE(float, toppledPulseHz, 4.5f, "Toppled Pulse Hz", 0.0f, 12.0f)

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

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugRatio, 0.0f, "Ratio")
    FBZZ_FIELD_READ_ONLY(float, debugDrain, 0.0f, "Drain")
    FBZZ_FIELD_READ_ONLY(bool, debugVisible, false, "Visible")
    FBZZ_FIELD_READ_ONLY(std::string, debugBoss, "", "ボスのオブジェクト")
    FBZZ_TOOLTIP("実際に読んでいる相手。空のままならボスを見つけられていない")

    void OnStart() override;
    void OnLateUpdate() override;

private:
    /// 追従帯が «追いつき切った» とみなす残差。1 ピクセルに満たない差のために
    /// 撃破後のバーを画面へ残し続けないための床。
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
    /// 減った直後だけ速く、そこから指数で追いつく。
    float AdvanceDrain(float ratio);

    EntityRef m_fill;
    EntityRef m_drain;
    EntityRef m_background;
    EntityRef m_name;
    EntityRef m_summary;
    EntityRef m_plate;
    EntityRef m_breakFill[2];
    EntityRef m_breakBack[2];
    EntityRef m_breakLabel[2];
    void RefreshBreakRows(const std::vector<GameObject*>& bosses);

    /// 追従帯の現在値と、被弾を検出するための前フレームの残量。
    float m_drainRatio    = 1.0f;
    float m_lastRatio     = 1.0f;
    float m_holdRemaining = 0.0f;
    /// 「ボスが見つからない」を 1 度だけ言うためのラッチ。
    /// @note 見つからないと Hide() が毎フレーム走るだけで画面には何も出ないため、探す先を明示する。
    bool m_warnedNoBoss = false;
};

FBZZ_REFLECT(BossHealthBarComponent)


inline GameObject* BossHealthBarComponent::Resolve(const Ref<GameObject>& reference,
                                                   const char* name) const
{
    if (GameObject* object = reference.Get()) return object;
    return scene.Find(name, true);
}

inline void BossHealthBarComponent::OnStart()
{
    m_warnedNoBoss = false;
    debugBoss.clear();
    if (auto* object = scene.Find("HUD_BossSummary", true)) m_summary = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("HUD_BossPlate", true)) m_plate = EntityRef{ object->GetID() };
    for (int i = 0; i < 2; ++i) {
        const std::string suffix = std::to_string(i);
        if (auto* object = scene.Find("HUD_BreakFill" + suffix, true)) m_breakFill[i] = EntityRef{ object->GetID() };
        if (auto* object = scene.Find("HUD_BreakBack" + suffix, true)) m_breakBack[i] = EntityRef{ object->GetID() };
        if (auto* object = scene.Find("HUD_BreakLabel" + suffix, true)) m_breakLabel[i] = EntityRef{ object->GetID() };
    }

    /// @note 参照の解決は Play 開始時に 1 度だけ。毎フレーム名前で探すと、見つからない構成で
    ///       静かに全シーン走査を続けることになる。
    GameObject* fill = Resolve(bossFill, kFillName);
    if (!fill) {
        debug.LogError("BossHealthBarComponent: boss fill UIImage not found "
                       "(assign Boss Fill, or name it HUD_BossFill in the scene).");
        return;
    }
    m_fill = EntityRef{ fill->GetID() };

    /// @note 帯は欠けていても «量» は読める。無い構成でもバーは動かす (雑魚と違い、
    ///       こちらは 3 枚をシーンに置く形なので、1 枚足し忘れただけで HUD が死ぬのは重い)。
    if (GameObject* drain = Resolve(bossDrain, kDrainName))
        m_drain = EntityRef{ drain->GetID() };
    if (GameObject* background = Resolve(bossBackground, kBackgroundName))
        m_background = EntityRef{ background->GetID() };
    if (GameObject* label = Resolve(bossName, kNameName))
        m_name = EntityRef{ label->GetID() };

    /// @note 追従帯は «今の残量» から始める。満タンから始めると、DLL リロードや
    ///       途中参加の構成で «開始と同時に大ダメージが入った» という嘘が 1 回流れる。
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
        /// @note 回復と初期化。遅らせる理由がないので即座に合わせる。
        m_drainRatio    = ratio;
        m_holdRemaining = 0.0f;
    } else {
        /// @note 新しく減った瞬間だけ保持時間を入れ直す。連続で入れても毎回「溜め」が入る。
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
    /// @note 出てくる前のボスは「居ない」として扱う: 畳まれている間もバーを出すと、
    ///       まだ見ぬ相手の満タン体力が画面に居座る。明示割り当てでも同じ判定を通す
    ///       (探し方が変わっても「いつ出るか」は変えない)。IBoss を引けなくても通す:
    ///       名指しされている以上ボスという意図は明らかで、黙らせると原因が画面から消える。
    if (GameObject* assigned = bossObject.Get()) {
        if (!assigned->activeInHierarchy()) return nullptr;
        const auto* boss = IBoss::Of(assigned);
        return (!boss || boss->IsEngaged()) ? assigned : nullptr;
    }

    return FindBossOnBoard(scene);
}

inline void BossHealthBarComponent::Hide()
{
    debugVisible = false;
    debugRatio   = 0.0f;

    /// @note 表示はアルファでなく表示スイッチで消す: アルファだと「消えている」を色の値でしか
    ///       表せず誰かが色を書き戻すと出てしまう。幅は 0 にしない (次に出る枠の寸法を保つ)。
    if (GameObject* fill = m_fill.Resolve(scene))       ui.SetImageEnabled(fill, false);
    if (GameObject* drain = m_drain.Resolve(scene))     ui.SetImageEnabled(drain, false);
    if (GameObject* back = m_background.Resolve(scene)) ui.SetImageEnabled(back, false);
    if (GameObject* label = m_name.Resolve(scene))      ui.SetTextEnabled(label, false);
    if (auto* object = m_summary.Resolve(scene)) ui.SetTextEnabled(object, false);
    if (auto* object = m_plate.Resolve(scene)) ui.SetImageEnabled(object, false);
    for (int i = 0; i < 2; ++i) {
        if (auto* object = m_breakFill[i].Resolve(scene)) ui.SetImageEnabled(object, false);
        if (auto* object = m_breakBack[i].Resolve(scene)) ui.SetImageEnabled(object, false);
        if (auto* object = m_breakLabel[i].Resolve(scene)) ui.SetTextEnabled(object, false);
    }
}

inline void BossHealthBarComponent::RefreshBreakRows(const std::vector<GameObject*>& bosses)
{
    for (int i = 0; i < 2; ++i) {
        GameObject* boss = i < static_cast<int>(bosses.size()) ? bosses[i] : nullptr;
        const auto* health = boss ? scene.GetScript<EnemyHealthComponent>(boss) : nullptr;
        const auto* meter = boss ? scene.GetScript<BossBreakComponent>(boss) : nullptr;
        const bool visible = health && meter;
        const bool alive = visible && health->IsAlive();
        const bool down = alive && meter->IsToppled();
        const float amount = alive ? Clamp01(meter->Ratio()) : 0.0f;
        const Vector4 tint = down ? toppledColor : Vector4{ 0.54f, 0.85f, 0.91f, 1.0f };
        if (auto* object = m_breakBack[i].Resolve(scene)) ui.SetImageEnabled(object, visible);
        if (auto* object = m_breakFill[i].Resolve(scene)) {
            ui.SetImageEnabled(object, visible);
            ui.SetImageFillAmount(object, amount);
            ui.SetImageColor(object, tint);
        }
        if (auto* object = m_breakLabel[i].Resolve(scene)) {
            ui.SetTextEnabled(object, visible);
            if (!visible) continue;
            std::string label = bosses.size() > 1 ? std::to_string(i + 1) + "体目  " : "";
            const auto* identity = IBoss::Of(boss);
            if (bosses.size() > 1 && alive && identity)
                label += "第" + std::to_string(identity->CurrentPhase()) + "段階  ";
            const char* opening = identity && identity->UsesBodyHitbox()
                ? (identity->IsToppled() ? (identity->CanExecute() ? "連撃 / とどめの好機" : "本体へ連撃！") : "落下中")
                : "とどめの好機";
            label += !alive ? "撃破" : down ? opening
                : "崩し  " + std::to_string(static_cast<int>(amount * 100.0f)) + "%";
            ui.SetText(object, label);
            ui.SetTextColor(object, alive ? tint : neutralNameColor);
        }
    }
}

inline void BossHealthBarComponent::OnLateUpdate()
{
    GameObject* fill = m_fill.Resolve(scene);
    if (!fill) return;

    GameObject* boss = FindBoss();
    const auto* health = boss ? scene.GetScript<EnemyHealthComponent>(boss) : nullptr;
    debugBoss = boss ? boss->name : std::string{};

    /// @note 出ていないのと、居ないのと、倒し切ったのは別の話。
    ///       出ていない … 最終 Wave まで畳まれている正しい状態。黙って隠す
    ///       居ない     … 組み方の間違い。畳まれたものすら 1 体も無いときだけ名指しで言う
    ///       倒し切った … 下で 0 まで減らしてから引く
    if (!boss || !health) {
        if (!m_warnedNoBoss) {
            /// @note 割り当てがあるなら «まだ出ていないだけ»。全走査はそこで打ち切る。
            const bool authored = bossObject.IsAssigned()
                               || !IBoss::Registry().empty();
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
    /// @note 倒れてすぐ引かない: 最後の 1 発だけ非表示だと自分の一撃で倒したか判別できない。
    ///       0 まで減り切るのを見せてから追従帯が追いつくのを待つ (畳むまでの猶予は
    ///       BossDeathVfxComponent の崩壊が持つ)。
    const auto* bossScript = IBoss::Of(boss);

    /// @note バーが描くのは「残り部位」(脚を全部落とすのが勝利条件)。PartsTotal が 0 の
    ///       構成 (Serpent 以外の将来のボス) では 0 除算せず従来どおり HP を描く。1 本の
    ///       バーに盤面の全員を足す (2026-09-15): 代表 1 体だけ描くと片方を倒した瞬間に
    ///       もう 1 体の満タンへ跳ね上がり、積み上げた戦いが巻き戻って見える。
    int   bossCount  = 0;
    int   aliveCount = 0;
    int   partsLeft  = 0;
    int   partsTotal = 0;
    float healthSum  = 0.0f;
    bool bodyHealth = false;
    bool  toppled    = false;
    int   maxPhase   = 1;

    std::vector<GameObject*> bosses;
    CollectBossesOnBoard(scene, bosses);
    /// @note 名乗っていない相手を明示で割り当てている構成 (FindBoss の @note 参照) では名簿に居ない。
    if (std::find(bosses.begin(), bosses.end(), boss) == bosses.end()) bosses.push_back(boss);
    std::sort(bosses.begin(), bosses.end(), [](const GameObject* left, const GameObject* right) {
        return left->name < right->name;
    });

    for (GameObject* other : bosses) {
        const auto* otherHealth = scene.GetScript<EnemyHealthComponent>(other);
        if (!otherHealth) continue;
        ++bossCount;
        const bool otherAlive = otherHealth->IsAlive();
        if (otherAlive) ++aliveCount;

        const auto* script = IBoss::Of(other);
        bodyHealth = bodyHealth || (script && script->UsesBodyHitbox());
        const int   total  = script ? script->PartsTotal() : 0;
        const int   left   = script ? script->PartsRemaining() : -1;
        if (total > 0 && left >= 0) {
            partsTotal += total;
            /// @note 倒した相手は 0 本。残り節は最小値で止まる (SerpentBody の minSegments) ので、
            ///       生死を見ないと «倒したのにまだ 6 本ある» がバーに残る。
            partsLeft += otherAlive ? left : 0;
        }
        healthSum += otherAlive ? otherHealth->Normalized() : 0.0f;

        const auto* otherBreak = scene.GetScript<BossBreakComponent>(other);
        if (otherAlive && otherBreak && otherBreak->IsToppled()) toppled = true;
        if (script) maxPhase = std::max(maxPhase, script->CurrentPhase());
    }

    const bool  byParts = partsTotal > 0 && !bodyHealth;
    const float ratio = byParts
        ? static_cast<float>(partsLeft) / static_cast<float>(partsTotal)
        : (bossCount > 0 ? healthSum / static_cast<float>(bossCount) : 0.0f);
    const float drain = AdvanceDrain(ratio);
    debugRatio = ratio;
    debugDrain = drain;

    if (aliveCount == 0 && drain <= kDrainSettled) {
        Hide();
        return;
    }
    debugVisible = true;
    if (auto* object = m_plate.Resolve(scene)) ui.SetImageEnabled(object, true);
    RefreshBreakRows(bosses);
    if (auto* object = m_summary.Resolve(scene)) {
        ui.SetTextEnabled(object, false);
    }

    const Vector4 base = toppled
        ? toppledColor
        : emptyColor + (fullColor - emptyColor) * ratio;

    float pulse = 1.0f;
    /// @note 倒れている間は «今が攻め時» の明滅。残り時間が減る動きと重なって急かす。
    if (toppled && toppledPulseHz > 0.0f) {
        const float phase = Time::unscaledTime * toppledPulseHz * TWO_PI;
        pulse = 1.0f + std::sin(phase) * 0.22f;
    }

    /// @note アルファは動かさない。透けると背景の明暗でバーの読みが変わる。
    const Vector4 color{ base.x * pulse, base.y * pulse, base.z * pulse, base.w };

    if (GameObject* back = m_background.Resolve(scene)) {
        ui.SetImageEnabled(back, true);
        ui.SetImageColor(back, backgroundColor);
    }

    /// @note 帯は「さっきまであった量」なので本体より下 (Sort Order 小: 背景 0 / 帯 1 / 本体 2)
    ///       に敷いて減った区間をはみ出させる。帯は明滅させない: 明滅は「撃ち込める窓」を
    ///       表す語で、帯 (直前に入った量) に使うと合図が 2 つの意味に割れる。
    if (GameObject* drainObject = m_drain.Resolve(scene)) {
        ui.SetImageEnabled(drainObject, true);
        ui.SetImageFillAmount(drainObject, drain);
        ui.SetImageColor(drainObject, drainColor);
        if (!materialFillParam.empty())
            ui.SetMaterialFloat(drainObject, materialFillParam, drain);
        if (!materialColorParam.empty())
            ui.SetMaterialColor(drainObject, materialColorParam, drainColor);
    }

    /// @note 表示は毎フレーム押し直す。Hide() が落としたスイッチを戻すのはここだけで、
    ///       «バーが一度も出ない» の芽をこの 1 行に閉じ込めておく。
    ui.SetImageEnabled(fill, true);
    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, color);
    if (!materialFillParam.empty())
        ui.SetMaterialFloat(fill, materialFillParam, ratio);
    if (!materialColorParam.empty())
        ui.SetMaterialColor(fill, materialColorParam, color);

    if (GameObject* label = m_name.Resolve(scene)) {
        ui.SetTextEnabled(label, true);
        std::string text = bossScript ? bossScript->BossName() : "BOSS";

        ui.SetText(label, text);
        ui.SetTextColor(label, toppled ? toppledColor : neutralNameColor);
    }
}

} // namespace sandbox
