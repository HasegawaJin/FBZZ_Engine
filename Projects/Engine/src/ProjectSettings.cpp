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
    ps.game.project.defaultScene = "Assets/Scenes/Main.scene";
    ps.game.runtime.startScene   = "Assets/Scenes/Main.scene";
    ps.game.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                     "MainCamera", "Player", "GameController" };
    ps.game.layerNames = {
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
        game.project.name         = (*projectTbl)["name"].value_or(game.project.name);
        game.project.defaultScene = (*projectTbl)["default_scene"].value_or(game.project.defaultScene);
    }

    if (auto* runtimeTbl = tbl["runtime"].as_table()) {
        game.runtime.startScene = (*runtimeTbl)["start_scene"].value_or(game.runtime.startScene);
    }

    if (auto* tagArr = tbl["tags"]["list"].as_array()) {
        game.tags.clear();
        for (auto& elem : *tagArr)
            if (auto v = elem.value<std::string>())
                game.tags.push_back(*v);
    }

    if (auto* layArr = tbl["layers"]["names"].as_array()) {
        for (int i = 0; i < 32 && i < (int)layArr->size(); ++i)
            if (auto v = (*layArr)[i].value<std::string>())
                game.layerNames[i] = *v;
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
        render.particleBudget = (int)(*renderTbl)["particleBudget"].value_or((int64_t)render.particleBudget);
        render.particleBudgetEnabled = (*renderTbl)["particleBudgetEnabled"].value_or(render.particleBudgetEnabled);
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

        // ── Shadow 品質 ─────────────────────────────────────────────────────
        render.shadow.mapResolution = static_cast<uint32_t>(
            (*renderTbl)["shadowResolution"].value_or(static_cast<int64_t>(render.shadow.mapResolution)));
        render.shadow.pcfRadius = static_cast<int>(
            (*renderTbl)["shadowPcfRadius"].value_or(static_cast<int64_t>(render.shadow.pcfRadius)));
        render.shadow.pcssEnabled    = ReadBool(*renderTbl, "pcssEnabled",    render.shadow.pcssEnabled);
        render.shadow.pcssLightRadius = ReadFloat(*renderTbl, "pcssLightRadius", render.shadow.pcssLightRadius);

        // ── IBL ─────────────────────────────────────────────────────────────
        render.ibl.enabled       = ReadBool(*renderTbl,  "iblEnabled",      render.ibl.enabled);
        render.ibl.intensity     = ReadFloat(*renderTbl, "iblIntensity",    render.ibl.intensity);
        render.ibl.diffuseScale  = ReadFloat(*renderTbl, "iblDiffuseScale", render.ibl.diffuseScale);
        render.ibl.specularScale = ReadFloat(*renderTbl, "iblSpecularScale",render.ibl.specularScale);
        render.ibl.maxMipLevel   = static_cast<int>(
            (*renderTbl)["iblMaxMipLevel"].value_or(static_cast<int64_t>(render.ibl.maxMipLevel)));
        render.ibl.irradiancePath = (*renderTbl)["iblIrradiancePath"].value_or(render.ibl.irradiancePath);
        render.ibl.prefilterPath  = (*renderTbl)["iblPrefilterPath"].value_or(render.ibl.prefilterPath);

        // ── SSR ──────────────────────────────────────────────────────────────
        render.ssr.enabled     = ReadBool(*renderTbl,  "ssrEnabled",     render.ssr.enabled);
        render.ssr.intensity   = ReadFloat(*renderTbl, "ssrIntensity",   render.ssr.intensity);
        render.ssr.maxDistance = ReadFloat(*renderTbl, "ssrMaxDistance", render.ssr.maxDistance);
        render.ssr.thickness   = ReadFloat(*renderTbl, "ssrThickness",   render.ssr.thickness);
        render.ssr.steps       = static_cast<int>(
            (*renderTbl)["ssrSteps"].value_or(static_cast<int64_t>(render.ssr.steps)));

        // ── GTAO ─────────────────────────────────────────────────────────────
        render.gtao.enabled       = ReadBool(*renderTbl,  "gtaoEnabled",       render.gtao.enabled);
        render.gtao.intensity     = ReadFloat(*renderTbl, "gtaoIntensity",     render.gtao.intensity);
        render.gtao.radius        = ReadFloat(*renderTbl, "gtaoRadius",        render.gtao.radius);
        render.gtao.slices        = static_cast<int>(
            (*renderTbl)["gtaoSlices"].value_or(static_cast<int64_t>(render.gtao.slices)));
        render.gtao.stepsPerSlice = static_cast<int>(
            (*renderTbl)["gtaoStepsPerSlice"].value_or(static_cast<int64_t>(render.gtao.stepsPerSlice)));

        // ── Contact Shadows ───────────────────────────────────────────────────
        render.contactShadow.enabled   = ReadBool(*renderTbl,  "contactShadowEnabled",   render.contactShadow.enabled);
        render.contactShadow.strength  = ReadFloat(*renderTbl, "contactShadowStrength",  render.contactShadow.strength);
        render.contactShadow.rayLength = ReadFloat(*renderTbl, "contactShadowRayLength", render.contactShadow.rayLength);
        render.contactShadow.thickness = ReadFloat(*renderTbl, "contactShadowThickness", render.contactShadow.thickness);
        render.contactShadow.steps     = static_cast<int>(
            (*renderTbl)["contactShadowSteps"].value_or(static_cast<int64_t>(render.contactShadow.steps)));

        // ── TAA ───────────────────────────────────────────────────────────────
        render.taa.enabled  = ReadBool(*renderTbl,  "taaEnabled",  render.taa.enabled);
        render.taa.feedback = ReadFloat(*renderTbl, "taaFeedback", render.taa.feedback);

        // ── Motion Blur ───────────────────────────────────────────────────────
        render.motionBlur.enabled  = ReadBool(*renderTbl,  "motionBlurEnabled",  render.motionBlur.enabled);
        render.motionBlur.strength = ReadFloat(*renderTbl, "motionBlurStrength", render.motionBlur.strength);
        render.motionBlur.samples  = static_cast<int>(
            (*renderTbl)["motionBlurSamples"].value_or(static_cast<int64_t>(render.motionBlur.samples)));

        // ── Volumetric Light ──────────────────────────────────────────────────
        render.volumetricLight.enabled    = ReadBool(*renderTbl,  "volLightEnabled",    render.volumetricLight.enabled);
        render.volumetricLight.intensity  = ReadFloat(*renderTbl, "volLightIntensity",  render.volumetricLight.intensity);
        render.volumetricLight.scattering = ReadFloat(*renderTbl, "volLightScattering", render.volumetricLight.scattering);
        render.volumetricLight.maxDist    = ReadFloat(*renderTbl, "volLightMaxDist",    render.volumetricLight.maxDist);
        render.volumetricLight.steps      = static_cast<int>(
            (*renderTbl)["volLightSteps"].value_or(static_cast<int64_t>(render.volumetricLight.steps)));

        // ── Lens Flare ────────────────────────────────────────────────────────
        render.lensFlare.enabled    = ReadBool(*renderTbl,  "lensFlareEnabled",    render.lensFlare.enabled);
        render.lensFlare.intensity  = ReadFloat(*renderTbl, "lensFlareIntensity",  render.lensFlare.intensity);
        render.lensFlare.haloWidth  = ReadFloat(*renderTbl, "lensFlareHaloWidth",  render.lensFlare.haloWidth);
        render.lensFlare.distortion = ReadFloat(*renderTbl, "lensFlareDistortion", render.lensFlare.distortion);
        render.lensFlare.ghostCount = static_cast<int>(
            (*renderTbl)["lensFlareGhostCount"].value_or(static_cast<int64_t>(render.lensFlare.ghostCount)));

        // ── LUT Color Grading ─────────────────────────────────────────────────
        render.lutColorGrading.enabled     = ReadBool(*renderTbl,  "lutEnabled",     render.lutColorGrading.enabled);
        render.lutColorGrading.blend       = ReadFloat(*renderTbl, "lutBlend",       render.lutColorGrading.blend);
        render.lutColorGrading.contrast    = ReadFloat(*renderTbl, "lutContrast",    render.lutColorGrading.contrast);
        render.lutColorGrading.saturation  = ReadFloat(*renderTbl, "lutSaturation",  render.lutColorGrading.saturation);
        render.lutColorGrading.hueShift    = ReadFloat(*renderTbl, "lutHueShift",    render.lutColorGrading.hueShift);
        render.lutColorGrading.temperature = ReadFloat(*renderTbl, "lutTemperature", render.lutColorGrading.temperature);
        render.lutColorGrading.tint        = ReadFloat(*renderTbl, "lutTint",        render.lutColorGrading.tint);

        // WHY: TOML から直接設定されても Inspector と同じ排他規則を適用する。
        //      競合時は履歴や Compute 出力を必要としない既存パスを優先する。
        const uint32_t pipelineConflicts = render.NormalizeExclusivePipelineSlots();
        if ((pipelineConflicts & renderer::RenderSettings::PIPELINE_CONFLICT_AA_SLOT) != 0)
            FBZZ_LOG_WARN("ProjectSettings: TAA disabled because FXAA uses the same pipeline slot.");
        if ((pipelineConflicts & renderer::RenderSettings::PIPELINE_CONFLICT_AO_SLOT) != 0)
            FBZZ_LOG_WARN("ProjectSettings: GTAO disabled because SSAO uses the same pipeline slot.");

        // WHY: IBL 用キューブマップが未指定のまま有効化されると、未バインド SRV を読み
        //      環境光が黒になるため、必要な両テクスチャが揃うまで無効として扱う。
        if (render.ibl.enabled && !render.HasValidIblAssets()) {
            FBZZ_LOG_WARN("ProjectSettings: IBL disabled because irradiance/prefilter paths are empty.");
            render.ibl.enabled = false;
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
        if (auto backend = (*appTbl)["renderer"].value<std::string>())
            app.rendererBackend = renderer::BackendFromString(*backend);
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
    for (const auto& t : game.tags)
        tagArr.push_back(t);

    toml::array layArr;
    for (const auto& n : game.layerNames)
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
    renderTbl.insert("particleBudget", (int64_t)render.particleBudget);
    renderTbl.insert("particleBudgetEnabled", render.particleBudgetEnabled);
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

    // ── Shadow 品質 ─────────────────────────────────────────────────────────
    renderTbl.insert("shadowResolution",  (int64_t)render.shadow.mapResolution);
    renderTbl.insert("shadowPcfRadius",   (int64_t)render.shadow.pcfRadius);
    renderTbl.insert("pcssEnabled",       render.shadow.pcssEnabled);
    renderTbl.insert("pcssLightRadius",   (double)render.shadow.pcssLightRadius);

    // ── IBL ─────────────────────────────────────────────────────────────────
    renderTbl.insert("iblEnabled",        render.ibl.enabled);
    renderTbl.insert("iblIntensity",      (double)render.ibl.intensity);
    renderTbl.insert("iblDiffuseScale",   (double)render.ibl.diffuseScale);
    renderTbl.insert("iblSpecularScale",  (double)render.ibl.specularScale);
    renderTbl.insert("iblMaxMipLevel",    (int64_t)render.ibl.maxMipLevel);
    renderTbl.insert("iblIrradiancePath", render.ibl.irradiancePath);
    renderTbl.insert("iblPrefilterPath",  render.ibl.prefilterPath);

    // ── SSR ──────────────────────────────────────────────────────────────────
    renderTbl.insert("ssrEnabled",        render.ssr.enabled);
    renderTbl.insert("ssrIntensity",      (double)render.ssr.intensity);
    renderTbl.insert("ssrMaxDistance",    (double)render.ssr.maxDistance);
    renderTbl.insert("ssrThickness",      (double)render.ssr.thickness);
    renderTbl.insert("ssrSteps",          (int64_t)render.ssr.steps);

    // ── GTAO ─────────────────────────────────────────────────────────────────
    renderTbl.insert("gtaoEnabled",       render.gtao.enabled);
    renderTbl.insert("gtaoIntensity",     (double)render.gtao.intensity);
    renderTbl.insert("gtaoRadius",        (double)render.gtao.radius);
    renderTbl.insert("gtaoSlices",        (int64_t)render.gtao.slices);
    renderTbl.insert("gtaoStepsPerSlice", (int64_t)render.gtao.stepsPerSlice);

    // ── Contact Shadows ───────────────────────────────────────────────────────
    renderTbl.insert("contactShadowEnabled",   render.contactShadow.enabled);
    renderTbl.insert("contactShadowStrength",  (double)render.contactShadow.strength);
    renderTbl.insert("contactShadowRayLength", (double)render.contactShadow.rayLength);
    renderTbl.insert("contactShadowThickness", (double)render.contactShadow.thickness);
    renderTbl.insert("contactShadowSteps",     (int64_t)render.contactShadow.steps);

    // ── TAA ───────────────────────────────────────────────────────────────────
    renderTbl.insert("taaEnabled",        render.taa.enabled);
    renderTbl.insert("taaFeedback",       (double)render.taa.feedback);

    // ── Motion Blur ───────────────────────────────────────────────────────────
    renderTbl.insert("motionBlurEnabled",  render.motionBlur.enabled);
    renderTbl.insert("motionBlurStrength", (double)render.motionBlur.strength);
    renderTbl.insert("motionBlurSamples",  (int64_t)render.motionBlur.samples);

    // ── Volumetric Light ──────────────────────────────────────────────────────
    renderTbl.insert("volLightEnabled",    render.volumetricLight.enabled);
    renderTbl.insert("volLightIntensity",  (double)render.volumetricLight.intensity);
    renderTbl.insert("volLightScattering", (double)render.volumetricLight.scattering);
    renderTbl.insert("volLightMaxDist",    (double)render.volumetricLight.maxDist);
    renderTbl.insert("volLightSteps",      (int64_t)render.volumetricLight.steps);

    // ── Lens Flare ────────────────────────────────────────────────────────────
    renderTbl.insert("lensFlareEnabled",    render.lensFlare.enabled);
    renderTbl.insert("lensFlareIntensity",  (double)render.lensFlare.intensity);
    renderTbl.insert("lensFlareHaloWidth",  (double)render.lensFlare.haloWidth);
    renderTbl.insert("lensFlareDistortion", (double)render.lensFlare.distortion);
    renderTbl.insert("lensFlareGhostCount", (int64_t)render.lensFlare.ghostCount);

    // ── LUT Color Grading ─────────────────────────────────────────────────────
    renderTbl.insert("lutEnabled",     render.lutColorGrading.enabled);
    renderTbl.insert("lutBlend",       (double)render.lutColorGrading.blend);
    renderTbl.insert("lutContrast",    (double)render.lutColorGrading.contrast);
    renderTbl.insert("lutSaturation",  (double)render.lutColorGrading.saturation);
    renderTbl.insert("lutHueShift",    (double)render.lutColorGrading.hueShift);
    renderTbl.insert("lutTemperature", (double)render.lutColorGrading.temperature);
    renderTbl.insert("lutTint",        (double)render.lutColorGrading.tint);

    toml::table audioTbl;
    audioTbl.insert("bgmVolume", (double)audio.bgmVolume);
    audioTbl.insert("seVolume",  (double)audio.seVolume);

    toml::table screenTbl;
    screenTbl.insert("width",  (int64_t)screen.width);
    screenTbl.insert("height", (int64_t)screen.height);

    toml::table appTbl;
    appTbl.insert("targetFps", (int64_t)app.targetFps);
    appTbl.insert("renderer", renderer::ToString(app.rendererBackend));

    toml::table windowTbl;
    windowTbl.insert("title",      window.title);
    windowTbl.insert("width",      (int64_t)window.width);
    windowTbl.insert("height",     (int64_t)window.height);
    windowTbl.insert("fullscreen", window.fullscreen);

    toml::table projectTbl;
    projectTbl.insert("name", game.project.name);
    projectTbl.insert("default_scene", game.project.defaultScene);

    toml::table runtimeTbl;
    runtimeTbl.insert("start_scene", game.runtime.startScene);

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
