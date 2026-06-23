// FBZZ Engine
// PostProcessAsset.cpp | fbzz::asset
// .fzpp ポストプロセスプロファイルの TOML 永続化実装
// キー命名規則: snake_case (ProjectSettings の camelCase とは別フォーマット)
// 失敗時は bool で返し、例外は使わない。
#include <Engine/Asset/PostProcessAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>
#include <string>

namespace fbzz::asset {

namespace {

bool ReadBool(const toml::table& t, const char* key, bool def)
{
    if (auto v = t[key].value<bool>()) return *v;
    return def;
}

float ReadFloat(const toml::table& t, const char* key, float def)
{
    if (auto v = t[key].value<double>())   return static_cast<float>(*v);
    if (auto v = t[key].value<int64_t>()) return static_cast<float>(*v);
    return def;
}

void ReadFloat3(const toml::table& t, const char* key, float out[3])
{
    const auto* arr = t[key].as_array();
    if (!arr || arr->size() < 3) return;
    for (int i = 0; i < 3; ++i) {
        if (auto v = (*arr)[static_cast<size_t>(i)].value<double>())
            out[i] = static_cast<float>(*v);
    }
}

toml::array MakeFloat3(const float v[3])
{
    toml::array a;
    a.push_back((double)v[0]);
    a.push_back((double)v[1]);
    a.push_back((double)v[2]);
    return a;
}

void NormalizeFloats(toml::node& node)
{
    constexpr double SCALE = 1000000.0;
    if (auto* fv = node.as_floating_point()) {
        const double r = std::round(fv->get() * SCALE) / SCALE;
        fv->get() = (r == 0.0) ? 0.0 : r;
        return;
    }
    if (auto* tbl = node.as_table())
        for (auto&& [k, v] : *tbl) { (void)k; NormalizeFloats(v); }
    if (auto* arr = node.as_array())
        for (auto& v : *arr) NormalizeFloats(v);
}

} // namespace

bool LoadPostProcessAssetFromFile(std::string_view path, renderer::PostProcessSettings& out)
{
    std::string text;
    if (!util::FileSystem::ReadText(std::string(path), text)) {
        FBZZ_LOG_WARN("PostProcessAsset: read failed: %s", std::string(path).c_str());
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("PostProcessAsset: parse failed: %s", std::string(path).c_str());
        return false;
    }
    auto& root = result.table();

    if (auto* t = root["base"].as_table()) {
        out.fxaaEnabled = ReadBool(*t, "fxaa_enabled", out.fxaaEnabled);
        out.exposure    = ReadFloat(*t, "exposure", out.exposure);
    }

    if (auto* t = root["bloom"].as_table()) {
        out.bloom.enabled   = ReadBool (*t, "enabled",   out.bloom.enabled);
        out.bloom.intensity = ReadFloat(*t, "intensity", out.bloom.intensity);
        out.bloom.threshold = ReadFloat(*t, "threshold", out.bloom.threshold);
        out.bloom.softKnee  = ReadFloat(*t, "soft_knee", out.bloom.softKnee);
    }

    if (auto* t = root["ambient_occlusion"].as_table()) {
        out.ambientOcclusion.enabled   = ReadBool (*t, "enabled",   out.ambientOcclusion.enabled);
        out.ambientOcclusion.intensity = ReadFloat(*t, "intensity", out.ambientOcclusion.intensity);
    }

    if (auto* t = root["fog"].as_table()) {
        out.fog.enabled     = ReadBool (*t, "enabled",  out.fog.enabled);
        out.fog.density     = ReadFloat(*t, "density",  out.fog.density);
        out.fog.farDistance = ReadFloat(*t, "far",      out.fog.farDistance);
        ReadFloat3(*t, "color", out.fog.color);
    }

    if (auto* t = root["color_grading"].as_table()) {
        out.colorGrading.enabled     = ReadBool (*t, "enabled",     out.colorGrading.enabled);
        out.colorGrading.contrast    = ReadFloat(*t, "contrast",    out.colorGrading.contrast);
        out.colorGrading.saturation  = ReadFloat(*t, "saturation",  out.colorGrading.saturation);
        out.colorGrading.hueShift    = ReadFloat(*t, "hue_shift",   out.colorGrading.hueShift);
        out.colorGrading.temperature = ReadFloat(*t, "temperature", out.colorGrading.temperature);
        out.colorGrading.tint        = ReadFloat(*t, "tint",        out.colorGrading.tint);
    }

    if (auto* t = root["vignette"].as_table()) {
        out.vignette.enabled    = ReadBool (*t, "enabled",    out.vignette.enabled);
        out.vignette.intensity  = ReadFloat(*t, "intensity",  out.vignette.intensity);
        out.vignette.smoothness = ReadFloat(*t, "smoothness", out.vignette.smoothness);
        out.vignette.roundness  = ReadFloat(*t, "roundness",  out.vignette.roundness);
        ReadFloat3(*t, "color", out.vignette.color);
    }

    if (auto* t = root["film_grain"].as_table()) {
        out.filmGrain.enabled   = ReadBool (*t, "enabled",   out.filmGrain.enabled);
        out.filmGrain.intensity = ReadFloat(*t, "intensity", out.filmGrain.intensity);
        out.filmGrain.response  = ReadFloat(*t, "response",  out.filmGrain.response);
    }

    if (auto* t = root["sharpen"].as_table()) {
        out.sharpen.enabled  = ReadBool (*t, "enabled",  out.sharpen.enabled);
        out.sharpen.strength = ReadFloat(*t, "strength", out.sharpen.strength);
        out.sharpen.radius   = ReadFloat(*t, "radius",   out.sharpen.radius);
    }

    if (auto* t = root["depth_of_field"].as_table()) {
        out.depthOfField.enabled       = ReadBool (*t, "enabled",        out.depthOfField.enabled);
        out.depthOfField.focusDistance = ReadFloat(*t, "focus_distance", out.depthOfField.focusDistance);
        out.depthOfField.focusRange    = ReadFloat(*t, "focus_range",    out.depthOfField.focusRange);
        out.depthOfField.blurRadius    = ReadFloat(*t, "blur_radius",    out.depthOfField.blurRadius);
    }

    if (auto* t = root["lens"].as_table()) {
        out.lens.chromaticAberrationEnabled = ReadBool (*t, "chromatic_aberration_enabled", out.lens.chromaticAberrationEnabled);
        out.lens.distortionEnabled          = ReadBool (*t, "distortion_enabled",           out.lens.distortionEnabled);
        out.lens.chromaticAberration        = ReadFloat(*t, "chromatic_aberration",         out.lens.chromaticAberration);
        out.lens.distortion                 = ReadFloat(*t, "distortion",                   out.lens.distortion);
    }

    if (auto* t = root["stylized"].as_table()) {
        out.stylized.sepiaEnabled    = ReadBool (*t, "sepia_enabled",    out.stylized.sepiaEnabled);
        out.stylized.sepiaIntensity  = ReadFloat(*t, "sepia_intensity",  out.stylized.sepiaIntensity);
        out.stylized.invertEnabled   = ReadBool (*t, "invert_enabled",   out.stylized.invertEnabled);
        out.stylized.invertIntensity = ReadFloat(*t, "invert_intensity", out.stylized.invertIntensity);
        out.stylized.posterizeEnabled = ReadBool (*t, "posterize_enabled", out.stylized.posterizeEnabled);
        out.stylized.posterizeLevels  = ReadFloat(*t, "posterize_levels",  out.stylized.posterizeLevels);
        out.stylized.pixelateEnabled = ReadBool (*t, "pixelate_enabled", out.stylized.pixelateEnabled);
        out.stylized.pixelSize       = ReadFloat(*t, "pixel_size",       out.stylized.pixelSize);
    }

    if (auto* t = root["image_quality"].as_table()) {
        out.imageQuality.clarityEnabled          = ReadBool (*t, "clarity_enabled",           out.imageQuality.clarityEnabled);
        out.imageQuality.clarityStrength         = ReadFloat(*t, "clarity_strength",          out.imageQuality.clarityStrength);
        out.imageQuality.clarityRadius           = ReadFloat(*t, "clarity_radius",            out.imageQuality.clarityRadius);
        out.imageQuality.shadowHighlightEnabled  = ReadBool (*t, "shadow_highlight_enabled",  out.imageQuality.shadowHighlightEnabled);
        out.imageQuality.shadowLift              = ReadFloat(*t, "shadow_lift",               out.imageQuality.shadowLift);
        out.imageQuality.highlightCompression    = ReadFloat(*t, "highlight_compression",     out.imageQuality.highlightCompression);
        out.imageQuality.colorFilterEnabled      = ReadBool (*t, "color_filter_enabled",      out.imageQuality.colorFilterEnabled);
        out.imageQuality.colorFilterIntensity    = ReadFloat(*t, "color_filter_intensity",    out.imageQuality.colorFilterIntensity);
        ReadFloat3(*t, "color_filter", out.imageQuality.colorFilter);
    }

    if (auto* arr = root["custom_effects"].as_array()) {
        out.customEffects.clear();
        for (auto& elem : *arr) {
            auto* t = elem.as_table();
            if (!t) continue;
            renderer::CustomPostProcessSettings custom;
            custom.name       = (*t)["name"].value_or(custom.name);
            custom.enabled    = ReadBool (*t, "enabled",   custom.enabled);
            custom.shaderPath = (*t)["shader"].value_or(custom.shaderPath);
            custom.intensity  = ReadFloat(*t, "intensity", custom.intensity);
            custom.blend      = ReadFloat(*t, "blend",     custom.blend);
            if (auto* pa = (*t)["parameters"].as_array(); pa && pa->size() >= 4) {
                for (int i = 0; i < 4; ++i)
                    if (auto v = (*pa)[static_cast<size_t>(i)].value<double>())
                        custom.parameters[i] = static_cast<float>(*v);
            }
            out.customEffects.push_back(std::move(custom));
        }
    }

    return true;
}

bool SavePostProcessAssetToFile(std::string_view path, const renderer::PostProcessSettings& s)
{
    const auto& b  = s.bloom;
    const auto& ao = s.ambientOcclusion;
    const auto& f  = s.fog;
    const auto& cg = s.colorGrading;
    const auto& vg = s.vignette;
    const auto& fg = s.filmGrain;
    const auto& sh = s.sharpen;
    const auto& df = s.depthOfField;
    const auto& ln = s.lens;
    const auto& st = s.stylized;
    const auto& iq = s.imageQuality;

    toml::table baseTbl;
    baseTbl.insert("fxaa_enabled", s.fxaaEnabled);
    baseTbl.insert("exposure", (double)s.exposure);

    toml::table bloomTbl;
    bloomTbl.insert("enabled",   b.enabled);
    bloomTbl.insert("intensity", (double)b.intensity);
    bloomTbl.insert("threshold", (double)b.threshold);
    bloomTbl.insert("soft_knee", (double)b.softKnee);

    toml::table aoTbl;
    aoTbl.insert("enabled",   ao.enabled);
    aoTbl.insert("intensity", (double)ao.intensity);

    toml::table fogTbl;
    fogTbl.insert("enabled", f.enabled);
    fogTbl.insert("density", (double)f.density);
    fogTbl.insert("far",     (double)f.farDistance);
    fogTbl.insert("color",   MakeFloat3(f.color));

    toml::table cgTbl;
    cgTbl.insert("enabled",     cg.enabled);
    cgTbl.insert("contrast",    (double)cg.contrast);
    cgTbl.insert("saturation",  (double)cg.saturation);
    cgTbl.insert("hue_shift",   (double)cg.hueShift);
    cgTbl.insert("temperature", (double)cg.temperature);
    cgTbl.insert("tint",        (double)cg.tint);

    toml::table vgTbl;
    vgTbl.insert("enabled",    vg.enabled);
    vgTbl.insert("intensity",  (double)vg.intensity);
    vgTbl.insert("smoothness", (double)vg.smoothness);
    vgTbl.insert("roundness",  (double)vg.roundness);
    vgTbl.insert("color",      MakeFloat3(vg.color));

    toml::table fgTbl;
    fgTbl.insert("enabled",   fg.enabled);
    fgTbl.insert("intensity", (double)fg.intensity);
    fgTbl.insert("response",  (double)fg.response);

    toml::table shTbl;
    shTbl.insert("enabled",  sh.enabled);
    shTbl.insert("strength", (double)sh.strength);
    shTbl.insert("radius",   (double)sh.radius);

    toml::table dfTbl;
    dfTbl.insert("enabled",        df.enabled);
    dfTbl.insert("focus_distance", (double)df.focusDistance);
    dfTbl.insert("focus_range",    (double)df.focusRange);
    dfTbl.insert("blur_radius",    (double)df.blurRadius);

    toml::table lnTbl;
    lnTbl.insert("chromatic_aberration_enabled", ln.chromaticAberrationEnabled);
    lnTbl.insert("distortion_enabled",           ln.distortionEnabled);
    lnTbl.insert("chromatic_aberration",         (double)ln.chromaticAberration);
    lnTbl.insert("distortion",                   (double)ln.distortion);

    toml::table stTbl;
    stTbl.insert("sepia_enabled",    st.sepiaEnabled);
    stTbl.insert("sepia_intensity",  (double)st.sepiaIntensity);
    stTbl.insert("invert_enabled",   st.invertEnabled);
    stTbl.insert("invert_intensity", (double)st.invertIntensity);
    stTbl.insert("posterize_enabled", st.posterizeEnabled);
    stTbl.insert("posterize_levels",  (double)st.posterizeLevels);
    stTbl.insert("pixelate_enabled", st.pixelateEnabled);
    stTbl.insert("pixel_size",       (double)st.pixelSize);

    toml::table iqTbl;
    iqTbl.insert("clarity_enabled",          iq.clarityEnabled);
    iqTbl.insert("clarity_strength",         (double)iq.clarityStrength);
    iqTbl.insert("clarity_radius",           (double)iq.clarityRadius);
    iqTbl.insert("shadow_highlight_enabled", iq.shadowHighlightEnabled);
    iqTbl.insert("shadow_lift",              (double)iq.shadowLift);
    iqTbl.insert("highlight_compression",    (double)iq.highlightCompression);
    iqTbl.insert("color_filter_enabled",     iq.colorFilterEnabled);
    iqTbl.insert("color_filter",             MakeFloat3(iq.colorFilter));
    iqTbl.insert("color_filter_intensity",   (double)iq.colorFilterIntensity);

    toml::array customArr;
    for (const auto& custom : s.customEffects) {
        toml::array params;
        for (int i = 0; i < 4; ++i) params.push_back((double)custom.parameters[i]);
        toml::table customTbl;
        customTbl.insert("name",       custom.name);
        customTbl.insert("enabled",    custom.enabled);
        customTbl.insert("shader",     custom.shaderPath);
        customTbl.insert("intensity",  (double)custom.intensity);
        customTbl.insert("blend",      (double)custom.blend);
        customTbl.insert("parameters", std::move(params));
        customArr.push_back(std::move(customTbl));
    }

    toml::table root;
    root.insert("base",            std::move(baseTbl));
    root.insert("bloom",           std::move(bloomTbl));
    root.insert("ambient_occlusion", std::move(aoTbl));
    root.insert("fog",             std::move(fogTbl));
    root.insert("color_grading",   std::move(cgTbl));
    root.insert("vignette",        std::move(vgTbl));
    root.insert("film_grain",      std::move(fgTbl));
    root.insert("sharpen",         std::move(shTbl));
    root.insert("depth_of_field",  std::move(dfTbl));
    root.insert("lens",            std::move(lnTbl));
    root.insert("stylized",        std::move(stTbl));
    root.insert("image_quality",   std::move(iqTbl));
    root.insert("custom_effects",  std::move(customArr));

    NormalizeFloats(root);

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(std::string(path), ss.str());
}

} // namespace fbzz::asset
