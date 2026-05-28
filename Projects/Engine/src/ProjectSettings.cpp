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

} // namespace

ProjectSettings ProjectSettings::Default()
{
    ProjectSettings ps;
    ps.project.defaultScene = "Assets/Scenes/Main.fbzz";
    ps.runtime.startScene   = "Assets/Scenes/Main.fbzz";
    ps.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                "MainCamera", "Player", "GameController" };
    ps.layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
    ps.physics.hz       = 60;
    ps.physics.substeps = 4;
    ps.physics.gravity  = { 0.0f, -9.81f, 0.0f };
    return ps;
}

bool ProjectSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        *this = Default();
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("ProjectSettings: parse failed: %s", path.c_str());
        *this = Default();
        return false;
    }
    auto& tbl = result.table();

    *this = Default();

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
        render.wireframeMode = (*renderTbl)["wireframe"].value_or(render.wireframeMode);
        render.shadowEnabled = (*renderTbl)["shadow"].value_or(render.shadowEnabled);
        pp.bloom.enabled = (*renderTbl)["bloom"].value_or(pp.bloom.enabled);
        pp.fxaaEnabled = (*renderTbl)["fxaa"].value_or(pp.fxaaEnabled);
        pp.fog.enabled = (*renderTbl)["fog"].value_or(pp.fog.enabled);
        pp.colorGrading.enabled = (*renderTbl)["colorGrading"].value_or(pp.colorGrading.enabled);
        pp.vignette.enabled = (*renderTbl)["vignette"].value_or(pp.vignette.enabled);
        pp.filmGrain.enabled = (*renderTbl)["filmGrain"].value_or(pp.filmGrain.enabled);
        pp.lens.chromaticAberrationEnabled = (*renderTbl)["chromaticAberrationEnabled"].value_or(pp.lens.chromaticAberrationEnabled);
        pp.lens.distortionEnabled = (*renderTbl)["lensDistortionEnabled"].value_or(pp.lens.distortionEnabled);
        render.showColliders = (*renderTbl)["showColliders"].value_or(render.showColliders);
        render.showDecalBounds = (*renderTbl)["showDecalBounds"].value_or(render.showDecalBounds);
        render.showSelectionOutline = (*renderTbl)["showSelectionOutline"].value_or(render.showSelectionOutline);
        render.passViewerEnabled = (*renderTbl)["passViewerEnabled"].value_or(render.passViewerEnabled);
        pp.exposure = (float)(*renderTbl)["exposure"].value_or((double)pp.exposure);
        pp.bloom.intensity = (float)(*renderTbl)["bloomIntensity"].value_or((double)pp.bloom.intensity);
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
        pp.lens.chromaticAberration = (float)(*renderTbl)["chromaticAberration"].value_or((double)pp.lens.chromaticAberration);
        pp.lens.distortion = (float)(*renderTbl)["lensDistortion"].value_or((double)pp.lens.distortion);
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
    renderTbl.insert("wireframe",    render.wireframeMode);
    renderTbl.insert("shadow",       render.shadowEnabled);
    renderTbl.insert("bloom",        pp.bloom.enabled);
    renderTbl.insert("fxaa",         pp.fxaaEnabled);
    renderTbl.insert("fog",          pp.fog.enabled);
    renderTbl.insert("colorGrading", pp.colorGrading.enabled);
    renderTbl.insert("vignette",     pp.vignette.enabled);
    renderTbl.insert("filmGrain",    pp.filmGrain.enabled);
    renderTbl.insert("chromaticAberrationEnabled", pp.lens.chromaticAberrationEnabled);
    renderTbl.insert("lensDistortionEnabled", pp.lens.distortionEnabled);
    renderTbl.insert("showColliders",render.showColliders);
    renderTbl.insert("showDecalBounds", render.showDecalBounds);
    renderTbl.insert("showSelectionOutline", render.showSelectionOutline);
    renderTbl.insert("passViewerEnabled", render.passViewerEnabled);
    renderTbl.insert("exposure",     (double)pp.exposure);
    renderTbl.insert("bloomIntensity", (double)pp.bloom.intensity);
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
    renderTbl.insert("chromaticAberration", (double)pp.lens.chromaticAberration);
    renderTbl.insert("lensDistortion", (double)pp.lens.distortion);
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

    toml::table projectTbl;
    projectTbl.insert("name", project.name);
    projectTbl.insert("default_scene", project.defaultScene);

    toml::table runtimeTbl;
    runtimeTbl.insert("start_scene", runtime.startScene);

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

    NormalizeTomlFloats(root);

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz
