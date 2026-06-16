// FBZZ Engine
// ProjectSettings.cpp | fbzz
// プロジェクト設定の TOML 永続化実装
// タグ・レイヤーなどエディタとランタイムで共有する設定を読み書きする。
// 失敗時は bool で返し、例外は使わない。
#include <Engine/ProjectSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>
#include <utility>

namespace fbzz {

namespace {

double RoundTomlFloat(double value)
{
    constexpr double SCALE = 1000000.0;
    const double rounded = std::round(value * SCALE) / SCALE;
    return rounded == 0.0 ? 0.0 : rounded;
}

void NormalizeTomlFloats(toml::node& node)
{
    if (auto* value = node.as_floating_point()) {
        value->get() = RoundTomlFloat(value->get());
        return;
    }

    if (auto* table = node.as_table()) {
        for (auto&& [key, child] : *table) {
            (void)key;
            NormalizeTomlFloats(child);
        }
        return;
    }

    if (auto* array = node.as_array()) {
        for (auto& child : *array)
            NormalizeTomlFloats(child);
    }
}

toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    return a;
}

math::Vector3 ArrToVec3(const toml::array* arr, const math::Vector3& def)
{
    if (!arr || arr->size() < 3) return def;
    return {
        (float)(*arr)[0].value_or((double)def.x),
        (float)(*arr)[1].value_or((double)def.y),
        (float)(*arr)[2].value_or((double)def.z)
    };
}

bool ReadBool(const toml::table& table, const char* key, bool fallback)
{
    if (auto value = table[key].value<bool>())
        return *value;
    return fallback;
}

float ReadFloat(const toml::table& table, const char* key, float fallback)
{
    if (auto value = table[key].value<double>())
        return static_cast<float>(*value);
    if (auto value = table[key].value<int64_t>())
        return static_cast<float>(*value);
    return fallback;
}

void ReadFloat3(const toml::table& table, const char* key, float out[3])
{
    const auto* arr = table[key].as_array();
    if (!arr || arr->size() < 3)
        return;

    for (int i = 0; i < 3; ++i) {
        if (auto value = (*arr)[static_cast<size_t>(i)].value<double>()) {
            out[i] = static_cast<float>(*value);
        } else if (auto intValue = (*arr)[static_cast<size_t>(i)].value<int64_t>()) {
            out[i] = static_cast<float>(*intValue);
        }
    }
}

} // namespace

ProjectSettings ProjectSettings::Default()
{
    ProjectSettings ps;
    ps.project.defaultScene = "Assets/Scenes/Main.scene";
    ps.runtime.startScene   = "Assets/Scenes/Main.scene";
    ps.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                "MainCamera", "Player", "GameController" };
    ps.layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
    ps.physics.hz       = 60;
    ps.physics.substeps = 1;
    ps.physics.gravity  = { 0.0f, -9.81f, 0.0f };
    return ps;
}

bool ProjectSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        // WHY: Load 失敗時に既存設定を代入で破棄すると、呼び出し側が保持していた設定や UI 参照まで巻き戻る。
        //      失敗は bool で伝え、現在の設定はそのまま残す。
        FBZZ_LOG_WARN("ProjectSettings: read failed: %s", path.c_str());
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("ProjectSettings: parse failed: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();

    if (auto* projectTbl = tbl["project"].as_table()) {
        project.name = (*projectTbl)["name"].value_or(project.name);
        project.defaultScene = (*projectTbl)["default_scene"].value_or(project.defaultScene);
    }

    if (auto* runtimeTbl = tbl["runtime"].as_table()) {
        runtime.startScene = (*runtimeTbl)["start_scene"].value_or(runtime.startScene);
    }

    if (auto* tagArr = tbl["tags"]["list"].as_array()) {
        tags.clear();
        for (auto& elem : *tagArr)
            if (auto v = elem.value<std::string>())
                tags.push_back(*v);
    }

    if (auto* layArr = tbl["layers"]["names"].as_array()) {
        for (int i = 0; i < 32 && i < (int)layArr->size(); ++i)
            if (auto v = (*layArr)[i].value<std::string>())
                layerNames[i] = *v;
    }

    if (auto* physicsTbl = tbl["physics"].as_table()) {
        physics.hz      = (int)(*physicsTbl)["hz"].value_or((int64_t)physics.hz);
        physics.substeps = (int)(*physicsTbl)["substeps"].value_or((int64_t)physics.substeps);
        physics.gravity = ArrToVec3((*physicsTbl)["gravity"].as_array(), physics.gravity);
    }

    if (physics.hz < 1)        physics.hz = 1;
    if (physics.hz > 1000)     physics.hz = 1000;
    if (physics.substeps < 1)  physics.substeps = 1;
    if (physics.substeps > 32) physics.substeps = 32;

    if (auto* renderTbl = tbl["render"].as_table()) {
        auto& pp = render.postProcess;
        {
            const auto s = (*renderTbl)["pipeline"].value_or(std::string("forward"));
            render.pipeline = (s == "deferred")
                ? renderer::RenderingPipeline::Deferred
                : renderer::RenderingPipeline::Forward;
        }
        {
            const int vm = (*renderTbl)["viewMode"].value_or(static_cast<int>(render.viewMode));
            render.viewMode = static_cast<renderer::ViewMode>(vm);
        }
        render.shadowEnabled = (*renderTbl)["shadow"].value_or(render.shadowEnabled);
        pp.bloom.enabled = (*renderTbl)["bloom"].value_or(pp.bloom.enabled);
        pp.ambientOcclusion.enabled   = (*renderTbl)["ambientOcclusion"].value_or(pp.ambientOcclusion.enabled);
        pp.ambientOcclusion.intensity = (float)(*renderTbl)["ambientOcclusionIntensity"].value_or((double)pp.ambientOcclusion.intensity);
        pp.fxaaEnabled = (*renderTbl)["fxaa"].value_or(pp.fxaaEnabled);
        pp.fog.enabled = (*renderTbl)["fog"].value_or(pp.fog.enabled);
        pp.colorGrading.enabled = (*renderTbl)["colorGrading"].value_or(pp.colorGrading.enabled);
        pp.vignette.enabled = (*renderTbl)["vignette"].value_or(pp.vignette.enabled);
        pp.filmGrain.enabled = (*renderTbl)["filmGrain"].value_or(pp.filmGrain.enabled);
        pp.sharpen.enabled = ReadBool(*renderTbl, "sharpen", pp.sharpen.enabled);
        pp.depthOfField.enabled = ReadBool(*renderTbl, "depthOfField", pp.depthOfField.enabled);
        pp.lens.chromaticAberrationEnabled = (*renderTbl)["chromaticAberrationEnabled"].value_or(pp.lens.chromaticAberrationEnabled);
        pp.lens.distortionEnabled = (*renderTbl)["lensDistortionEnabled"].value_or(pp.lens.distortionEnabled);
        pp.stylized.sepiaEnabled = ReadBool(*renderTbl, "sepia", pp.stylized.sepiaEnabled);
        pp.stylized.invertEnabled = ReadBool(*renderTbl, "invert", pp.stylized.invertEnabled);
        pp.stylized.posterizeEnabled = ReadBool(*renderTbl, "posterize", pp.stylized.posterizeEnabled);
        pp.stylized.pixelateEnabled = ReadBool(*renderTbl, "pixelate", pp.stylized.pixelateEnabled);
        pp.imageQuality.clarityEnabled = ReadBool(*renderTbl, "clarity", pp.imageQuality.clarityEnabled);
        pp.imageQuality.shadowHighlightEnabled = ReadBool(*renderTbl, "shadowHighlight", pp.imageQuality.shadowHighlightEnabled);
        pp.imageQuality.colorFilterEnabled = ReadBool(*renderTbl, "colorFilter", pp.imageQuality.colorFilterEnabled);
        render.showColliders = (*renderTbl)["showColliders"].value_or(render.showColliders);
        render.showDecalBounds = (*renderTbl)["showDecalBounds"].value_or(render.showDecalBounds);
        render.showSelectionOutline = (*renderTbl)["showSelectionOutline"].value_or(render.showSelectionOutline);
        render.passViewerEnabled = (*renderTbl)["passViewerEnabled"].value_or(render.passViewerEnabled);
        pp.exposure = (float)(*renderTbl)["exposure"].value_or((double)pp.exposure);
        pp.bloom.intensity = (float)(*renderTbl)["bloomIntensity"].value_or((double)pp.bloom.intensity);
        pp.bloom.threshold = ReadFloat(*renderTbl, "bloomThreshold", pp.bloom.threshold);
        pp.bloom.softKnee = ReadFloat(*renderTbl, "bloomSoftKnee", pp.bloom.softKnee);
        pp.fog.density = (float)(*renderTbl)["fogDensity"].value_or((double)pp.fog.density);
        pp.fog.farDistance = (float)(*renderTbl)["fogFar"].value_or((double)pp.fog.farDistance);
        pp.colorGrading.contrast = (float)(*renderTbl)["contrast"].value_or((double)pp.colorGrading.contrast);
        pp.colorGrading.saturation = (float)(*renderTbl)["saturation"].value_or((double)pp.colorGrading.saturation);
        pp.colorGrading.hueShift = (float)(*renderTbl)["hueShift"].value_or((double)pp.colorGrading.hueShift);
        pp.colorGrading.temperature = (float)(*renderTbl)["temperature"].value_or((double)pp.colorGrading.temperature);
        pp.colorGrading.tint = (float)(*renderTbl)["tint"].value_or((double)pp.colorGrading.tint);
        pp.vignette.intensity = (float)(*renderTbl)["vignetteIntensity"].value_or((double)pp.vignette.intensity);
        pp.vignette.smoothness = (float)(*renderTbl)["vignetteSmoothness"].value_or((double)pp.vignette.smoothness);
        pp.vignette.roundness = (float)(*renderTbl)["vignetteRoundness"].value_or((double)pp.vignette.roundness);
        pp.filmGrain.intensity = (float)(*renderTbl)["filmGrainIntensity"].value_or((double)pp.filmGrain.intensity);
        pp.filmGrain.response = (float)(*renderTbl)["filmGrainResponse"].value_or((double)pp.filmGrain.response);
        pp.sharpen.strength = ReadFloat(*renderTbl, "sharpenStrength", pp.sharpen.strength);
        pp.sharpen.radius = ReadFloat(*renderTbl, "sharpenRadius", pp.sharpen.radius);
        pp.depthOfField.focusDistance = ReadFloat(*renderTbl, "dofFocusDistance", pp.depthOfField.focusDistance);
        pp.depthOfField.focusRange = ReadFloat(*renderTbl, "dofFocusRange", pp.depthOfField.focusRange);
        pp.depthOfField.blurRadius = ReadFloat(*renderTbl, "dofBlurRadius", pp.depthOfField.blurRadius);
        pp.lens.chromaticAberration = (float)(*renderTbl)["chromaticAberration"].value_or((double)pp.lens.chromaticAberration);
        pp.lens.distortion = (float)(*renderTbl)["lensDistortion"].value_or((double)pp.lens.distortion);
        pp.stylized.sepiaIntensity = ReadFloat(*renderTbl, "sepiaIntensity", pp.stylized.sepiaIntensity);
        pp.stylized.invertIntensity = ReadFloat(*renderTbl, "invertIntensity", pp.stylized.invertIntensity);
        pp.stylized.posterizeLevels = ReadFloat(*renderTbl, "posterizeLevels", pp.stylized.posterizeLevels);
        pp.stylized.pixelSize = ReadFloat(*renderTbl, "pixelSize", pp.stylized.pixelSize);
        pp.imageQuality.clarityStrength = ReadFloat(*renderTbl, "clarityStrength", pp.imageQuality.clarityStrength);
        pp.imageQuality.clarityRadius = ReadFloat(*renderTbl, "clarityRadius", pp.imageQuality.clarityRadius);
        pp.imageQuality.shadowLift = ReadFloat(*renderTbl, "shadowLift", pp.imageQuality.shadowLift);
        pp.imageQuality.highlightCompression = ReadFloat(*renderTbl, "highlightCompression", pp.imageQuality.highlightCompression);
        pp.imageQuality.colorFilterIntensity = ReadFloat(*renderTbl, "colorFilterIntensity", pp.imageQuality.colorFilterIntensity);
        render.outlineWidth  = (float)(*renderTbl)["outlineWidth"].value_or((double)render.outlineWidth);
        if (auto* fogColorArr = (*renderTbl)["fogColor"].as_array(); fogColorArr && fogColorArr->size() >= 3) {
            pp.fog.color[0] = (float)(*fogColorArr)[0].value_or((double)pp.fog.color[0]);
            pp.fog.color[1] = (float)(*fogColorArr)[1].value_or((double)pp.fog.color[1]);
            pp.fog.color[2] = (float)(*fogColorArr)[2].value_or((double)pp.fog.color[2]);
        }
        if (auto* vignetteColorArr = (*renderTbl)["vignetteColor"].as_array(); vignetteColorArr && vignetteColorArr->size() >= 3) {
            pp.vignette.color[0] = (float)(*vignetteColorArr)[0].value_or((double)pp.vignette.color[0]);
            pp.vignette.color[1] = (float)(*vignetteColorArr)[1].value_or((double)pp.vignette.color[1]);
            pp.vignette.color[2] = (float)(*vignetteColorArr)[2].value_or((double)pp.vignette.color[2]);
        }
        ReadFloat3(*renderTbl, "colorFilterColor", pp.imageQuality.colorFilter);
        if (auto* outlineColorArr = (*renderTbl)["outlineColor"].as_array(); outlineColorArr && outlineColorArr->size() >= 4) {
            render.outlineColor[0] = (float)(*outlineColorArr)[0].value_or((double)render.outlineColor[0]);
            render.outlineColor[1] = (float)(*outlineColorArr)[1].value_or((double)render.outlineColor[1]);
            render.outlineColor[2] = (float)(*outlineColorArr)[2].value_or((double)render.outlineColor[2]);
            render.outlineColor[3] = (float)(*outlineColorArr)[3].value_or((double)render.outlineColor[3]);
        }
        if (auto* customArr = (*renderTbl)["customPostProcesses"].as_array()) {
            pp.customEffects.clear();
            for (auto& elem : *customArr) {
                auto* customTbl = elem.as_table();
                if (!customTbl) continue;

                renderer::CustomPostProcessSettings custom;
                custom.name = (*customTbl)["name"].value_or(custom.name);
                custom.enabled = (*customTbl)["enabled"].value_or(custom.enabled);
                custom.shaderPath = (*customTbl)["shader"].value_or(custom.shaderPath);
                custom.intensity = (float)(*customTbl)["intensity"].value_or((double)custom.intensity);
                custom.blend = (float)(*customTbl)["blend"].value_or((double)custom.blend);
                if (auto* paramsArr = (*customTbl)["parameters"].as_array(); paramsArr && paramsArr->size() >= 4) {
                    custom.parameters[0] = (float)(*paramsArr)[0].value_or((double)custom.parameters[0]);
                    custom.parameters[1] = (float)(*paramsArr)[1].value_or((double)custom.parameters[1]);
                    custom.parameters[2] = (float)(*paramsArr)[2].value_or((double)custom.parameters[2]);
                    custom.parameters[3] = (float)(*paramsArr)[3].value_or((double)custom.parameters[3]);
                }
                pp.customEffects.push_back(std::move(custom));
            }
        } else if ((*renderTbl)["customPostProcess"].value_or(false)) {
            renderer::CustomPostProcessSettings custom;
            custom.enabled = true;
            custom.shaderPath = (*renderTbl)["customPostProcessShader"].value_or(custom.shaderPath);
            custom.intensity = (float)(*renderTbl)["customPostProcessIntensity"].value_or((double)custom.intensity);
            custom.blend = (float)(*renderTbl)["customPostProcessBlend"].value_or((double)custom.blend);
            if (auto* customParamsArr = (*renderTbl)["customPostProcessParameters"].as_array(); customParamsArr && customParamsArr->size() >= 4) {
                custom.parameters[0] = (float)(*customParamsArr)[0].value_or((double)custom.parameters[0]);
                custom.parameters[1] = (float)(*customParamsArr)[1].value_or((double)custom.parameters[1]);
                custom.parameters[2] = (float)(*customParamsArr)[2].value_or((double)custom.parameters[2]);
                custom.parameters[3] = (float)(*customParamsArr)[3].value_or((double)custom.parameters[3]);
            }
            pp.customEffects.push_back(std::move(custom));
        }
    }

    if (auto* audioTbl = tbl["audio"].as_table()) {
        audio.bgmVolume = (float)(*audioTbl)["bgmVolume"].value_or((double)audio.bgmVolume);
        audio.seVolume  = (float)(*audioTbl)["seVolume"].value_or((double)audio.seVolume);
        if (audio.bgmVolume < 0.0f) audio.bgmVolume = 0.0f;
        if (audio.bgmVolume > 1.0f) audio.bgmVolume = 1.0f;
        if (audio.seVolume  < 0.0f) audio.seVolume  = 0.0f;
        if (audio.seVolume  > 1.0f) audio.seVolume  = 1.0f;
    }

    if (auto* screenTbl = tbl["screen"].as_table()) {
        screen.width  = (int)(*screenTbl)["width"].value_or((int64_t)screen.width);
        screen.height = (int)(*screenTbl)["height"].value_or((int64_t)screen.height);
        if (screen.width  < 1) screen.width  = 1;
        if (screen.height < 1) screen.height = 1;
    }

    if (auto* appTbl = tbl["app"].as_table()) {
        app.targetFps = (int)(*appTbl)["targetFps"].value_or((int64_t)app.targetFps);
        if (app.targetFps < 0) app.targetFps = 0;
    }

    if (auto* windowTbl = tbl["window"].as_table()) {
        window.title      = (*windowTbl)["title"].value_or(window.title);
        window.width      = (int)(*windowTbl)["width"].value_or((int64_t)window.width);
        window.height     = (int)(*windowTbl)["height"].value_or((int64_t)window.height);
        window.fullscreen = (*windowTbl)["fullscreen"].value_or(window.fullscreen);
        if (window.width  < 1) window.width  = 1;
        if (window.height < 1) window.height = 1;
    }

    if (auto* uiTbl = tbl["ui"].as_table())
        ui.defaultFontPath = (*uiTbl)["default_font"].value_or(ui.defaultFontPath);

    return true;
}

bool ProjectSettings::Save(const std::string& path) const
{
    toml::array tagArr;
    for (const auto& t : tags)
        tagArr.push_back(t);

    toml::array layArr;
    for (const auto& n : layerNames)
        layArr.push_back(n);

    toml::table tagTbl;
    tagTbl.insert("list", std::move(tagArr));

    toml::table layTbl;
    layTbl.insert("names", std::move(layArr));

    toml::table physicsTbl;
    physicsTbl.insert("hz",       (int64_t)physics.hz);
    physicsTbl.insert("substeps", (int64_t)physics.substeps);
    physicsTbl.insert("gravity",  Vec3ToArr(physics.gravity));

    const auto& pp = render.postProcess;

    toml::array fogColorArr;
    fogColorArr.push_back((double)pp.fog.color[0]);
    fogColorArr.push_back((double)pp.fog.color[1]);
    fogColorArr.push_back((double)pp.fog.color[2]);

    toml::array vignetteColorArr;
    vignetteColorArr.push_back((double)pp.vignette.color[0]);
    vignetteColorArr.push_back((double)pp.vignette.color[1]);
    vignetteColorArr.push_back((double)pp.vignette.color[2]);

    toml::array colorFilterArr;
    colorFilterArr.push_back((double)pp.imageQuality.colorFilter[0]);
    colorFilterArr.push_back((double)pp.imageQuality.colorFilter[1]);
    colorFilterArr.push_back((double)pp.imageQuality.colorFilter[2]);

    toml::array outlineColorArr;
    outlineColorArr.push_back((double)render.outlineColor[0]);
    outlineColorArr.push_back((double)render.outlineColor[1]);
    outlineColorArr.push_back((double)render.outlineColor[2]);
    outlineColorArr.push_back((double)render.outlineColor[3]);

    toml::array customEffectsArr;
    for (const auto& custom : pp.customEffects) {
        toml::array customParamsArr;
        customParamsArr.push_back((double)custom.parameters[0]);
        customParamsArr.push_back((double)custom.parameters[1]);
        customParamsArr.push_back((double)custom.parameters[2]);
        customParamsArr.push_back((double)custom.parameters[3]);

        toml::table customTbl;
        customTbl.insert("name", custom.name);
        customTbl.insert("enabled", custom.enabled);
        customTbl.insert("shader", custom.shaderPath);
        customTbl.insert("intensity", (double)custom.intensity);
        customTbl.insert("blend", (double)custom.blend);
        customTbl.insert("parameters", std::move(customParamsArr));
        customEffectsArr.push_back(std::move(customTbl));
    }

    toml::table renderTbl;
    renderTbl.insert("pipeline", render.pipeline == renderer::RenderingPipeline::Deferred ? "deferred" : "forward");
    renderTbl.insert("viewMode",     static_cast<int>(render.viewMode));
    renderTbl.insert("shadow",       render.shadowEnabled);
    renderTbl.insert("bloom",                   pp.bloom.enabled);
    renderTbl.insert("ambientOcclusion",        pp.ambientOcclusion.enabled);
    renderTbl.insert("ambientOcclusionIntensity", (double)pp.ambientOcclusion.intensity);
    renderTbl.insert("fxaa",                    pp.fxaaEnabled);
    renderTbl.insert("fog",          pp.fog.enabled);
    renderTbl.insert("colorGrading", pp.colorGrading.enabled);
    renderTbl.insert("vignette",     pp.vignette.enabled);
    renderTbl.insert("filmGrain",    pp.filmGrain.enabled);
    renderTbl.insert("sharpen",      pp.sharpen.enabled);
    renderTbl.insert("depthOfField", pp.depthOfField.enabled);
    renderTbl.insert("chromaticAberrationEnabled", pp.lens.chromaticAberrationEnabled);
    renderTbl.insert("lensDistortionEnabled", pp.lens.distortionEnabled);
    renderTbl.insert("sepia",        pp.stylized.sepiaEnabled);
    renderTbl.insert("invert",       pp.stylized.invertEnabled);
    renderTbl.insert("posterize",    pp.stylized.posterizeEnabled);
    renderTbl.insert("pixelate",     pp.stylized.pixelateEnabled);
    renderTbl.insert("clarity",      pp.imageQuality.clarityEnabled);
    renderTbl.insert("shadowHighlight", pp.imageQuality.shadowHighlightEnabled);
    renderTbl.insert("colorFilter",  pp.imageQuality.colorFilterEnabled);
    renderTbl.insert("showColliders",render.showColliders);
    renderTbl.insert("showDecalBounds", render.showDecalBounds);
    renderTbl.insert("showSelectionOutline", render.showSelectionOutline);
    renderTbl.insert("passViewerEnabled", render.passViewerEnabled);
    renderTbl.insert("exposure",     (double)pp.exposure);
    renderTbl.insert("bloomIntensity", (double)pp.bloom.intensity);
    renderTbl.insert("bloomThreshold", (double)pp.bloom.threshold);
    renderTbl.insert("bloomSoftKnee", (double)pp.bloom.softKnee);
    renderTbl.insert("fogDensity",   (double)pp.fog.density);
    renderTbl.insert("fogFar",       (double)pp.fog.farDistance);
    renderTbl.insert("fogColor",     std::move(fogColorArr));
    renderTbl.insert("contrast",     (double)pp.colorGrading.contrast);
    renderTbl.insert("saturation",   (double)pp.colorGrading.saturation);
    renderTbl.insert("hueShift",     (double)pp.colorGrading.hueShift);
    renderTbl.insert("temperature",  (double)pp.colorGrading.temperature);
    renderTbl.insert("tint",         (double)pp.colorGrading.tint);
    renderTbl.insert("vignetteIntensity", (double)pp.vignette.intensity);
    renderTbl.insert("vignetteSmoothness", (double)pp.vignette.smoothness);
    renderTbl.insert("vignetteRoundness", (double)pp.vignette.roundness);
    renderTbl.insert("vignetteColor", std::move(vignetteColorArr));
    renderTbl.insert("filmGrainIntensity", (double)pp.filmGrain.intensity);
    renderTbl.insert("filmGrainResponse", (double)pp.filmGrain.response);
    renderTbl.insert("sharpenStrength", (double)pp.sharpen.strength);
    renderTbl.insert("sharpenRadius", (double)pp.sharpen.radius);
    renderTbl.insert("dofFocusDistance", (double)pp.depthOfField.focusDistance);
    renderTbl.insert("dofFocusRange", (double)pp.depthOfField.focusRange);
    renderTbl.insert("dofBlurRadius", (double)pp.depthOfField.blurRadius);
    renderTbl.insert("chromaticAberration", (double)pp.lens.chromaticAberration);
    renderTbl.insert("lensDistortion", (double)pp.lens.distortion);
    renderTbl.insert("sepiaIntensity", (double)pp.stylized.sepiaIntensity);
    renderTbl.insert("invertIntensity", (double)pp.stylized.invertIntensity);
    renderTbl.insert("posterizeLevels", (double)pp.stylized.posterizeLevels);
    renderTbl.insert("pixelSize", (double)pp.stylized.pixelSize);
    renderTbl.insert("clarityStrength", (double)pp.imageQuality.clarityStrength);
    renderTbl.insert("clarityRadius", (double)pp.imageQuality.clarityRadius);
    renderTbl.insert("shadowLift", (double)pp.imageQuality.shadowLift);
    renderTbl.insert("highlightCompression", (double)pp.imageQuality.highlightCompression);
    renderTbl.insert("colorFilterColor", std::move(colorFilterArr));
    renderTbl.insert("colorFilterIntensity", (double)pp.imageQuality.colorFilterIntensity);
    renderTbl.insert("customPostProcesses", std::move(customEffectsArr));
    renderTbl.insert("outlineWidth", (double)render.outlineWidth);
    renderTbl.insert("outlineColor", std::move(outlineColorArr));

    toml::table audioTbl;
    audioTbl.insert("bgmVolume", (double)audio.bgmVolume);
    audioTbl.insert("seVolume",  (double)audio.seVolume);

    toml::table screenTbl;
    screenTbl.insert("width",  (int64_t)screen.width);
    screenTbl.insert("height", (int64_t)screen.height);

    toml::table appTbl;
    appTbl.insert("targetFps", (int64_t)app.targetFps);

    toml::table windowTbl;
    windowTbl.insert("title",      window.title);
    windowTbl.insert("width",      (int64_t)window.width);
    windowTbl.insert("height",     (int64_t)window.height);
    windowTbl.insert("fullscreen", window.fullscreen);

    toml::table projectTbl;
    projectTbl.insert("name", project.name);
    projectTbl.insert("default_scene", project.defaultScene);

    toml::table runtimeTbl;
    runtimeTbl.insert("start_scene", runtime.startScene);

    toml::table uiTbl;
    uiTbl.insert("default_font", ui.defaultFontPath);

    toml::table root;
    root.insert("project", std::move(projectTbl));
    root.insert("runtime", std::move(runtimeTbl));
    root.insert("tags",    std::move(tagTbl));
    root.insert("layers",  std::move(layTbl));
    root.insert("physics", std::move(physicsTbl));
    root.insert("render",  std::move(renderTbl));
    root.insert("audio",   std::move(audioTbl));
    root.insert("screen",  std::move(screenTbl));
    root.insert("app",     std::move(appTbl));
    root.insert("window",  std::move(windowTbl));
    root.insert("ui",      std::move(uiTbl));

    NormalizeTomlFloats(root);

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz
