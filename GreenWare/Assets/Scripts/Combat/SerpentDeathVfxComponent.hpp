/// @file    SerpentDeathVfxComponent.hpp
/// @brief   蛇の撃破 ─ 頭から尾へ順に爆ぜ、最後に頭が落ちる。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note ボス 1 の BossDeathVfxComponent は流用しない。あちらは «四足の体のあちこちを
///       跨ぐ» ための固定ボーン名表で順序を散らすことに意味があるが、蛇は 1 本の鎖で
///       散らすと «どこで何が» が読めない。逆に頭から尾へ順に走らせることが «長いもの
///       が端から死んでいく» の読みになる (共有は `shock::NearnessTo` の減衰式のみ)。
/// @note 床下の節では爆ぜない。撃破の瞬間、地上に出ているのは 24 m 中 11 m ほどで、
///       残りは縦坑の中 ─ 爆ぜても «間が空いた» としか見えない。見えている胴の上だけ
///       走らせ、間隔はその長さから割る。
/// @note 決定打は頭に置く。頭は終盤まで床下にいて触れず «頭を斬りたい» だけが動機
///       として置かれている (boss-serpent.md「撃破までの形」)。最後の 1 発で決着させる。
/// @note 体はここで溶かさない。ディゾルブと粒は `EnemyDeathVfxComponent` が持ち
///       (雑魚もボス 1 も同じ口)、こちらは決定打までの `buildupSeconds` だけを受け持つ
///       ─ あちらの Body Dissolve > Delay と同じ秒数にして崩れ始めと決定打を重ねる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentDeathVfxComponent : public Script {
    FBZZ_SCRIPT(SerpentDeathVfxComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_TOOLTIP("衝撃の減衰を測る相手。パッドを持っているのはプレイヤー本人")

    FBZZ_GROUP("Break Up")
    FBZZ_FIELD_RANGE(float, buildupSeconds, 1.40f, "蓄積", 0.1f, 8.0f)
    FBZZ_TOOLTIP("決定打までの秒数。EnemyDeathVfxComponent の Body Dissolve > Delay と "
                 "同じ値にすること (崩れ始めが決定打に重なる)")
    FBZZ_FIELD_RANGE_INT(int, blastCount, 10, "爆発の数", 0, 32)
    FBZZ_TOOLTIP("頭から尾へ走らせる爆発の数。地上に出ている胴の上だけに置く")
    FBZZ_FIELD_RANGE(float, blastSpread, 0.45f, "爆発の拡がり", 0.0f, 6.0f)
    FBZZ_TOOLTIP("節の芯からどれだけ散らすか [m]。0 だと関節の中心で毎回光る")
    FBZZ_FIELD_RANGE(float, blastStrengthStart, 0.30f, "強度 (最初)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blastStrengthEnd, 0.70f, "強度 (最後)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blastBias, 2.0f, "Timing Bias", 0.5f, 5.0f)
    FBZZ_TOOLTIP("頭側を «速く»・尾側を «遅く» する強さ。1.0 で等間隔。"
                 "上げるほど «端から一気に走って、最後はゆっくり大きく» になる")
    FBZZ_FIELD_RANGE_INT(int, heavyEvery, 3, "Heavy Every", 1, 8)
    FBZZ_TOOLTIP("何発に 1 発を «爆発» にするか。残りは光も陽炎も持たない軽い破片。"
                 "1 にすると 10 発すべてが爆発になり、同時に生きる陽炎が 7〜8 枚"
                 "＝ 全画面コピーがその数だけ走る")

    FBZZ_GROUP("Head Detonation")
    FBZZ_FIELD_RANGE(float, climaxStrength, 1.0f, "強度", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, climaxLift, 0.5f, "浮き", -4.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, climaxSurgeSeconds, 0.55f, "サージ", 0.0f, 3.0f)
    FBZZ_FIELD_COLOR(climaxSurgeColor, (Vector4{ 1.00f, 0.72f, 0.30f, 1.00f }), "Surge Color")
    FBZZ_TOOLTIP("ディゾルブの縁と粒に合わせた琥珀。刀の色 (赤 / 青) を使うと "
                 "«どちらかの刀でやられた» と読める")

    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "揺れの比率", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, feedbackRange, 30.0f, "減衰の範囲", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, feedbackNear, 4.0f, "全開になる距離", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, blastVolume, 0.85f, "爆発の音量", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, climaxVolume, 1.0f, "起爆の音量", 0.0f, 2.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugStage, "Idle", "ステージ")
    FBZZ_FIELD_READ_ONLY(int, debugBlastsFired, 0, "発射した爆発")

    /// 撃破された瞬間に 1 度だけ呼ぶ。2 度目以降は無視する。
    void Begin();
    [[nodiscard]] bool IsRunning() const { return m_running; }
    /// 決定打までの秒数。畳む側はこれとディゾルブの長い方まで待つこと。
    [[nodiscard]] float TotalSeconds() const { return Max(buildupSeconds, 0.0f); }

    void OnUpdate() override;

private:
    [[nodiscard]] SerpentSpineComponent* Spine() const
    {
        return scene.GetScript<SerpentSpineComponent>();
    }
    [[nodiscard]] SerpentBodyComponent* Body() const
    {
        return scene.GetScript<SerpentBodyComponent>();
    }
    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag, true); }

    /// 1 発。heavy なら爆発 (光と陽炎を持つ)、そうでなければ軽い破片だけ。
    ///
    /// @note 全部を爆発にしない。1 発は焦げ跡まで含めて 4 秒あるので、1.4 秒に 10 発
    ///       置くと常時 7〜8 発が生きる。爆発は陽炎の層を持ち歪むエミッター 1 つにつき
    ///       全画面コピーが 1 回走るため、要所だけ爆発にして間は破片で繋ぐ。
    void Blast(const Vector3& point, float strength01, float volume, const se::Bank& bank,
               bool heavy) const;
    /// index 番目 (0 = 頭側) の爆発を置く点。
    [[nodiscard]] Vector3 BlastPoint(int index) const;
    /// 地上に出ていて生きている節のうち、headIndex 以降で最初に見つかるもの。
    [[nodiscard]] int NearestLiveSegment(int headIndex) const;

    float m_elapsed  = 0.0f;
    int   m_next     = 0;
    bool  m_running  = false;
    bool  m_climaxed = false;
};

FBZZ_REFLECT(SerpentDeathVfxComponent)

inline int SerpentDeathVfxComponent::NearestLiveSegment(int headIndex) const
{
    const auto* spine = Spine();
    const auto* body  = Body();
    if (!spine) return std::clamp(headIndex, 0, serpent::kSegmentCount);

    const int from = std::clamp(headIndex, 1, serpent::kSegmentCount);

    /// @note 指した節が床下や潰れた節だったら、そこから尾側へ探す。見つからなければ頭側へ。
    for (int i = from; i <= serpent::kSegmentCount; ++i)
        if ((!body || body->IsAlive(i)) && spine->IsExposed(i)) return i;
    for (int i = from; i >= 1; --i)
        if ((!body || body->IsAlive(i)) && spine->IsExposed(i)) return i;
    /// @note 頭。地上に 1 節も無いときの最後の拠り所
    return 0;
}

inline Vector3 SerpentDeathVfxComponent::BlastPoint(int index) const
{
    const auto* spine = Spine();
    if (!spine) return transform.worldPosition;

    /// @note 頭から尾へ等間隔。節の «番号» で割るので、折られて短くなった胴でも端まで届く。
    const int count = Max(blastCount, 1);
    const int seg   = 1 + (index * (serpent::kSegmentCount - 1)) / Max(count - 1, 1);

    Vector3 point = spine->JointPosition(NearestLiveSegment(seg));

    /// @note 黄金角で散らす。番号から一意に決まるので、同じ撃破は何度見ても同じ形になる。
    const float angle  = static_cast<float>(index) * 2.39996323f;
    const float radius = Max(blastSpread, 0.0f);
    point.x += std::cos(angle) * radius;
    point.z += std::sin(angle) * radius;
    point.y += std::sin(angle * 1.7f) * radius * 0.6f;
    return point;
}

inline void SerpentDeathVfxComponent::Blast(const Vector3& point, float strength01, float volume,
                                            const se::Bank& bank, bool heavy) const
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    if (auto* vfx = VfxManagerComponent::Instance()) {
        if (heavy) vfx->PlayImpact(point, BladeSide::None, strength, /*againstAnchor=*/false);
        /// @note 破片は «そこも爆ぜた» だけを言う。光も陽炎も持たないので何発重ねても軽い。
        else       vfx->PlaySerpentGeyser(point, strength, 0.6f + 0.7f * strength);
    }
    se::PlayAt(audio, bank, point, volume);

    /// @note 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    ///       震える距離ができて «どこで起きたか» の答えが 2 つになる。
    const float nearness = shock::NearnessTo(Player(), point, Max(feedbackRange, 1.0f),
                                             Max(feedbackNear, 0.0f));
    if (nearness <= 0.0f) return;

    const float weight = strength * nearness;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(weight);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(weight * Clamp01(shakeRatio));
}

inline void SerpentDeathVfxComponent::Begin()
{
    if (m_running) return;
    m_running  = true;
    m_climaxed = false;
    m_elapsed  = 0.0f;
    m_next     = 0;
    debugBlastsFired = 0;
    debugStage = "Breaking Up";

    se::EnsureSource(scene, "SE", 1.0f);
}

inline void SerpentDeathVfxComponent::OnUpdate()
{
    if (!m_running) return;

    /// @note 実時間でなくスケール時間で数える。決定打はディゾルブが始まる瞬間と重ならないと
    ///       «崩れ始めたのに何も起きない» が出る。`EnemyDeathVfxComponent` が
    ///       `Time::deltaTime` で数えている以上、実時間にするとヒットストップぶんずれる。
    m_elapsed += Time::deltaTime;

    const int   count   = Max(blastCount, 0);
    const float buildup = Max(buildupSeconds, 0.0f);

    while (m_next < count) {
        const float t = count > 1
            ? static_cast<float>(m_next) / static_cast<float>(count - 1) : 1.0f;

        /// @note 頭側を速く、尾側を遅く。等間隔だと強さの傾斜 (0.30 → 0.70) だけでは
        ///       «10 発が等間隔で鳴った» としか見えない。時間を曲げると頭側で一気に
        ///       走って尾で溜める形になり、頭の決定打までの «間» もそこで作れる。
        const float shape = std::pow(t, Max(blastBias, 0.5f));
        const float at    = buildup * (shape * static_cast<float>(count) + 0.5f)
                          / static_cast<float>(count + 1);
        if (m_elapsed < at) break;

        /// @note 要所だけ «爆発»。全部を爆発にすると同時に生きる陽炎が 7〜8 枚になる。
        const int  every = Max(heavyEvery, 1);
        const bool heavy = (m_next % every) == 0 || m_next == count - 1;
        Blast(BlastPoint(m_next), Lerp(blastStrengthStart, blastStrengthEnd, t), blastVolume,
              heavy ? se::kImpactHeavy : se::kImpactDebris, heavy);
        ++m_next;
        debugBlastsFired = m_next;
    }

    if (m_climaxed || m_elapsed < buildup) return;

    /// @note 決定打は頭。ここからディゾルブが体を食い始める。
    m_climaxed = true;
    debugStage = "Head Detonation";

    Vector3 head = Spine() ? Spine()->HeadPosition() : transform.worldPosition;
    head.y += climaxLift;
    Blast(head, climaxStrength, climaxVolume, se::kSerpentDestroy, /*heavy=*/true);
    ++debugBlastsFired;

    /// @note 白いフラッシュでなくサージにする。画面を塗る白は «こちらが受けた» を表す語で、
    ///       倒した瞬間に出すと被弾と読み違える。縁だけを灼く色付きの一撃なら、盤面の
    ///       中央で起きたことを隠さずに済む。
    if (climaxSurgeSeconds > 0.0f) {
        if (auto* screen = ScreenEffectManagerComponent::Instance())
            screen->Surge(climaxSurgeColor, Clamp01(climaxStrength), climaxSurgeSeconds);
    }
}

} // namespace sandbox
