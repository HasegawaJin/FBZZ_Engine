/// @file    HitstopManagerComponent.hpp
/// @brief   ヒットストップの要求を受け付け、長さと強さを 1 箇所で決める
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 呼び出し側から切り離すか:
///   ヒットストップは「当たった」と感じさせる主要な手段で (17 章)、当たる場所が増えるほど
///   呼び出し元も増える。長さと強さを呼び出し元それぞれが持つと、全体の重さを
///   調整したいときに全部を回ることになり、しかも各所の値が少しずつずれていく。
///   呼び出し元が渡すのは「どれくらい強い当たりか (0..1)」だけにして、
///   それを何秒どの深さで止めるかはここが決める。
///
/// WHY 重ねずに 1 本へ畳むか:
///   同じフレームに複数の衝突が起きるのは普通で、素直に足すと弱い衝突が重なっただけで
///   画面が長時間止まる。強い方の深さと長い方の残り時間を採り、常に 1 本として扱う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class HitstopManagerComponent : public Script {
    FBZZ_SCRIPT(HitstopManagerComponent)

public:
    FBZZ_GROUP("Strength")
    FBZZ_FIELD_RANGE(float, maxSeconds, 0.09f, "Max Seconds", 0.0f, 0.5f)
    FBZZ_TOOLTIP("最も強い当たりで止まる長さ。実際の長さは強さ (0..1) に比例して縮む")
    FBZZ_FIELD_RANGE(float, minSeconds, 0.02f, "Min Seconds", 0.0f, 0.5f)
    FBZZ_TOOLTIP("弱い当たりでも最低これだけは止める。0 にすると軽い接触が無反応になる")
    FBZZ_FIELD_RANGE(float, timeScale, 0.05f, "Time Scale", 0.0f, 1.0f)
    FBZZ_TOOLTIP("停止中のタイムスケール。0 で完全停止。少し流した方が固まって見えにくい")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")

    [[nodiscard]] static HitstopManagerComponent* Instance() { return s_instance; }

    // 当たりの強さ (0..1) から長さを決めて止める。呼び出し側の標準手段。
    void Hit(float strength01);
    // 長さと深さを直接指定する。演出上どうしても個別に決めたい場所だけで使う。
    void Request(float seconds, float scale);
    void Cancel();

    [[nodiscard]] bool IsActive() const { return m_remaining > 0.0f; }

    // 今かかっている止めの重さ 0..1。止まっていなければ 0。
    //
    // WHY 深さではなく長さで測るか: Hit() が強さから動かすのは長さだけで、深さ
    //     (timeScale) は 1 つの値を全員で共有している。深さを返すと、軽い接触も
    //     全力の激突も同じ数字になる。
    //
    // WHY 公開するか: 止めに重ねる画面効果 (ScreenEffectManagerComponent の Freeze)
    //     は、止めと «同じ重さ» でなければならない。最低の 0.02 秒 = 60fps で
    //     1 フレームの止めに全力の絵を出すと、当たりではなく描画のちらつきに見える。
    //     受け取る側が長さから逆算すると、Hit() の対応表をもう 1 つ持つことになる。
    [[nodiscard]] float Weight01() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override { Cancel(); if (s_instance == this) s_instance = nullptr; }

private:
    static inline HitstopManagerComponent* s_instance = nullptr;

    float m_remaining    = 0.0f;
    float m_scale        = 1.0f;
    // 今の止めが «始まったときの» 長さ。残りだけでは重さが測れない
    // (解除の直前はどんな止めでも残り 0 になる)。
    float m_seconds      = 0.0f;
    bool  m_warnedNoTime = false;
};

FBZZ_REFLECT(HitstopManagerComponent)

inline void HitstopManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("HitstopManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance     = this;
    m_remaining    = 0.0f;
    m_scale        = 1.0f;
    m_seconds      = 0.0f;
    m_warnedNoTime = false;
}

inline float HitstopManagerComponent::Weight01() const
{
    if (m_remaining <= 0.0f) return 0.0f;
    // Hit() が minSeconds〜maxSeconds へ写した長さを、そのまま逆に読む。
    // Request() を直接叩いた場合はこの範囲の外へ出るので、両端で止める。
    //
    // WHY Option の倍率を掛け戻すか: m_seconds は倍率を掛けた «後» の長さ。素の
    //     範囲と比べると、止めを弱める設定にしただけで全部の当たりが軽い判定になり、
    //     «どれくらい強い当たりだったか» が設定で変わってしまう。
    const float optionScale = std::max(GameSettingsComponent::HitstopScale(), EPSILON);
    const float low  = std::max(minSeconds, 0.0f) * optionScale;
    const float high = std::max(maxSeconds, 0.0f) * optionScale;
    if (high - low <= EPSILON) return 1.0f;
    return Clamp01((m_seconds - low) / (high - low));
}

inline void HitstopManagerComponent::Hit(float strength01)
{
    const float strength = Clamp01(strength01);
    const float seconds  = Lerp(std::max(minSeconds, 0.0f), std::max(maxSeconds, 0.0f), strength);
    Request(seconds, timeScale);
}

inline void HitstopManagerComponent::Request(float seconds, float scale)
{
    // Option の「ヒットストップ」。長さを縮める形で効かせる。
    // WHY 深さ (scale) ではなく長さか: 深さを浅くすると「止まったのにすぐ動く」
    //     半端な引っ掛かりになる。短くすれば 0 で完全に無くなり、途中の値も
    //     「軽く止まる」として素直に読める。
    seconds *= GameSettingsComponent::HitstopScale();
    if (seconds <= 0.0f) return;

    // 強い方の深さと長い方の残りを採る。足し合わせると弱い衝突の重なりで長時間止まる。
    m_remaining = std::max(m_remaining, seconds);
    m_scale     = std::min(m_scale, Clamp01(scale));
    // 重さも «長い方» に揃える。残りと別々に選ぶと、重ねた瞬間に長さと重さが
    // 食い違い、Weight01() が 1 本の止めを表さなくなる。
    m_seconds   = std::max(m_seconds, seconds);

    // WHY OnStart で有無を確かめないか: スクリプトの並び順によっては TimeManager の
    //     OnStart が後になる。並び順に依存した警告は、順番を入れ替えただけで嘘になる。
    if (auto* timeManager = TimeManagerComponent::Instance()) {
        timeManager->SetOverride(m_scale);
    } else if (!m_warnedNoTime) {
        m_warnedNoTime = true;
        debug.LogError("HitstopManagerComponent requires a TimeManagerComponent in the scene "
                       "(the time scale is only written there).");
    }
}

inline void HitstopManagerComponent::Cancel()
{
    m_remaining    = 0.0f;
    m_scale        = 1.0f;
    m_seconds      = 0.0f;
    debugRemaining = 0.0f;
    if (auto* timeManager = TimeManagerComponent::Instance())
        timeManager->ClearOverride();
}

inline void HitstopManagerComponent::OnUpdate()
{
    if (m_remaining <= 0.0f) return;

    // WHY 実時間で数えるか: 止めている当人が縮んだ時間で残りを数えると、
    //     深く止めるほど解除が遅れる。timeScale 0 では永久に戻らない。
    m_remaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
    debugRemaining = std::max(m_remaining, 0.0f);
    if (m_remaining > 0.0f) return;

    Cancel();
}

} // namespace sandbox
