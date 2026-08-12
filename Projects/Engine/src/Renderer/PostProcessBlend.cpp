// FBZZ Engine
// PostProcessBlend.cpp | fbzz::renderer
// PostProcessSettings の重み付き合成の実装
#include <Engine/Renderer/PostProcessBlend.hpp>

#include <algorithm>

namespace fbzz::renderer {

namespace {

// 端点で厳密に元の値を返す線形補間。
// WHY 端点を特別扱いするか: a + (b - a) * 1.0f は (b - a) の丸め誤差により
//     b と厳密には一致しない。「ボリュームの中に完全に入っているのに設定が
//     微妙に違う」という追跡困難な差異を防ぐため、端点を明示的に返す。
//     これにより関数全体での早期 return が不要になり、
//     override マスクとの併用 (t=1 でも非対象セクションは a を保つ) が自然に成立する。
float Lerp(float a, float b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

// bool は連続量ではないため、重みが半分を超えた側を採用する。
bool PickBool(bool a, bool b, float t) { return t >= 0.5f ? b : a; }

void LerpColor3(const float (&a)[3], const float (&b)[3], float t, float (&out)[3])
{
    for (int i = 0; i < 3; ++i) out[i] = Lerp(a[i], b[i], t);
}

} // namespace

PostProcessSettings LerpPostProcessSettings(
    const PostProcessSettings& a, const PostProcessSettings& b, float t)
{
    return LerpPostProcessSettings(a, b, t, PostProcessOverrides::All());
}

PostProcessSettings LerpPostProcessSettings(
    const PostProcessSettings& a, const PostProcessSettings& b, float t,
    const PostProcessOverrides& overrides)
{
    if (t <= 0.0f) return a;

    // out は a の複製から始める。上書き対象でないセクションは一切触らないため、
    // 自動的に a の値がそのまま残る。
    PostProcessSettings out = a;

    if (overrides.fxaa)     out.fxaaEnabled = PickBool(a.fxaaEnabled, b.fxaaEnabled, t);
    if (overrides.exposure) out.exposure    = Lerp(a.exposure, b.exposure, t);

    if (overrides.bloom) {
        out.bloom.enabled   = PickBool(a.bloom.enabled, b.bloom.enabled, t);
        out.bloom.intensity = Lerp(a.bloom.intensity, b.bloom.intensity, t);
        out.bloom.threshold = Lerp(a.bloom.threshold, b.bloom.threshold, t);
        out.bloom.softKnee  = Lerp(a.bloom.softKnee,  b.bloom.softKnee,  t);
    }

    if (overrides.ambientOcclusion) {
        out.ambientOcclusion.enabled   = PickBool(a.ambientOcclusion.enabled, b.ambientOcclusion.enabled, t);
        out.ambientOcclusion.intensity = Lerp(a.ambientOcclusion.intensity, b.ambientOcclusion.intensity, t);
    }

    if (overrides.fog) {
        out.fog.enabled     = PickBool(a.fog.enabled, b.fog.enabled, t);
        out.fog.density     = Lerp(a.fog.density,     b.fog.density,     t);
        out.fog.farDistance = Lerp(a.fog.farDistance, b.fog.farDistance, t);
        LerpColor3(a.fog.color, b.fog.color, t, out.fog.color);
        // source は列挙値。連続量ではないので bool と同じく閾値で切り替える。
        out.fog.source = t >= 0.5f ? b.fog.source : a.fog.source;
    }

    if (overrides.colorGrading) {
        out.colorGrading.enabled     = PickBool(a.colorGrading.enabled, b.colorGrading.enabled, t);
        out.colorGrading.contrast    = Lerp(a.colorGrading.contrast,    b.colorGrading.contrast,    t);
        out.colorGrading.saturation  = Lerp(a.colorGrading.saturation,  b.colorGrading.saturation,  t);
        out.colorGrading.hueShift    = Lerp(a.colorGrading.hueShift,    b.colorGrading.hueShift,    t);
        out.colorGrading.temperature = Lerp(a.colorGrading.temperature, b.colorGrading.temperature, t);
        out.colorGrading.tint        = Lerp(a.colorGrading.tint,        b.colorGrading.tint,        t);
    }

    if (overrides.vignette) {
        out.vignette.enabled    = PickBool(a.vignette.enabled, b.vignette.enabled, t);
        out.vignette.intensity  = Lerp(a.vignette.intensity,  b.vignette.intensity,  t);
        out.vignette.smoothness = Lerp(a.vignette.smoothness, b.vignette.smoothness, t);
        out.vignette.roundness  = Lerp(a.vignette.roundness,  b.vignette.roundness,  t);
        LerpColor3(a.vignette.color, b.vignette.color, t, out.vignette.color);
    }

    if (overrides.filmGrain) {
        out.filmGrain.enabled   = PickBool(a.filmGrain.enabled, b.filmGrain.enabled, t);
        out.filmGrain.intensity = Lerp(a.filmGrain.intensity, b.filmGrain.intensity, t);
        out.filmGrain.response  = Lerp(a.filmGrain.response,  b.filmGrain.response,  t);
    }

    if (overrides.sharpen) {
        out.sharpen.enabled  = PickBool(a.sharpen.enabled, b.sharpen.enabled, t);
        out.sharpen.strength = Lerp(a.sharpen.strength, b.sharpen.strength, t);
        out.sharpen.radius   = Lerp(a.sharpen.radius,   b.sharpen.radius,   t);
    }

    if (overrides.depthOfField) {
        out.depthOfField.enabled       = PickBool(a.depthOfField.enabled, b.depthOfField.enabled, t);
        out.depthOfField.focusDistance = Lerp(a.depthOfField.focusDistance, b.depthOfField.focusDistance, t);
        out.depthOfField.focusRange    = Lerp(a.depthOfField.focusRange,    b.depthOfField.focusRange,    t);
        out.depthOfField.blurRadius    = Lerp(a.depthOfField.blurRadius,    b.depthOfField.blurRadius,    t);
    }

    if (overrides.lens) {
        out.lens.chromaticAberrationEnabled =
            PickBool(a.lens.chromaticAberrationEnabled, b.lens.chromaticAberrationEnabled, t);
        out.lens.distortionEnabled   = PickBool(a.lens.distortionEnabled, b.lens.distortionEnabled, t);
        out.lens.chromaticAberration = Lerp(a.lens.chromaticAberration, b.lens.chromaticAberration, t);
        out.lens.distortion          = Lerp(a.lens.distortion,          b.lens.distortion,          t);
    }

    if (overrides.stylized) {
        out.stylized.sepiaEnabled     = PickBool(a.stylized.sepiaEnabled,     b.stylized.sepiaEnabled,     t);
        out.stylized.invertEnabled    = PickBool(a.stylized.invertEnabled,    b.stylized.invertEnabled,    t);
        out.stylized.posterizeEnabled = PickBool(a.stylized.posterizeEnabled, b.stylized.posterizeEnabled, t);
        out.stylized.pixelateEnabled  = PickBool(a.stylized.pixelateEnabled,  b.stylized.pixelateEnabled,  t);
        out.stylized.sepiaIntensity   = Lerp(a.stylized.sepiaIntensity,   b.stylized.sepiaIntensity,   t);
        out.stylized.invertIntensity  = Lerp(a.stylized.invertIntensity,  b.stylized.invertIntensity,  t);
        out.stylized.posterizeLevels  = Lerp(a.stylized.posterizeLevels,  b.stylized.posterizeLevels,  t);
        out.stylized.pixelSize        = Lerp(a.stylized.pixelSize,        b.stylized.pixelSize,        t);
    }

    if (overrides.imageQuality) {
        out.imageQuality.clarityEnabled = PickBool(a.imageQuality.clarityEnabled, b.imageQuality.clarityEnabled, t);
        out.imageQuality.shadowHighlightEnabled =
            PickBool(a.imageQuality.shadowHighlightEnabled, b.imageQuality.shadowHighlightEnabled, t);
        out.imageQuality.colorFilterEnabled =
            PickBool(a.imageQuality.colorFilterEnabled, b.imageQuality.colorFilterEnabled, t);
        out.imageQuality.clarityStrength      = Lerp(a.imageQuality.clarityStrength,      b.imageQuality.clarityStrength,      t);
        out.imageQuality.clarityRadius        = Lerp(a.imageQuality.clarityRadius,        b.imageQuality.clarityRadius,        t);
        out.imageQuality.shadowLift           = Lerp(a.imageQuality.shadowLift,           b.imageQuality.shadowLift,           t);
        out.imageQuality.highlightCompression = Lerp(a.imageQuality.highlightCompression, b.imageQuality.highlightCompression, t);
        out.imageQuality.colorFilterIntensity = Lerp(a.imageQuality.colorFilterIntensity, b.imageQuality.colorFilterIntensity, t);
        LerpColor3(a.imageQuality.colorFilter, b.imageQuality.colorFilter, t, out.imageQuality.colorFilter);
    }

    // 配列は補間せず、重みが大きい側を丸ごと採用する。
    // WHY 補間しないか: 要素数も名前も異なりうる配列を補間する自然な定義が存在しない。
    if (overrides.customEffects)
        out.customEffects = t >= 0.5f ? b.customEffects : a.customEffects;

    // 画面フェードはボリュームの領分ではなくランタイム演出。
    // WHY 常にベース側を保つか: スクリプトがフェード中にボリュームをまたぐと、
    //      演出の進行がボリューム配置に引きずられて破綻する。
    //      override マスクにも含めない (ボリュームで制御する対象ではない)。
    out.screenFadeAlpha = a.screenFadeAlpha;
    out.screenFadeColor[0] = a.screenFadeColor[0];
    out.screenFadeColor[1] = a.screenFadeColor[1];
    out.screenFadeColor[2] = a.screenFadeColor[2];

    return out;
}

float PostProcessVolumeDistanceWeight(float distance, float radius, float blendDistance)
{
    if (radius <= 0.0f) return 0.0f;
    if (distance >= radius) return 0.0f;

    // blendDistance が半径以上だと内側の「完全適用域」が消える。
    // その場合は中心で 1、境界で 0 の単純な線形降下にする。
    const float fade = std::clamp(blendDistance, 0.0f, radius);
    const float solidRadius = radius - fade;
    if (distance <= solidRadius) return 1.0f;
    if (fade <= 0.0f) return 1.0f;

    return std::clamp((radius - distance) / fade, 0.0f, 1.0f);
}

} // namespace fbzz::renderer
