/// @file ScreenEffectManagerComponent.hpp
/// @brief 画面効果 (フェード・フラッシュ・ビネット・色収差) の唯一の書き手
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 1 箇所に集めるか:
///   ランタイムの PostProcessSettings は「まるごと差し替える」器で、TimeManager が扱う
///   Time::timeScale と同じ性質を持つ。書き手が 2 つになると、後から書いた側が
///   前の効果を消す。実際 SceneManagerScript はフェードのために TryGet() で読んで
///   書き戻しており、そこへ被弾フラッシュが割り込めば、どちらかが必ず消える。
///
/// WHY 毎フレーム読み戻さないか:
///   自分が書いた結果を次のフレームの基準として読むと、効果が積み重なって発散する。
///   効果が 1 つも無い状態の設定を基準として保持し、常にそこから作り直す。
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ScreenEffectManagerComponent : public Script {
    FBZZ_SCRIPT(ScreenEffectManagerComponent)

public:
    FBZZ_GROUP("Flash")
    FBZZ_FIELD_COLOR(defaultFlashColor, (Vector4{ 1.0f, 0.25f, 0.20f, 1.0f }), "Flash Color")
    FBZZ_FIELD_RANGE(float, defaultFlashStrength, 0.45f, "Flash Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で画面を覆う濃さ。1.0 にすると一瞬完全に染まる")
    FBZZ_FIELD_RANGE(float, defaultFlashSeconds, 0.22f, "Flash Seconds", 0.0f, 2.0f)

    FBZZ_GROUP("Vignette")
    FBZZ_FIELD_RANGE(float, vignetteIntensity, 0.45f, "Intensity", 0.0f, 1.0f)
    FBZZ_TOOLTIP("スローや被弾中に足すビネットの濃さ")
    FBZZ_FIELD_RANGE(float, vignetteSmoothness, 0.5f, "Smoothness", 0.0f, 1.0f)

    FBZZ_GROUP("Chromatic Aberration")
    FBZZ_FIELD_RANGE(float, aberrationAmount, 0.008f, "Amount", 0.0f, 0.05f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で入れる色収差の量")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugFade, 0.0f, "Fade")
    FBZZ_FIELD_READ_ONLY(float, debugFlash, 0.0f, "Flash")
    FBZZ_FIELD_READ_ONLY(float, debugDistortion, 0.0f, "Distortion")

    [[nodiscard]] static ScreenEffectManagerComponent* Instance() { return s_instance; }

    // --- フェード (シーン遷移が持ち続ける値。自動では減らない) ---
    void SetFade(float alpha, const Vector4& color = { 0.0f, 0.0f, 0.0f, 1.0f });
    void ClearFade() { m_fadeAlpha = 0.0f; }
    [[nodiscard]] float FadeAlpha() const { return m_fadeAlpha; }

    // --- フラッシュ (時間で自動的に消える) ---
    void Flash(float strength01);
    void Flash(const Vector4& color, float strength01, float seconds);

    // --- 歪み: ビネット + 色収差 (時間で自動的に消える) ---
    void Distort(float strength01, float seconds);
    // 解除するまで維持する版。スロー中に掛けっぱなしにする用途。
    void SetSustainedDistortion(float strength01) { m_sustainedDistortion = Clamp01(strength01); }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    static inline ScreenEffectManagerComponent* s_instance = nullptr;

    // 効果が 1 つも無い状態の設定。ここから毎フレーム作り直す。
    void CaptureBase();

    fbzz::renderer::PostProcessSettings m_base{};
    bool    m_hasBase = false;
    bool    m_writing = false;

    float   m_fadeAlpha = 0.0f;
    Vector4 m_fadeColor{ 0.0f, 0.0f, 0.0f, 1.0f };

    Vector4 m_flashColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float   m_flashStrength = 0.0f;
    float   m_flashSeconds  = 0.0f;
    float   m_flashRemaining = 0.0f;

    float   m_distortStrength  = 0.0f;
    float   m_distortSeconds   = 0.0f;
    float   m_distortRemaining = 0.0f;
    float   m_sustainedDistortion = 0.0f;
};

FBZZ_REFLECT(ScreenEffectManagerComponent)

inline void ScreenEffectManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("ScreenEffectManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_hasBase = false;
    m_writing = false;
    m_fadeAlpha = 0.0f;
    m_flashRemaining = 0.0f;
    m_distortRemaining = 0.0f;
    m_sustainedDistortion = 0.0f;
}

inline void ScreenEffectManagerComponent::OnDestroy()
{
    // 効果を掛けたままシーンを抜けると、次のシーンが暗転や赤染めのまま始まる。
    if (m_writing) postprocess.Clear();
    if (s_instance == this) s_instance = nullptr;
}

inline void ScreenEffectManagerComponent::CaptureBase()
{
    if (m_hasBase) return;
    // 何も書いていない今の設定を基準にする。効果が終わればここへ戻る。
    m_base = postprocess.TryGet() ? *postprocess.TryGet() : fbzz::renderer::PostProcessSettings{};
    m_hasBase = true;
}

inline void ScreenEffectManagerComponent::SetFade(float alpha, const Vector4& color)
{
    CaptureBase();
    m_fadeAlpha = Clamp01(alpha);
    m_fadeColor = color;
}

inline void ScreenEffectManagerComponent::Flash(float strength01)
{
    Flash(defaultFlashColor, strength01 * defaultFlashStrength, defaultFlashSeconds);
}

inline void ScreenEffectManagerComponent::Flash(const Vector4& color, float strength01, float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    // 強い方を採る。足すと連続被弾で画面が真っ赤のまま戻らなくなる。
    if (strength >= m_flashStrength || m_flashRemaining <= 0.0f) {
        m_flashColor    = color;
        m_flashStrength = strength;
        m_flashSeconds  = seconds;
    }
    m_flashRemaining = std::max(m_flashRemaining, seconds);
}

inline void ScreenEffectManagerComponent::Distort(float strength01, float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    if (strength >= m_distortStrength || m_distortRemaining <= 0.0f) {
        m_distortStrength = strength;
        m_distortSeconds  = seconds;
    }
    m_distortRemaining = std::max(m_distortRemaining, seconds);
}

inline void ScreenEffectManagerComponent::OnLateUpdate()
{
    // WHY 実時間か: 画面効果はヒットストップ中こそ見せたい。止めると被弾の赤が
    //     停止解除まで出ないので、当たった瞬間の情報が遅れて届く。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    float flash = 0.0f;
    if (m_flashRemaining > 0.0f && m_flashSeconds > EPSILON) {
        m_flashRemaining = std::max(0.0f, m_flashRemaining - dt);
        flash = m_flashStrength * Clamp01(m_flashRemaining / m_flashSeconds);
        if (m_flashRemaining <= 0.0f) m_flashStrength = 0.0f;
    }

    float distortion = m_sustainedDistortion;
    if (m_distortRemaining > 0.0f && m_distortSeconds > EPSILON) {
        m_distortRemaining = std::max(0.0f, m_distortRemaining - dt);
        distortion = std::max(distortion,
                              m_distortStrength * Clamp01(m_distortRemaining / m_distortSeconds));
        if (m_distortRemaining <= 0.0f) m_distortStrength = 0.0f;
    }

    debugFade       = m_fadeAlpha;
    debugFlash      = flash;
    debugDistortion = distortion;

    const bool active = m_fadeAlpha > 0.0f || flash > 0.0f || distortion > 0.0f;
    if (!active) {
        // 何も掛かっていない間はランタイム上書きを外す。載せっぱなしにすると
        // シーンの PostProcessVolume が効かなくなる。
        if (m_writing) {
            postprocess.Clear();
            m_writing = false;
            m_hasBase = false;
        }
        return;
    }

    CaptureBase();
    fbzz::renderer::PostProcessSettings pp = m_base;

    if (distortion > 0.0f) {
        pp.vignette.enabled   = true;
        pp.vignette.intensity = std::max(pp.vignette.intensity,
                                         vignetteIntensity * distortion);
        pp.vignette.smoothness = vignetteSmoothness;
        pp.lens.chromaticAberrationEnabled = true;
        pp.lens.chromaticAberration = std::max(pp.lens.chromaticAberration,
                                               aberrationAmount * distortion);
    }

    // フラッシュとフェードは同じ画面塗りの器を共有する。両方出ているときは、
    // 濃い方を採ったうえで色を混ぜる。遷移中の暗転がフラッシュで薄まると事故に見える。
    float  paintAlpha = m_fadeAlpha;
    Vector4 paintColor = m_fadeColor;
    if (flash > m_fadeAlpha) {
        paintAlpha = flash;
        paintColor = m_flashColor;
    } else if (flash > 0.0f && m_fadeAlpha > 0.0f) {
        const float t = flash / std::max(m_fadeAlpha, EPSILON);
        paintColor = m_fadeColor + (m_flashColor - m_fadeColor) * Clamp01(t) * 0.5f;
    }

    pp.screenFadeAlpha    = paintAlpha;
    pp.screenFadeColor[0] = paintColor.x;
    pp.screenFadeColor[1] = paintColor.y;
    pp.screenFadeColor[2] = paintColor.z;

    postprocess.Set(pp);
    m_writing = true;
}

} // namespace sandbox
