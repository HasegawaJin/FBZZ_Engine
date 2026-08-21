// FBZZ Engine
// VolumeOverrides.hpp | fbzz::asset
// 組み込みの VolumeOverride 派生一覧。
//
// 効果 1 つ = クラス 1 つ。各クラスはその効果のパラメーターだけを持ち、
// 対応する XxxSettings::enabled は Apply 側で立てる。
// WHY enabled をフィールドに持たないか:
//   「リストに載っていること」自体が使用の意思表示で、一時的に外したいときは
//   基底の active を落とす。両方あると「enabled=false の Bloom オーバーライド」
//   という、存在するのに効かない読みにくい状態が作れてしまう。
//
// 新しい効果を足す手順はこのファイル内で完結する:
//   1. ここにクラスを 1 つ足す
//   2. VolumeOverrides.cpp に Apply / Reflect を書き、FBZZ_REGISTER_VOLUME_OVERRIDE
//   Inspector の Add Override メニューと .fzdata の読み書きは自動的に追随する。
#pragma once
#include <Engine/Asset/VolumeOverride.hpp>
#include <Engine/Renderer/RenderSettings.hpp>

namespace fbzz::asset {

// 定型の override 宣言。TYPE_NAME / 表示名 / カテゴリ / Clone を一括で与える。
// WHY マクロにするか: 4 つの仮想関数はどのクラスでも中身が同じ形で、
//     手書きすると「Clone のコピー先クラス名を直し忘れる」類の事故が起きる。
#define FBZZ_VOLUME_OVERRIDE_BODY(Class, TypeNameLiteral, DisplayLiteral, CategoryEnum) \
public:                                                                                \
    static constexpr const char* TYPE_NAME = TypeNameLiteral;                          \
    const char* GetTypeName() const override { return TYPE_NAME; }                     \
    const char* GetDisplayName() const override { return DisplayLiteral; }             \
    VolumeOverrideCategory GetCategory() const override                                \
    { return VolumeOverrideCategory::CategoryEnum; }                                   \
    std::unique_ptr<VolumeOverride> Clone() const override                             \
    { return std::make_unique<Class>(*this); }                                         \
    void Apply(renderer::VolumeSettings& target, float weight) const override;         \
    void Reflect(scene::IReflector& r) override;

// ── 露出 ────────────────────────────────────────────────────────────────
class ExposureOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ExposureOverride, "Exposure", "Exposure", Exposure)
    float exposure = 1.0f;
};

// ── アンチエイリアシング (FXAA / TAA は同一スロットで排他) ────────────────
class FxaaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FxaaOverride, "FXAA", "FXAA", AntiAliasing)
};

class TaaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(TaaOverride, "TAA", "TAA (Temporal)", AntiAliasing)
    float feedback = 0.9f;
};

// ── アンビエントオクルージョン (SSAO / GTAO は同一スロットで排他) ──────────
class SsaoOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SsaoOverride, "SSAO", "SSAO (Simple)", AmbientOcclusion)
    float intensity = 1.0f;
};

class GtaoOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(GtaoOverride, "GTAO", "GTAO (Ground Truth)", AmbientOcclusion)
    float intensity     = 1.0f;
    float radius        = 1.5f;
    int   slices        = 8;
    int   stepsPerSlice = 8;
};

// ── 色 ──────────────────────────────────────────────────────────────────
class BloomOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(BloomOverride, "Bloom", "Bloom", Color)
    float intensity = 0.9f;
    float threshold = 0.65f;
    float softKnee  = 0.4f;
};

class ColorGradingOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ColorGradingOverride, "ColorGrading", "Color Grading", Color)
    float contrast    = 0.0f;
    float saturation  = 1.0f;
    float hueShift    = 0.0f;
    float temperature = 0.0f;
    float tint        = 0.0f;
};

class LutColorGradingOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LutColorGradingOverride, "LUTColorGrading",
                              "LUT Color Grading (Procedural)", Color)
    float blend       = 1.0f;
    float contrast    = 0.0f;
    float saturation  = 1.0f;
    float hueShift    = 0.0f;
    float temperature = 0.0f;
    float tint        = 0.0f;
};

class ColorFilterOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ColorFilterOverride, "ColorFilter", "Color Filter", Color)
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 0.5f;
};

class ClarityOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ClarityOverride, "Clarity", "Clarity", Color)
    float strength = 0.12f;
    float radius   = 2.0f;
};

class ShadowHighlightOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ShadowHighlightOverride, "ShadowHighlight",
                              "Shadows / Highlights", Color)
    float shadowLift           = 0.08f;
    float highlightCompression = 0.08f;
};

class SharpenOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SharpenOverride, "Sharpen", "Sharpen", Color)
    float strength = 0.35f;
    float radius   = 1.0f;
};

class FilmGrainOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FilmGrainOverride, "FilmGrain", "Film Grain", Color)
    float intensity = 0.03f;
    float response  = 0.8f;
};

// ── レンズ ──────────────────────────────────────────────────────────────
class VignetteOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(VignetteOverride, "Vignette", "Vignette", Lens)
    float intensity  = 0.25f;
    float smoothness = 0.45f;
    float roundness  = 1.0f;
    float color[3]   = { 0.0f, 0.0f, 0.0f };
};

class DepthOfFieldOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(DepthOfFieldOverride, "DepthOfField", "Depth of Field", Lens)
    float focusDistance = 8.0f;
    float focusRange    = 4.0f;
    float blurRadius    = 3.0f;
};

class ChromaticAberrationOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ChromaticAberrationOverride, "ChromaticAberration",
                              "Chromatic Aberration", Lens)
    float amount = 0.005f;
};

class LensDistortionOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LensDistortionOverride, "LensDistortion", "Lens Distortion", Lens)
    float distortion = 0.0f;
};

class LensFlareOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LensFlareOverride, "LensFlare", "Lens Flare", Lens)
    float intensity  = 0.5f;
    float haloWidth  = 0.4f;
    float distortion = 1.0f;
    int   ghostCount = 4;
};

class MotionBlurOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(MotionBlurOverride, "MotionBlur", "Motion Blur", Lens)
    float strength = 0.5f;
    int   samples  = 16;
};

// ── 大気 ────────────────────────────────────────────────────────────────
class FogOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FogOverride, "Fog", "Fog", Atmosphere)
    float density     = 0.04f;
    float farDistance = 80.0f;
    float color[3]    = { 0.55f, 0.65f, 0.75f };
    // 0=Exponential (固定色) / 1=Atmosphere (大気散乱)。
    // AtmosphericScatteringComponent::FogSource と一致させること。
    int   source      = 0;
};

class VolumetricLightOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(VolumetricLightOverride, "VolumetricLight",
                              "Volumetric Light", Atmosphere)
    float scattering  = 0.3f;
    float intensity   = 0.8f;
    float minDistance = 0.0f;
    float maxDistance = 30.0f;
    float edgeFade    = 0.2f;
    float density     = 0.0f;
    float heightFalloff = 0.0f;
    float heightStart   = 0.0f;
    float tint[3]     = { 1.0f, 1.0f, 1.0f };
    int   steps       = 32;
};

// ── 影・反射 (いずれも Deferred Pipeline 専用) ──────────────────────────
class SsrOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SsrOverride, "SSR", "Screen Space Reflections", Shadowing)
    float maxDistance = 50.0f;
    float thickness   = 0.15f;
    float intensity   = 0.8f;
    int   steps       = 64;
};

class ContactShadowOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ContactShadowOverride, "ContactShadow", "Contact Shadows", Shadowing)
    float strength  = 0.5f;
    float rayLength = 1.5f;
    float thickness = 0.2f;
    int   steps     = 16;
};

// ── 演出 ────────────────────────────────────────────────────────────────
class SepiaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SepiaOverride, "Sepia", "Sepia", Stylize)
    float intensity = 0.75f;
};

class InvertOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(InvertOverride, "Invert", "Invert", Stylize)
    float intensity = 1.0f;
};

class PosterizeOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(PosterizeOverride, "Posterize", "Posterize", Stylize)
    float levels = 6.0f;
};

class PixelateOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(PixelateOverride, "Pixelate", "Pixelate", Stylize)
    float pixelSize = 4.0f;
};

// ── カスタム ────────────────────────────────────────────────────────────
// 自作 HLSL をフルスクリーンパスとして差し込む。別々のシェーダーを重ねられるよう
// これだけは 1 プロファイルに複数入れられる。
class CustomEffectOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(CustomEffectOverride, "CustomEffect", "Custom Effect", Custom)
public:
    [[nodiscard]] bool AllowsMultiple() const override { return true; }

    renderer::CustomPostProcessSettings effect;
};

#undef FBZZ_VOLUME_OVERRIDE_BODY

} // namespace fbzz::asset
