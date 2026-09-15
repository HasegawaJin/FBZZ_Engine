/// @file    TimeManagerComponent.hpp
/// @brief   ゲーム内時間の速さを 1 箇所で決める
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 1 箇所に集めるか:
///   Time::timeScale は誰でも書けるグローバルな 1 変数で、書き手が 2 つになった瞬間に
///   壊れる。実際ヒットストップは「開始前のスケールを保存して
///   戻す」方式だったが、これはスローと重なると復帰後にスローの値を消してしまう。
///   逆にスロー側が後から書けばヒットストップが解除される。
///   「誰が最後に書いたか」で結果が決まる作りは、症状が入力タイミング依存になって
///   再現できない。ここを唯一の書き手にし、他は要求を出すだけにする。
///
/// WHY 演出のスローと停止を別枠にするか:
///   スロー (演出) と停止 (ヒットストップ・ポーズ) は解除の条件が違う。
///   同じ変数で表すと「スローの途中でヒットストップが入って抜けた」ときに、
///   スローへ戻すのか等速へ戻すのかを判断する材料が残らない。
///   常に「基準 → スロー → 上書き」の順で合成し、上書きが外れたら下の層へ戻る。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class TimeManagerComponent : public Script {
    FBZZ_SCRIPT(TimeManagerComponent)

public:
    FBZZ_GROUP("ブレンド")
    FBZZ_FIELD_RANGE(float, defaultBlendSeconds, 0.12f, "Default Blend", 0.0f, 2.0f)
    FBZZ_TOOLTIP("スローの掛け始め / 戻しに掛ける秒数。0 で瞬間的に切り替わる")
    FBZZ_FIELD_RANGE(float, minScale, 0.0f, "Min Scale", 0.0f, 1.0f)
    FBZZ_TOOLTIP("要求されたスケールの下限。0 未満や暴走値で完全停止するのを防ぐ")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugScale, 1.0f, "Current Scale")
    FBZZ_FIELD_READ_ONLY(std::string, debugSource, "Normal", "Source")

    // どこからでも呼べる入口。シーンに 1 つだけ置く前提。
    [[nodiscard]] static TimeManagerComponent* Instance() { return s_instance; }

    // --- 演出のスロー (下の層) ---
    // scale へ blendSeconds かけて寄せ、解除するまで維持する。
    void SetSlow(float scale, float blendSeconds = -1.0f);
    // 等速へ戻す。
    void ClearSlow(float blendSeconds = -1.0f);
    // seconds 秒だけスローにして自動で戻す。時間は実時間で数える。
    void SlowFor(float scale, float seconds, float blendSeconds = -1.0f);
    // 掛け始めと戻しで別の秒数を使う版。
    //
    // WHY 戻しを別に持つか: 掛け始めは «出来事が起きた瞬間» なので速く、戻しは
    //     «世界が動き出す» ので長く取りたい。1 つの秒数だと、速く掛けると速く戻って
    //     «止まって、急に走り出した» に見える (回避のスローが重く見えた原因)。
    void SlowFor(float scale, float seconds, float blendIn, float blendOut);

    /// 今どれだけ遅いか [0,1]。0 で等速、1 で完全停止。画面効果がこれを読む
    /// (Override = ヒットストップは含めない。あちらは Freeze が別に描く)。
    [[nodiscard]] float Slow01() const { return Clamp01(1.0f - (IsParryRush() ? std::min(m_blended, m_rushScale) : m_blended)); }

    // --- ポーズ (スローより優先、解除するまで維持) ---
    void SetPaused(bool paused) { m_paused = paused; }
    [[nodiscard]] bool IsPaused() const { return m_paused; }

    // --- 絶対上書き (最優先。ヒットストップが使う) ---
    // 有効な間は演出スローより優先する。ポーズは停止を維持する。解除で下の層へ戻る。
    void SetOverride(float scale);
    void ClearOverride() { m_hasOverride = false; }
    [[nodiscard]] bool HasOverride() const { return m_hasOverride; }

    [[nodiscard]] float CurrentScale() const { return debugScale; }

    void OnStart() override;
    void BeginParryRush(float seconds, float worldScale, float attackSpeed)
    {
        m_rushRemaining = m_rushDuration = std::max(seconds, 0.0f);
        m_rushScale = std::clamp(worldScale, 0.125f, 1.0f);
        m_rushAttackSpeed = std::clamp(attackSpeed, 1.0f, 3.0f);
    }
    void EnsureParryRushSeconds(float seconds)
    {
        if (!IsParryRush()) return;
        m_rushRemaining = std::max(m_rushRemaining, seconds);
        m_rushDuration = std::max(m_rushDuration, m_rushRemaining);
    }
    void EndParryRush() { m_rushRemaining = 0.0f; }
    [[nodiscard]] bool IsParryRush() const { return m_rushRemaining > 0.0f; }
    [[nodiscard]] float ParryRush01() const
    { return m_rushDuration > 0.0f ? Clamp01(m_rushRemaining / m_rushDuration) : 0.0f; }
    [[nodiscard]] static float PlayerTimeScale()
    {
        const auto* manager = Instance();
        if (manager && manager->IsPaused()) return 0.0f;
        if (!manager || !manager->IsParryRush() || manager->IsPaused()
            || manager->HasOverride() || Time::deltaTime <= 0.0f) return 1.0f;
        return std::clamp(Time::unscaledDeltaTime / Time::deltaTime, 1.0f, 8.0f);
    }
    [[nodiscard]] static float PlayerDeltaTime()
    {
        const auto* manager = Instance();
        if (manager && manager->IsPaused()) return 0.0f;
        return std::max(Time::deltaTime, 0.0f) * PlayerTimeScale();
    }
    [[nodiscard]] static float RushAttackSpeed()
    {
        const auto* manager = Instance();
        return manager && manager->IsParryRush() ? manager->m_rushAttackSpeed : 1.0f;
    }
    [[nodiscard]] static bool ParryRushActive()
    { return Instance() && Instance()->IsParryRush(); }
    void OnUpdate() override;
    void OnDestroy() override;

private:
    static inline TimeManagerComponent* s_instance = nullptr;

    [[nodiscard]] float ResolveBlend(float requested) const
    {
        return requested >= 0.0f ? requested : std::max(defaultBlendSeconds, 0.0f);
    }
    [[nodiscard]] float Sanitize(float scale) const
    {
        return Clamp(scale, std::max(minScale, 0.0f), 8.0f);
    }

    float m_slowTarget   = 1.0f;
    float m_rushRemaining = 0.0f;
    float m_rushDuration = 0.0f;
    float m_rushScale = 0.25f;
    float m_rushAttackSpeed = 1.65f;
    float m_blendSeconds = 0.0f;
    float m_blended      = 1.0f;   // 実際に適用している下位層の値
    float m_slowRemaining = 0.0f;  // SlowFor の残り (実時間)。0 以下で無期限
    bool  m_slowTimed    = false;
    float m_slowOutBlend = -1.0f;  // SlowFor が戻すときの秒数。負なら既定
    bool  m_paused       = false;
    bool  m_hasOverride  = false;
    float m_overrideScale = 1.0f;
};

FBZZ_REFLECT(TimeManagerComponent)

inline void TimeManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("TimeManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    m_rushRemaining = 0.0f;

    m_slowTarget    = 1.0f;
    m_blended       = 1.0f;
    m_slowRemaining = 0.0f;
    m_slowTimed     = false;
    m_paused        = false;
    m_hasOverride   = false;
    time.SetTimeScale(1.0f);
}

inline void TimeManagerComponent::OnDestroy()
{
    // シーン遷移がスローの最中に起きても、次のシーンへ遅い時間を持ち込まない。
    if (s_instance == this) {
        time.SetTimeScale(1.0f);
        s_instance = nullptr;
    }
}

inline void TimeManagerComponent::SetSlow(float scale, float blendSeconds)
{
    m_slowTarget    = Sanitize(scale);
    m_blendSeconds  = ResolveBlend(blendSeconds);
    m_slowTimed     = false;
    m_slowRemaining = 0.0f;
}

inline void TimeManagerComponent::ClearSlow(float blendSeconds)
{
    m_slowTarget    = 1.0f;
    m_blendSeconds  = ResolveBlend(blendSeconds);
    m_slowTimed     = false;
    m_slowRemaining = 0.0f;
}

inline void TimeManagerComponent::SlowFor(float scale, float seconds, float blendSeconds)
{
    SetSlow(scale, blendSeconds);
    m_slowTimed     = true;
    m_slowRemaining = std::max(seconds, 0.0f);
    m_slowOutBlend  = -1.0f;
}

inline void TimeManagerComponent::SlowFor(float scale, float seconds, float blendIn,
                                          float blendOut)
{
    SlowFor(scale, seconds, blendIn);
    m_slowOutBlend = std::max(blendOut, 0.0f);
}

inline void TimeManagerComponent::SetOverride(float scale)
{
    m_hasOverride   = true;
    m_overrideScale = Sanitize(scale);
}

inline void TimeManagerComponent::OnUpdate()
{
    // WHY 実時間で数えるか: スケールを下げている当人が縮んだ時間で自分の残りを数えると、
    //     スローが深いほど解除が遅れる。0 に近づけると永久に戻らない。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    if (m_slowTimed) {
        m_slowRemaining -= dt;
        if (m_slowRemaining <= 0.0f) ClearSlow(m_slowOutBlend);
    }

    if (!m_paused && !m_hasOverride)
        m_rushRemaining = std::max(m_rushRemaining - dt, 0.0f);
    const float target = m_paused ? 0.0f : m_slowTarget;
    if (m_blendSeconds <= 0.0f) {
        m_blended = target;
    } else {
        // 指数的に寄せる。フレームレートが変わっても寄り方が変わらない。
        const float response = 1.0f - std::exp(-dt / m_blendSeconds);
        m_blended += (target - m_blended) * response;
        if (std::abs(target - m_blended) < 0.001f) m_blended = target;
    }

    const float worldScale = IsParryRush() ? std::min(m_blended, m_rushScale) : m_blended;
    const float applied = m_paused ? 0.0f : m_hasOverride ? m_overrideScale : worldScale;
    time.SetTimeScale(applied);

    debugScale  = applied;
    debugSource = m_hasOverride ? "Override"
                : m_paused      ? "Paused"
                : IsParryRush() ? "Parry Rush"
                : (m_blended < 0.999f) ? "Slow"
                                       : "Normal";
}

} // namespace sandbox
