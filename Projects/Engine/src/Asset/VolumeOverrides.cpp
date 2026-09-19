/// @file    VolumeOverrides.cpp
/// @brief   組み込み VolumeOverride の Apply / Reflect 実装とレジストリ登録。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// 各 Apply は「対象の現在値と自分の値を weight で混ぜ、対応する enabled を立てる」だけ。
/// 混ぜ方の規則 (float は線形、bool / int は閾値) は BlendXxx に集約されている。
#include <Engine/Asset/VolumeOverrides.hpp>
#include <Engine/Renderer/PostProcessBlend.hpp>
#include <cstdio>

namespace fbzz::asset {

namespace {

using scene::IReflector;
using renderer::BlendBool;
using renderer::BlendColor3;
using renderer::BlendFloat;
using renderer::BlendInt;

/// @name Reflect の短縮ヘルパー
/// BeginField で永続キーと表示名を与えてから Field を呼ぶ、という定型をまとめる。

void Range(IReflector& r, const char* name, float& value, float min, float max)
{
    r.BeginField(name, name);
    r.FloatRange(name, value, min, max);
}

void Number(IReflector& r, const char* name, float& value)
{
    r.BeginField(name, name);
    r.Field(name, value);
}

/// 整数にもレンジを与える。Inspector 側 (ImGuiReflector) がスライダーになり、
/// 品質パラメーターの現実的な範囲がその場で分かる。
void Integer(IReflector& r, const char* name, int& value, int min, int max)
{
    r.BeginField(name, name);
    r.IntRange(name, value, min, max);
}

void Text(IReflector& r, const char* name, std::string& value)
{
    r.BeginField(name, name);
    r.Field(name, value);
}

/// float[3] を Vector3 経由で反映し、カラーピッカーのヒントを付ける。
/// @note IReflector は C 配列を直接扱えないため、一時 Vector3 へ写して書き戻す。
void Color3(IReflector& r, const char* name, float (&color)[3])
{
    math::Vector3 value{ color[0], color[1], color[2] };
    r.BeginField(name, name);
    r.SetFieldHint(IReflector::FieldHint::Color);
    r.Field(name, value);
    color[0] = value.x;
    color[1] = value.y;
    color[2] = value.z;
}

} // namespace

/// 露出

void ExposureOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    target.post.exposure = BlendFloat(target.post.exposure, exposure, weight);
}

void ExposureOverride::Reflect(IReflector& r)
{
    Range(r, "exposure", exposure, 0.0f, 8.0f);
}

/// アンチエイリアシング

void FxaaOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    target.post.fxaaEnabled = BlendBool(target.post.fxaaEnabled, true, weight);
}

void FxaaOverride::Reflect(IReflector&)
{
    /// @note パラメーターを持たない。ON/OFF は基底の active とリストへの有無で表す。
}

void TaaOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    target.taa.enabled  = BlendBool(target.taa.enabled, true, weight);
    target.taa.feedback = BlendFloat(target.taa.feedback, feedback, weight);
}

void TaaOverride::Reflect(IReflector& r)
{
    Range(r, "feedback", feedback, 0.0f, 1.0f);
}

/// アンビエントオクルージョン

void SsaoOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    target.post.ambientOcclusion.enabled   = BlendBool(target.post.ambientOcclusion.enabled, true, weight);
    target.post.ambientOcclusion.intensity = BlendFloat(target.post.ambientOcclusion.intensity, intensity, weight);
}

void SsaoOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 4.0f);
}

void GtaoOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    target.gtao.enabled       = BlendBool(target.gtao.enabled, true, weight);
    target.gtao.intensity     = BlendFloat(target.gtao.intensity, intensity, weight);
    target.gtao.radius        = BlendFloat(target.gtao.radius, radius, weight);
    target.gtao.slices        = BlendInt(target.gtao.slices, slices, weight);
    target.gtao.stepsPerSlice = BlendInt(target.gtao.stepsPerSlice, stepsPerSlice, weight);
}

void GtaoOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 4.0f);
    Range(r, "radius",    radius,    0.1f, 10.0f);
    Integer(r, "slices", slices, 1, 8);
    Integer(r, "stepsPerSlice", stepsPerSlice, 1, 16);
}

/// 色

void BloomOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.bloom;
    v.enabled   = BlendBool(v.enabled, true, weight);
    v.intensity = BlendFloat(v.intensity, intensity, weight);
    v.threshold = BlendFloat(v.threshold, threshold, weight);
    v.softKnee  = BlendFloat(v.softKnee,  softKnee,  weight);
}

void BloomOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 10.0f);
    Range(r, "threshold", threshold, 0.0f, 4.0f);
    Range(r, "softKnee",  softKnee,  0.0f, 1.0f);
}

void ColorGradingOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.colorGrading;
    v.enabled     = BlendBool(v.enabled, true, weight);
    v.contrast    = BlendFloat(v.contrast,    contrast,    weight);
    v.saturation  = BlendFloat(v.saturation,  saturation,  weight);
    v.hueShift    = BlendFloat(v.hueShift,    hueShift,    weight);
    v.temperature = BlendFloat(v.temperature, temperature, weight);
    v.tint        = BlendFloat(v.tint,        tint,        weight);
}

void ColorGradingOverride::Reflect(IReflector& r)
{
    Range(r, "contrast",    contrast,    -1.0f, 1.0f);
    Range(r, "saturation",  saturation,   0.0f, 3.0f);
    Range(r, "hueShift",    hueShift,  -180.0f, 180.0f);
    Range(r, "temperature", temperature, -1.0f, 1.0f);
    Range(r, "tint",        tint,        -1.0f, 1.0f);
}

void LutColorGradingOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.lutColorGrading;
    v.enabled     = BlendBool(v.enabled, true, weight);
    v.blend       = BlendFloat(v.blend,       blend,       weight);
    v.contrast    = BlendFloat(v.contrast,    contrast,    weight);
    v.saturation  = BlendFloat(v.saturation,  saturation,  weight);
    v.hueShift    = BlendFloat(v.hueShift,    hueShift,    weight);
    v.temperature = BlendFloat(v.temperature, temperature, weight);
    v.tint        = BlendFloat(v.tint,        tint,        weight);
}

void LutColorGradingOverride::Reflect(IReflector& r)
{
    Range(r, "blend",       blend,        0.0f, 1.0f);
    Range(r, "contrast",    contrast,    -1.0f, 2.0f);
    Range(r, "saturation",  saturation,   0.0f, 3.0f);
    Range(r, "hueShift",    hueShift,  -180.0f, 180.0f);
    Range(r, "temperature", temperature, -2.0f, 2.0f);
    Range(r, "tint",        tint,        -2.0f, 2.0f);
}

void ColorFilterOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.imageQuality;
    v.colorFilterEnabled   = BlendBool(v.colorFilterEnabled, true, weight);
    v.colorFilterIntensity = BlendFloat(v.colorFilterIntensity, intensity, weight);
    BlendColor3(v.colorFilter, color, weight, v.colorFilter);
}

void ColorFilterOverride::Reflect(IReflector& r)
{
    Color3(r, "color", color);
    Range(r, "intensity", intensity, 0.0f, 1.0f);
}

void ClarityOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.imageQuality;
    v.clarityEnabled  = BlendBool(v.clarityEnabled, true, weight);
    v.clarityStrength = BlendFloat(v.clarityStrength, strength, weight);
    v.clarityRadius   = BlendFloat(v.clarityRadius,   radius,   weight);
}

void ClarityOverride::Reflect(IReflector& r)
{
    Range(r, "strength", strength, 0.0f, 1.0f);
    Range(r, "radius",   radius,   0.5f, 8.0f);
}

void ShadowHighlightOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.imageQuality;
    v.shadowHighlightEnabled = BlendBool(v.shadowHighlightEnabled, true, weight);
    v.shadowLift           = BlendFloat(v.shadowLift,           shadowLift,           weight);
    v.highlightCompression = BlendFloat(v.highlightCompression, highlightCompression, weight);
}

void ShadowHighlightOverride::Reflect(IReflector& r)
{
    Range(r, "shadowLift",           shadowLift,          -1.0f, 1.0f);
    Range(r, "highlightCompression", highlightCompression, 0.0f, 1.0f);
}

void SharpenOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.sharpen;
    v.enabled  = BlendBool(v.enabled, true, weight);
    v.strength = BlendFloat(v.strength, strength, weight);
    v.radius   = BlendFloat(v.radius,   radius,   weight);
}

void SharpenOverride::Reflect(IReflector& r)
{
    Range(r, "strength", strength, 0.0f, 2.0f);
    Range(r, "radius",   radius,   0.25f, 4.0f);
}

void FilmGrainOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.filmGrain;
    v.enabled   = BlendBool(v.enabled, true, weight);
    v.intensity = BlendFloat(v.intensity, intensity, weight);
    v.response  = BlendFloat(v.response,  response,  weight);
}

void FilmGrainOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 0.5f);
    Range(r, "response",  response,  0.0f, 1.0f);
}

/// レンズ

void VignetteOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.vignette;
    v.enabled    = BlendBool(v.enabled, true, weight);
    v.intensity  = BlendFloat(v.intensity,  intensity,  weight);
    v.smoothness = BlendFloat(v.smoothness, smoothness, weight);
    v.roundness  = BlendFloat(v.roundness,  roundness,  weight);
    BlendColor3(v.color, color, weight, v.color);
}

void VignetteOverride::Reflect(IReflector& r)
{
    Range(r, "intensity",  intensity,  0.0f, 1.0f);
    Range(r, "smoothness", smoothness, 0.0f, 1.0f);
    Range(r, "roundness",  roundness,  0.0f, 1.0f);
    Color3(r, "color", color);
}

void DepthOfFieldOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.depthOfField;
    v.enabled       = BlendBool(v.enabled, true, weight);
    v.focusDistance = BlendFloat(v.focusDistance, focusDistance, weight);
    v.focusRange    = BlendFloat(v.focusRange,    focusRange,    weight);
    v.blurRadius    = BlendFloat(v.blurRadius,    blurRadius,    weight);
}

void DepthOfFieldOverride::Reflect(IReflector& r)
{
    Range(r, "focusDistance", focusDistance, 0.1f, 1000.0f);
    Range(r, "focusRange",    focusRange,    0.1f, 500.0f);
    Range(r, "blurRadius",    blurRadius,    0.0f, 16.0f);
}

void ChromaticAberrationOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.lens;
    v.chromaticAberrationEnabled = BlendBool(v.chromaticAberrationEnabled, true, weight);
    v.chromaticAberration        = BlendFloat(v.chromaticAberration, amount, weight);
}

void ChromaticAberrationOverride::Reflect(IReflector& r)
{
    Range(r, "amount", amount, 0.0f, 0.1f);
}

void LensDistortionOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.lens;
    v.distortionEnabled = BlendBool(v.distortionEnabled, true, weight);
    v.distortion        = BlendFloat(v.distortion, distortion, weight);
}

void LensDistortionOverride::Reflect(IReflector& r)
{
    Range(r, "distortion", distortion, -1.0f, 1.0f);
}

void LensFlareOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.lensFlare;
    v.enabled    = BlendBool(v.enabled, true, weight);
    v.intensity  = BlendFloat(v.intensity,  intensity,  weight);
    v.haloWidth  = BlendFloat(v.haloWidth,  haloWidth,  weight);
    v.distortion = BlendFloat(v.distortion, distortion, weight);
    v.ghostCount = BlendInt(v.ghostCount, ghostCount, weight);
}

void LensFlareOverride::Reflect(IReflector& r)
{
    Range(r, "intensity",  intensity,  0.0f, 4.0f);
    Range(r, "haloWidth",  haloWidth,  0.0f, 2.0f);
    Range(r, "distortion", distortion, 0.0f, 4.0f);
    Integer(r, "ghostCount", ghostCount, 1, 16);
}

void MotionBlurOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.motionBlur;
    v.enabled  = BlendBool(v.enabled, true, weight);
    v.strength = BlendFloat(v.strength, strength, weight);
    v.samples  = BlendInt(v.samples, samples, weight);
}

void MotionBlurOverride::Reflect(IReflector& r)
{
    Range(r, "strength", strength, 0.0f, 4.0f);
    Integer(r, "samples", samples, 2, 32);
}

/// 大気

void FogOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.fog;
    v.enabled     = BlendBool(v.enabled, true, weight);
    v.density     = BlendFloat(v.density,     density,     weight);
    v.farDistance = BlendFloat(v.farDistance, farDistance, weight);
    BlendColor3(v.color, color, weight, v.color);
    /// @note source は列挙値なので閾値で切り替える。
    v.source = BlendInt(v.source, source, weight);
}

void FogOverride::Reflect(IReflector& r)
{
    Range(r, "density",     density,     0.0f, 1.0f);
    Range(r, "farDistance", farDistance, 0.0f, 10000.0f);
    Color3(r, "color", color);
    Integer(r, "source", source, 0, 1);
}

void VolumetricLightOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.volumetricLight;
    v.enabled       = BlendBool(v.enabled, true, weight);
    v.scattering    = BlendFloat(v.scattering,    scattering,    weight);
    v.intensity     = BlendFloat(v.intensity,     intensity,     weight);
    v.minDist       = BlendFloat(v.minDist,       minDistance,   weight);
    v.maxDist       = BlendFloat(v.maxDist,       maxDistance,   weight);
    v.edgeFade      = BlendFloat(v.edgeFade,      edgeFade,      weight);
    v.density       = BlendFloat(v.density,       density,       weight);
    v.heightFalloff = BlendFloat(v.heightFalloff, heightFalloff, weight);
    v.heightStart   = BlendFloat(v.heightStart,   heightStart,   weight);
    BlendColor3(v.tint, tint, weight, v.tint);
    v.steps         = BlendInt(v.steps, steps, weight);
}

void VolumetricLightOverride::Reflect(IReflector& r)
{
    Range(r, "scattering",  scattering,  0.0f, 1.0f);
    Range(r, "intensity",   intensity,   0.0f, 8.0f);
    Color3(r, "tint", tint);
    Range(r, "minDistance", minDistance, 0.0f, 500.0f);
    /// @note 屋外の光芒は雲の切れ間から地面まで伸びるため、200 では手前の空気しか積分できない。
    Range(r, "maxDistance", maxDistance, 1.0f, 4000.0f);
    Range(r, "edgeFade",    edgeFade,    0.0f, 1.0f);
    Range(r, "density",     density,     0.0f, 0.2f);
    Range(r, "heightFalloff", heightFalloff, 0.0f, 0.5f);
    Range(r, "heightStart",   heightStart, -500.0f, 500.0f);
    Integer(r, "steps", steps, 4, 128);
}

void FroxelFogOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& f = target.froxelFog;
    f.enabled       = BlendBool(f.enabled, true, weight);
    f.density       = BlendFloat(f.density,       density,       weight);
    f.anisotropy    = BlendFloat(f.anisotropy,    anisotropy,    weight);
    f.heightFalloff = BlendFloat(f.heightFalloff, heightFalloff, weight);
    f.heightStart   = BlendFloat(f.heightStart,   heightStart,   weight);
    f.nearDistance  = BlendFloat(f.nearDistance,  nearDistance,  weight);
    f.farDistance   = BlendFloat(f.farDistance,   farDistance,   weight);
    f.ambient       = BlendFloat(f.ambient,       ambient,       weight);
    BlendColor3(f.albedo,   albedo,   weight, f.albedo);
    BlendColor3(f.emissive, emissive, weight, f.emissive);
    /// @note グリッド寸法はブレンドしない (再確保が走るため)。RenderSettings の既定を使う。
}

void FroxelFogOverride::Reflect(IReflector& r)
{
    Range(r, "density",       density,       0.0f, 0.5f);
    Color3(r, "albedo",   albedo);
    Color3(r, "emissive", emissive);
    Range(r, "anisotropy",    anisotropy,   -0.95f, 0.95f);
    Range(r, "heightFalloff", heightFalloff, 0.0f, 0.5f);
    Range(r, "heightStart",   heightStart,  -500.0f, 500.0f);
    Range(r, "nearDistance",  nearDistance,  0.01f, 10.0f);
    Range(r, "farDistance",   farDistance,   1.0f, 500.0f);
    Range(r, "ambient",       ambient,       0.0f, 4.0f);
}

void AutoExposureOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& a = target.autoExposure;
    a.enabled       = BlendBool(a.enabled, true, weight);
    a.minEV         = BlendFloat(a.minEV,         minEV,         weight);
    a.maxEV         = BlendFloat(a.maxEV,         maxEV,         weight);
    a.lowPercent    = BlendFloat(a.lowPercent,    lowPercent,    weight);
    a.highPercent   = BlendFloat(a.highPercent,   highPercent,   weight);
    a.speedUp       = BlendFloat(a.speedUp,       speedUp,       weight);
    a.speedDown     = BlendFloat(a.speedDown,     speedDown,     weight);
    a.compensation  = BlendFloat(a.compensation,  compensation,  weight);
    a.minExposureEV = BlendFloat(a.minExposureEV, minExposureEV, weight);
    a.maxExposureEV = BlendFloat(a.maxExposureEV, maxExposureEV, weight);
}

void AutoExposureOverride::Reflect(IReflector& r)
{
    Range(r, "minEV",         minEV,        -16.0f, 0.0f);
    Range(r, "maxEV",         maxEV,          0.0f, 20.0f);
    Range(r, "lowPercent",    lowPercent,     0.0f, 0.95f);
    Range(r, "highPercent",   highPercent,    0.05f, 1.0f);
    Range(r, "speedUp",       speedUp,        0.0f, 20.0f);
    Range(r, "speedDown",     speedDown,      0.0f, 20.0f);
    Range(r, "compensation",  compensation,  -5.0f, 5.0f);
    Range(r, "minExposureEV", minExposureEV, -16.0f, 0.0f);
    Range(r, "maxExposureEV", maxExposureEV,   0.0f, 16.0f);
}

/// 影・反射

void SsrOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.ssr;
    v.enabled     = BlendBool(v.enabled, true, weight);
    v.maxDistance = BlendFloat(v.maxDistance, maxDistance, weight);
    v.thickness   = BlendFloat(v.thickness,   thickness,   weight);
    v.intensity   = BlendFloat(v.intensity,   intensity,   weight);
    v.steps       = BlendInt(v.steps, steps, weight);
}

void SsrOverride::Reflect(IReflector& r)
{
    Range(r, "maxDistance", maxDistance, 1.0f, 200.0f);
    Range(r, "thickness",   thickness,   0.01f, 2.0f);
    Range(r, "intensity",   intensity,   0.0f, 1.0f);
    Integer(r, "steps", steps, 8, 128);
}

void ContactShadowOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.contactShadow;
    v.enabled   = BlendBool(v.enabled, true, weight);
    v.strength  = BlendFloat(v.strength,  strength,  weight);
    v.rayLength = BlendFloat(v.rayLength, rayLength, weight);
    v.thickness = BlendFloat(v.thickness, thickness, weight);
    v.steps     = BlendInt(v.steps, steps, weight);
}

void ContactShadowOverride::Reflect(IReflector& r)
{
    Range(r, "strength",  strength,  0.0f, 1.0f);
    Range(r, "rayLength", rayLength, 0.1f, 20.0f);
    Range(r, "thickness", thickness, 0.01f, 2.0f);
    Integer(r, "steps", steps, 4, 64);
}

/// 演出

void SepiaOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.stylized;
    v.sepiaEnabled   = BlendBool(v.sepiaEnabled, true, weight);
    v.sepiaIntensity = BlendFloat(v.sepiaIntensity, intensity, weight);
}

void SepiaOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 1.0f);
}

void InvertOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.stylized;
    v.invertEnabled   = BlendBool(v.invertEnabled, true, weight);
    v.invertIntensity = BlendFloat(v.invertIntensity, intensity, weight);
}

void InvertOverride::Reflect(IReflector& r)
{
    Range(r, "intensity", intensity, 0.0f, 1.0f);
}

void PosterizeOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.stylized;
    v.posterizeEnabled = BlendBool(v.posterizeEnabled, true, weight);
    v.posterizeLevels  = BlendFloat(v.posterizeLevels, levels, weight);
}

void PosterizeOverride::Reflect(IReflector& r)
{
    Range(r, "levels", levels, 2.0f, 64.0f);
}

void PixelateOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    auto& v = target.post.stylized;
    v.pixelateEnabled = BlendBool(v.pixelateEnabled, true, weight);
    v.pixelSize       = BlendFloat(v.pixelSize, pixelSize, weight);
}

void PixelateOverride::Reflect(IReflector& r)
{
    Range(r, "pixelSize", pixelSize, 1.0f, 64.0f);
}

/// カスタム

void CustomEffectOverride::Apply(renderer::VolumeSettings& target, float weight) const
{
    /// @note 配列要素の「半分だけ存在する」状態を定義できないため、補間せず閾値で足す。
    ///       重みが半分を超えたときにパスとして追加し、パラメーターはオーバーライドの
    ///       値をそのまま使う。
    if (weight < 0.5f) return;

    renderer::CustomPostProcessSettings applied = effect;
    applied.enabled = true;
    target.post.customEffects.push_back(std::move(applied));
}

void CustomEffectOverride::Reflect(IReflector& r)
{
    Text(r, "name", effect.name);
    r.BeginField("shaderPath", "shaderPath");
    r.SetFileExtensions(".hlsl");
    r.Field("shaderPath", effect.shaderPath);
    /// @note .mat を指すと、シェーダー・テクスチャ・名前付きパラメーターがそちらから来る。
    r.BeginField("materialPath", "materialPath");
    r.SetFileExtensions(".mat");
    r.Field("materialPath", effect.materialPath);
    Range(r, "intensity", effect.intensity, 0.0f, 4.0f);
    Range(r, "blend",     effect.blend,     0.0f, 1.0f);

    /// @note 列挙は int で出す (FogOverride::source と同じ扱い)。
    ///       stage     0 = SceneHDR (Composite 前) / 1 = PostProcess (後) / 2 = AfterOpaque (半透明の前)
    ///       blendMode 0 = 置き換え / 1 = アルファ / 2 = 加算 / 3 = 事前乗算 (HDR の段のみ有効)
    int stage = static_cast<int>(effect.stage);
    Integer(r, "stage", stage, 0, 2);
    effect.stage = static_cast<renderer::CustomPassStage>(stage);

    int blendMode = static_cast<int>(effect.blendMode);
    Integer(r, "blendMode", blendMode, 0, 3);
    effect.blendMode = static_cast<renderer::BlendMode>(blendMode);

    /// @note 追加入力のビット (CustomPassInput)。
    ///       1 深度 / 2 オブジェクトマスク / 4 速度 / 8 法線 / 16 ブルーム / 32 AO
    int inputs = static_cast<int>(effect.inputs);
    Integer(r, "inputs", inputs, 0, 63);
    effect.inputs = static_cast<uint32_t>(inputs);

    Integer(r, "iterations", effect.iterations, 1, 8);
    Integer(r, "downscale",  effect.downscale,  1, 8);

    for (int i = 0; i < 8; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "param%d", i);
        Number(r, name, effect.parameters[i]);
    }
}

} // namespace fbzz::asset

/// @name レジストリ登録
/// @note 登録漏れを 1 か所で目視確認できるようにするため、ファイル末尾にまとめる。
///       クラス定義の直後に散らすと、追加時に「宣言はしたが登録し忘れた」が起きやすい。
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ExposureOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::FxaaOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::TaaOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::SsaoOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::GtaoOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::BloomOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ColorGradingOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::LutColorGradingOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ColorFilterOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ClarityOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ShadowHighlightOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::SharpenOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::FilmGrainOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::VignetteOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::DepthOfFieldOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ChromaticAberrationOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::LensDistortionOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::LensFlareOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::MotionBlurOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::FogOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::VolumetricLightOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::FroxelFogOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::AutoExposureOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::SsrOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::ContactShadowOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::SepiaOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::InvertOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::PosterizeOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::PixelateOverride)
FBZZ_REGISTER_VOLUME_OVERRIDE(::fbzz::asset::CustomEffectOverride)
