// FBZZ Engine
// PolarityGunComponent.hpp | sandbox
// 二丁拳銃。左右独立のクールダウンで極性だけを付与する。
//
//   右入力 → 右銃 ＋極 / 赤
//   左入力 → 左銃 −極 / 青
//
// WHY ダメージを持たないか:
//   企画書の前提 (2 章)「銃は敵を倒さない。倒すのは衝突である」。
//   ここに 1 行でもダメージ処理を足すと、企画そのものが別のゲームになる。
//
// WHY hitscan にするか:
//   6 章はエイム補助を入れると明言している。当たりが確定している以上、飛翔体で
//   当たり判定を取り直す意味は無い。判定は発射の瞬間に確定させ、エネルギー弾は
//   見た目としてだけ飛ばす。12.1 が最優先とした「強いヒット音・即座の硬直」も
//   即時判定でないと出せない (弾の到達を待つと手応えが遅れる)。
#pragma once

#include <Scripts/Data/PolarityTuning.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/CooldownTimer.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PolarityGunComponent : public Script {
    FBZZ_SCRIPT(PolarityGunComponent)

    // SE はここから鳴らす。無くても撃てるが 12.1 の「強いヒット音」が丸ごと消える。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("クールダウンと持続時間。未割り当てだと既定値 (CD 1.2s) で動く")

    FBZZ_GROUP("Muzzles")
    // 発射エフェクトの発生位置。未設定でも銃モデル内の muzzle ソケットを名前で復旧する。
    FBZZ_REF(GameObject, muzzleRight, "Muzzle Right (+)")
    FBZZ_REF(GameObject, muzzleLeft,  "Muzzle Left (-)")

    FBZZ_GROUP("Input")
    // 11 章の操作表: 左入力 = 左銃 (−) / 右入力 = 右銃 (＋)。
    // 左右の入力と左右の銃を一致させることが操作の直感性そのものなので、
    // ここを入れ替えられるようにはしていない。
    FBZZ_FIELD(bool, useMouseButtons, true, "Use Mouse Buttons")
    FBZZ_FIELD(KeyCode, keyPlus,  KeyCode::E, "Plus Key (fallback)")
    FBZZ_FIELD(KeyCode, keyMinus, KeyCode::Q, "Minus Key (fallback)")

    FBZZ_GROUP("Feedback (12.1)")
    // 12.1 は「4 週目の調整項目ではなく 1 週目から入れる」と名指ししている。
    // 極性弾にダメージが無い以上、撃った手応えが無いと開始 1 分で投げられる。
    FBZZ_FIELD_FILE(sfxFire,       "", "SFX Fire",       ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxHit,        "", "SFX Hit",        ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxNeutralize, "", "SFX Neutralize", ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxEmpty,      "", "SFX Empty",      ".wav,.ogg")
    FBZZ_TOOLTIP("クールダウン中に撃とうとしたときの空撃ち音")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugTracer, false, "Draw Debug Tracer")

    // UI のクールダウンゲージ用。1 = 撃てる / 0 = 撃った直後。
    [[nodiscard]] float ChargeOf(Polarity polarity) const;
    [[nodiscard]] bool  CanFire(Polarity polarity) const;

    void OnStart()  override;
    void OnUpdate() override;

private:
    void TryFire(Polarity polarity);
    // 実際に極性を乗せてクールダウンを消費する。
    void ApplyToTarget(Polarity polarity, GameObject& targetObject,
                       PolarityTargetComponent& target);
    void PlayFireFeedback(Polarity polarity, const PolarityResult& result,
                          const Vector3& targetPos);
    [[nodiscard]] float CooldownSeconds() const;
    [[nodiscard]] Vector3 MuzzlePosition(Polarity polarity) const;

    CooldownTimer m_plus;
    CooldownTimer m_minus;
};

FBZZ_REFLECT(PolarityGunComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline float PolarityGunComponent::CooldownSeconds() const
{
    return tuning ? tuning->gunCooldown : 1.2f;
}

inline float PolarityGunComponent::ChargeOf(Polarity polarity) const
{
    const CooldownTimer& timer = (polarity == Polarity::Plus) ? m_plus : m_minus;
    return timer.Charge(CooldownSeconds());
}

inline bool PolarityGunComponent::CanFire(Polarity polarity) const
{
    return (polarity == Polarity::Plus) ? m_plus.IsReady() : m_minus.IsReady();
}

inline void PolarityGunComponent::OnStart()
{
    m_plus.Reset();
    m_minus.Reset();

    // 7.7 の「破ってはいけない設計上の制約」をここで検算する。
    //     敵の極性持続時間 ＞ クールダウン × 2 ＋ 狙いを定める時間
    // 破っていると 2 体目を撃つ前に 1 体目の極性が切れ、ゲームが成立しない。
    // 破綻の仕方が「なんとなく繋がらない」なので、明示的に言わないと気付けない。
    if (tuning) {
        constexpr float kAimSecondsEstimate = 0.6f; // 振り向いて狙いを付けるまでの見積もり
        const float shortest = tuning->ShortestDuration();
        if (!tuning->SatisfiesTimingConstraint(shortest, kAimSecondsEstimate)) {
            debug.LogError(
                "PolarityTuning breaks the 7.7 timing constraint: shortest duration "
                "must exceed cooldown x 2 + aim time. Combos will not connect.");
        }
    }
}

inline void PolarityGunComponent::OnUpdate()
{
    const float dt = Time::deltaTime;
    m_plus.Tick(dt);
    m_minus.Tick(dt);

    // 右 = ＋ / 左 = −。左右の入力と左右の銃を一致させる (11 章)。
    const bool firePlus = useMouseButtons
        ? input.MouseButtonDown(MouseBtn::Right)
        : input.GetKeyDown(keyPlus);
    const bool fireMinus = useMouseButtons
        ? input.MouseButtonDown(MouseBtn::Left)
        : input.GetKeyDown(keyMinus);

    if (firePlus)  TryFire(Polarity::Plus);
    if (fireMinus) TryFire(Polarity::Minus);
}

inline void PolarityGunComponent::TryFire(Polarity polarity)
{
    if (!CanFire(polarity)) {
        // 空撃ちにも音を返す。無反応だと「入力が拾われていない」のか
        // 「クールダウン中」なのか区別できず、リズムを覚えられない。
        if (!sfxEmpty.empty()) audio.PlayOneShot(sfxEmpty);
        return;
    }

    auto* aim = scene.GetScript<PlayerAimComponent>();
    if (!aim) {
        // 6 章のエイム補助はこのゲームの前提。無い状態で撃てるようにすると、
        // 「当たらない」原因がプレイヤーの腕前に見えてしまう。
        debug.LogError("PolarityGunComponent requires PlayerAimComponent on the same object.");
        return;
    }

    GameObject* targetObject = aim->CurrentTarget();
    auto* target = aim->CurrentPolarityTarget();
    if (!targetObject || !target) {
        if (!sfxEmpty.empty()) audio.PlayOneShot(sfxEmpty);
        return;
    }

    // 対象が居て撃てる。ここから先は必ずクールダウンを消費する。
    // 柱へ置いた場合も同じだけ消費する (7.6 の「敵を狙うか柱に置くかの選択」)。
    ApplyToTarget(polarity, *targetObject, *target);
}

inline void PolarityGunComponent::ApplyToTarget(Polarity polarity,
                                                GameObject& targetObject,
                                                PolarityTargetComponent& target)
{
    const PolarityResult result = target.Apply(polarity);

    CooldownTimer& timer = (polarity == Polarity::Plus) ? m_plus : m_minus;
    timer.Trigger(CooldownSeconds());

    PlayFireFeedback(polarity, result, targetObject.transform.worldPosition);
}

inline void PolarityGunComponent::PlayFireFeedback(Polarity polarity,
                                                   const PolarityResult& result,
                                                   const Vector3& targetPos)
{
    if (!sfxFire.empty()) audio.PlayOneShot(sfxFire);

    // 12.4 は「中和・延長した瞬間にも明確なフィードバックを返す」を仕様として要求する。
    // 同じ音で済ませると、狙って中和したのか事故だったのかが耳で判別できない。
    switch (result.change) {
    case PolarityChange::Neutralized:
        if (!sfxNeutralize.empty()) audio.PlayOneShot(sfxNeutralize);
        break;
    case PolarityChange::Applied:
    case PolarityChange::Extended:
        if (!sfxHit.empty()) audio.PlayOneShot(sfxHit);
        break;
    }

    if (drawDebugTracer) {
        debug.DrawLine(MuzzlePosition(polarity), targetPos,
                       PolarityColor(polarity), 0.15f);
    }
}

inline Vector3 PolarityGunComponent::MuzzlePosition(Polarity polarity) const
{
    const auto& muzzle = (polarity == Polarity::Plus) ? muzzleRight : muzzleLeft;
    if (GameObject* object = muzzle.Get())
        return object->transform.worldPosition;

    // 旧シーンで EntityRef が未保存でも、銃モデル内のソケットを使う。
    // WHY: Player 原点へフォールバックすると、銃を手へ移した後も弾道だけが原点から出る。
    const char* socketName = polarity == Polarity::Plus
        ? "Sock_Muzzle_R"
        : "Sock_Muzzle_L";
    if (GameObject* socket = scene.Find(socketName))
        return socket->transform.worldPosition;
    return transform.worldPosition;
}

} // namespace sandbox
