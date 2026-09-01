/// @file    SerpentDeathVfxComponent.hpp
/// @brief   蛇の撃破 ─ 頭から尾へ順に爆ぜ、最後に頭が落ちる。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY ボス 1 の BossDeathVfxComponent を使い回さないか:
///   あちらは «四足の体のあちこちを跨ぐ» ための固定のボーン名表 (Head / Thigh_BL /
///   Hock_FR …) が演出そのもので、順序を «散らす» ことに意味がある。蛇は 1 本の鎖で、
///   散らすと «どこで何が起きたか» が読めない ─ 逆に **頭から尾へ順に走らせる**ことが
///   «長いものが端から死んでいく» の読みになる。共有できるのは減衰の式だけ
///   (shock::NearnessTo を両方が使う)。
///
/// WHY 床下の節では爆ぜないか:
///   撃破の瞬間、24 m のうち地上に出ているのは 11 m ほどで、残りは縦坑の中にいる。
///   そこで爆ぜても画面には何も出ず、«間が空いた» としか見えない。走らせるのは
///   «見えている胴» の上だけにして、間隔はその長さから割る。
///
/// WHY 決定打を頭に置くか:
///   終盤まで頭は床下にいて触れず、«頭を斬りたい» だけが動機として置かれている
///   (boss-serpent.md「撃破までの形」)。最後の 1 発が頭で起きれば、その動機に
///   決着が付く。
///
/// WHY ここで体を溶かさないか:
///   ディゾルブと粒は EnemyDeathVfxComponent が持つ (雑魚もボス 1 も同じ口)。
///   こちらは «決定打までの 1.4 秒» だけを受け持ち、あちらの Body Dissolve > Delay と
///   同じ秒数で終わる ─ 崩れ始めが決定打に重なる。
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
#include <Scripts/Utils/PolarityTypes.hpp>
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
    FBZZ_GROUP("Target")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")
    FBZZ_TOOLTIP("衝撃の減衰を測る相手。パッドを持っているのはプレイヤー本人")

    FBZZ_GROUP("Break Up")
    FBZZ_FIELD_RANGE(float, buildupSeconds, 1.40f, "Buildup", 0.1f, 8.0f)
    FBZZ_TOOLTIP("決定打までの秒数。EnemyDeathVfxComponent の Body Dissolve > Delay と "
                 "同じ値にすること (崩れ始めが決定打に重なる)")
    FBZZ_FIELD_RANGE_INT(int, blastCount, 10, "Blast Count", 0, 32)
    FBZZ_TOOLTIP("頭から尾へ走らせる爆発の数。地上に出ている胴の上だけに置く")
    FBZZ_FIELD_RANGE(float, blastSpread, 0.45f, "Blast Spread", 0.0f, 6.0f)
    FBZZ_TOOLTIP("節の芯からどれだけ散らすか [m]。0 だと関節の中心で毎回光る")
    FBZZ_FIELD_RANGE(float, blastStrengthStart, 0.30f, "Strength (first)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blastStrengthEnd, 0.70f, "Strength (last)", 0.0f, 1.0f)

    FBZZ_GROUP("Head Detonation")
    FBZZ_FIELD_RANGE(float, climaxStrength, 1.0f, "Strength", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, climaxLift, 0.5f, "Lift", -4.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, climaxSurgeSeconds, 0.55f, "Surge", 0.0f, 3.0f)
    FBZZ_FIELD_COLOR(climaxSurgeColor, (Vector4{ 1.00f, 0.72f, 0.30f, 1.00f }), "Surge Color")
    FBZZ_TOOLTIP("ディゾルブの縁と粒に合わせた琥珀。極性色 (赤 / 青) を使うと "
                 "«どちらかの極でやられた» と読める")

    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "Shake Ratio", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, feedbackRange, 30.0f, "Falloff Range", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, feedbackNear, 4.0f, "Full Strength Within", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, blastVolume, 0.85f, "Blast Volume", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, climaxVolume, 1.0f, "Detonation Volume", 0.0f, 2.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugStage, "Idle", "Stage")
    FBZZ_FIELD_READ_ONLY(int, debugBlastsFired, 0, "Blasts Fired")

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
    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag); }

    void Blast(const Vector3& point, float strength01, float volume, const se::Bank& bank) const;
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

    // 指した節が床下や潰れた節だったら、そこから尾側へ探す。見つからなければ頭側へ。
    for (int i = from; i <= serpent::kSegmentCount; ++i)
        if ((!body || body->IsAlive(i)) && spine->IsExposed(i)) return i;
    for (int i = from; i >= 1; --i)
        if ((!body || body->IsAlive(i)) && spine->IsExposed(i)) return i;
    return 0;   // 頭。地上に 1 節も無いときの最後の拠り所
}

inline Vector3 SerpentDeathVfxComponent::BlastPoint(int index) const
{
    const auto* spine = Spine();
    if (!spine) return transform.worldPosition;

    // 頭から尾へ等間隔。節の «番号» で割るので、折られて短くなった胴でも端まで届く。
    const int count = Max(blastCount, 1);
    const int seg   = 1 + (index * (serpent::kSegmentCount - 1)) / Max(count - 1, 1);

    Vector3 point = spine->JointPosition(NearestLiveSegment(seg));

    // 黄金角で散らす。番号から一意に決まるので、同じ撃破は何度見ても同じ形になる。
    const float angle  = static_cast<float>(index) * 2.39996323f;
    const float radius = Max(blastSpread, 0.0f);
    point.x += std::cos(angle) * radius;
    point.z += std::sin(angle) * radius;
    point.y += std::sin(angle * 1.7f) * radius * 0.6f;
    return point;
}

inline void SerpentDeathVfxComponent::Blast(const Vector3& point, float strength01, float volume,
                                            const se::Bank& bank) const
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(point, Polarity::None, strength, /*againstAnchor=*/false);
    se::PlayAt(audio, bank, point, volume);

    // 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    // 震える距離ができて «どこで起きたか» の答えが 2 つになる。
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

    // WHY 実時間ではなくスケール時間か: 決定打はディゾルブが始まる瞬間と重なっていないと
    //     «崩れ始めたのに何も起きない» が出る。EnemyDeathVfxComponent が Time::deltaTime で
    //     数えている以上、こちらだけ実時間で数えるとヒットストップのぶんだけずれる。
    m_elapsed += Time::deltaTime;

    const int   count   = Max(blastCount, 0);
    const float buildup = Max(buildupSeconds, 0.0f);

    while (m_next < count) {
        const float at = buildup * (static_cast<float>(m_next) + 0.5f)
                       / static_cast<float>(count + 1);
        if (m_elapsed < at) break;

        const float t = count > 1
            ? static_cast<float>(m_next) / static_cast<float>(count - 1) : 1.0f;
        Blast(BlastPoint(m_next), Lerp(blastStrengthStart, blastStrengthEnd, t), blastVolume,
              se::kImpactHeavy);
        ++m_next;
        debugBlastsFired = m_next;
    }

    if (m_climaxed || m_elapsed < buildup) return;

    // 決定打は頭。ここからディゾルブが体を食い始める。
    m_climaxed = true;
    debugStage = "Head Detonation";

    Vector3 head = Spine() ? Spine()->HeadPosition() : transform.worldPosition;
    head.y += climaxLift;
    Blast(head, climaxStrength, climaxVolume, se::kSerpentDestroy);
    ++debugBlastsFired;

    // WHY 白いフラッシュではなくサージか: 画面を塗る白は «こちらが受けた» を表す語で、
    //     倒した瞬間に出すと被弾と読み違える。縁だけを灼く色付きの一撃なら、
    //     盤面の中央で起きたことを隠さずに済む。
    if (climaxSurgeSeconds > 0.0f) {
        if (auto* screen = ScreenEffectManagerComponent::Instance())
            screen->Surge(climaxSurgeColor, Clamp01(climaxStrength), climaxSurgeSeconds);
    }
}

} // namespace sandbox
