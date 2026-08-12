// FBZZ Engine
// PostProcessProfile.cpp | fbzz::asset
// PostProcessSettings のリフレクション定義と DataAssetFactory への登録
//
// 各サブ構造体ごとに自由関数の反映ヘルパーを用意し、BeginObject / EndObject で
// 名前付きスコープに包む。これにより .fzdata では [bloom] / [colorGrading] のような
// 入れ子テーブルになり、同名フィールド (intensity など) が衝突しない。
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>

#include <cstddef>
#include <cstdio>
#include <string>

namespace fbzz::asset {

namespace {

using scene::IReflector;

// float[3] を Vector3 経由で反映し、カラーピッカーのヒントを付ける。
// WHY 経由が要るか: IReflector は C 配列を直接扱えない。
//     一時 Vector3 へ写して反映し、書き戻す。
void ReflectColor3(IReflector& r, const char* name, float (&color)[3])
{
    math::Vector3 value{ color[0], color[1], color[2] };
    r.BeginField(name, name);
    r.SetFieldHint(IReflector::FieldHint::Color);
    r.Field(name, value);
    color[0] = value.x;
    color[1] = value.y;
    color[2] = value.z;
}

// スカラー 1 個を「名前 + レンジ」で反映する短縮形。
void Range(IReflector& r, const char* name, float& value, float min, float max)
{
    r.BeginField(name, name);
    r.FloatRange(name, value, min, max);
}

void Boolean(IReflector& r, const char* name, bool& value)
{
    r.BeginField(name, name);
    r.Field(name, value);
}

void Text(IReflector& r, const char* name, std::string& value)
{
    r.BeginField(name, name);
    r.Field(name, value);
}

// ── サブ構造体ごとの反映 ────────────────────────────────────────────────────

void ReflectBloom(IReflector& r, renderer::BloomSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "intensity", v.intensity, 0.0f, 5.0f);
    Range(r, "threshold", v.threshold, 0.0f, 4.0f);
    Range(r, "softKnee",  v.softKnee,  0.0f, 1.0f);
}

void ReflectAmbientOcclusion(IReflector& r, renderer::AmbientOcclusionSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "intensity", v.intensity, 0.0f, 4.0f);
}

void Integer(IReflector& r, const char* name, int& value)
{
    r.BeginField(name, name);
    r.Field(name, value);
}

void ReflectFog(IReflector& r, renderer::FogSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "density",     v.density,     0.0f, 1.0f);
    Range(r, "farDistance", v.farDistance, 0.0f, 10000.0f);
    ReflectColor3(r, "color", v.color);
    // 0=Exponential (固定色) / 1=Atmosphere (大気散乱)
    Integer(r, "source", v.source);
}

void ReflectLens(IReflector& r, renderer::LensSettings& v)
{
    Boolean(r, "chromaticAberrationEnabled", v.chromaticAberrationEnabled);
    Boolean(r, "distortionEnabled",          v.distortionEnabled);
    Range(r, "chromaticAberration", v.chromaticAberration, 0.0f, 0.1f);
    Range(r, "distortion",          v.distortion,         -1.0f, 1.0f);
}

void ReflectStylized(IReflector& r, renderer::StylizedPostProcessSettings& v)
{
    Boolean(r, "sepiaEnabled",     v.sepiaEnabled);
    Boolean(r, "invertEnabled",    v.invertEnabled);
    Boolean(r, "posterizeEnabled", v.posterizeEnabled);
    Boolean(r, "pixelateEnabled",  v.pixelateEnabled);
    Range(r, "sepiaIntensity",  v.sepiaIntensity,  0.0f, 1.0f);
    Range(r, "invertIntensity", v.invertIntensity, 0.0f, 1.0f);
    Range(r, "posterizeLevels", v.posterizeLevels, 2.0f, 64.0f);
    Range(r, "pixelSize",       v.pixelSize,       1.0f, 64.0f);
}

void ReflectImageQuality(IReflector& r, renderer::ImageQualitySettings& v)
{
    Boolean(r, "clarityEnabled",         v.clarityEnabled);
    Boolean(r, "shadowHighlightEnabled", v.shadowHighlightEnabled);
    Boolean(r, "colorFilterEnabled",     v.colorFilterEnabled);
    Range(r, "clarityStrength",      v.clarityStrength,      0.0f, 1.0f);
    Range(r, "clarityRadius",        v.clarityRadius,        0.0f, 8.0f);
    Range(r, "shadowLift",           v.shadowLift,          -1.0f, 1.0f);
    Range(r, "highlightCompression", v.highlightCompression, 0.0f, 1.0f);
    ReflectColor3(r, "colorFilter", v.colorFilter);
    Range(r, "colorFilterIntensity", v.colorFilterIntensity, 0.0f, 1.0f);
}

void ReflectColorGrading(IReflector& r, renderer::ColorGradingSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "contrast",    v.contrast,   -1.0f, 1.0f);
    Range(r, "saturation",  v.saturation,  0.0f, 3.0f);
    Range(r, "hueShift",    v.hueShift,   -1.0f, 1.0f);
    Range(r, "temperature", v.temperature, -1.0f, 1.0f);
    Range(r, "tint",        v.tint,       -1.0f, 1.0f);
}

void ReflectVignette(IReflector& r, renderer::VignetteSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "intensity",  v.intensity,  0.0f, 1.0f);
    Range(r, "smoothness", v.smoothness, 0.0f, 1.0f);
    Range(r, "roundness",  v.roundness,  0.0f, 1.0f);
    ReflectColor3(r, "color", v.color);
}

void ReflectFilmGrain(IReflector& r, renderer::FilmGrainSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "intensity", v.intensity, 0.0f, 1.0f);
    Range(r, "response",  v.response,  0.0f, 1.0f);
}

void ReflectSharpen(IReflector& r, renderer::SharpenSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "strength", v.strength, 0.0f, 2.0f);
    Range(r, "radius",   v.radius,   0.0f, 4.0f);
}

void ReflectDepthOfField(IReflector& r, renderer::DepthOfFieldSettings& v)
{
    Boolean(r, "enabled", v.enabled);
    Range(r, "focusDistance", v.focusDistance, 0.0f, 1000.0f);
    Range(r, "focusRange",    v.focusRange,    0.0f, 1000.0f);
    Range(r, "blurRadius",    v.blurRadius,    0.0f, 16.0f);
}

void ReflectCustomEffect(IReflector& r, renderer::CustomPostProcessSettings& v)
{
    Text(r, "name", v.name);
    Boolean(r, "enabled", v.enabled);
    Text(r, "shaderPath", v.shaderPath);
    Range(r, "intensity", v.intensity, 0.0f, 4.0f);
    Range(r, "blend",     v.blend,     0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "param%d", i);
        Range(r, name, v.parameters[i], -100.0f, 100.0f);
    }
}

// 名前付きスコープで包む短縮形。
template<typename Fn>
void Scope(IReflector& r, const char* name, Fn&& fn)
{
    r.BeginField(name, name);
    r.BeginObject(name);
    fn();
    r.EndObject();
}

} // namespace

void PostProcessProfile::Reflect(scene::IReflector& r)
{
    Boolean(r, "fxaaEnabled", settings.fxaaEnabled);
    Range(r, "exposure", settings.exposure, 0.0f, 8.0f);

    Scope(r, "bloom",            [&] { ReflectBloom(r, settings.bloom); });
    Scope(r, "ambientOcclusion", [&] { ReflectAmbientOcclusion(r, settings.ambientOcclusion); });
    Scope(r, "fog",              [&] { ReflectFog(r, settings.fog); });
    Scope(r, "colorGrading",     [&] { ReflectColorGrading(r, settings.colorGrading); });
    Scope(r, "vignette",         [&] { ReflectVignette(r, settings.vignette); });
    Scope(r, "filmGrain",        [&] { ReflectFilmGrain(r, settings.filmGrain); });
    Scope(r, "sharpen",          [&] { ReflectSharpen(r, settings.sharpen); });
    Scope(r, "depthOfField",     [&] { ReflectDepthOfField(r, settings.depthOfField); });
    Scope(r, "lens",             [&] { ReflectLens(r, settings.lens); });
    Scope(r, "stylized",         [&] { ReflectStylized(r, settings.stylized); });
    Scope(r, "imageQuality",     [&] { ReflectImageQuality(r, settings.imageQuality); });

    // ── ユーザー定義エフェクト (構造体配列) ──────────────────────────────────
    {
        r.BeginField("customEffects", "customEffects");
        const std::size_t count =
            r.BeginObjectList("customEffects", settings.customEffects.size());
        settings.customEffects.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            r.BeginObjectElement(i);
            ReflectCustomEffect(r, settings.customEffects[i]);
            r.EndObjectElement();
        }
        const std::size_t removeIndex = r.EndObjectList();
        if (removeIndex < settings.customEffects.size()) {
            settings.customEffects.erase(
                settings.customEffects.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        }
    }

    // ── 上書き対象セクション ────────────────────────────────────────────────
    // WHY 別スコープに分けるか: 値とメタデータを混ぜると .fzdata が読みにくくなる。
    //     [overrides] にまとめることで「このプロファイルが何に責任を持つか」が
    //     ファイルの一箇所で分かる。
    Scope(r, "overrides", [&] {
        Boolean(r, "fxaa",             overrides.fxaa);
        Boolean(r, "exposure",         overrides.exposure);
        Boolean(r, "bloom",            overrides.bloom);
        Boolean(r, "ambientOcclusion", overrides.ambientOcclusion);
        Boolean(r, "fog",              overrides.fog);
        Boolean(r, "colorGrading",     overrides.colorGrading);
        Boolean(r, "vignette",         overrides.vignette);
        Boolean(r, "filmGrain",        overrides.filmGrain);
        Boolean(r, "sharpen",          overrides.sharpen);
        Boolean(r, "depthOfField",     overrides.depthOfField);
        Boolean(r, "lens",             overrides.lens);
        Boolean(r, "stylized",         overrides.stylized);
        Boolean(r, "imageQuality",     overrides.imageQuality);
        Boolean(r, "customEffects",    overrides.customEffects);
    });

    // 画面フェードはランタイム演出用の一時値であり、プロファイルとしては保存しない。
    // WHY: フェード中に profile を保存すると「真っ黒なプロファイル」ができてしまう。
}

} // namespace fbzz::asset

FBZZ_REGISTER_DATA_ASSET(::fbzz::asset::PostProcessProfile);
