/// @file    VolumeFlipbookOperators.cpp
/// @brief   解析ボリュームの設定・再生・ベイク・材質適用を直接操作する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Engine/Asset/VolumeFlipbookSources.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <type_traits>

namespace fbzz::editor {
namespace {
template<class Field>
void VisitSettings(asset::VolumeFlipbookBakeSettings& s, Field&& field)
{
    field("source.frameCount", s.source.frameCount, 2, 256);
    field("columns", s.columns, 0, 64);
    field("tileSize", s.tileSize, 128, 512);
    field("volumeResolution", s.volumeResolution, 32, 160);
    field("source.startTime", s.source.startTime, -1, 30);
    field("source.frameDt", s.source.frameDt, 0.008333333333333333, 0.25);
    field("source.loop", s.source.loop);
    field("noise.frequency", s.noise.frequency, 0.25, 8);
    field("noise.amplitude", s.noise.amplitude, 0, 1);
    field("cameraYawDegrees", s.cameraYawDegrees, -180, 180);
    field("halfExtent", s.halfExtent, 0.3, 2);
    field("lightYawDegrees", s.lightYawDegrees, -180, 180);
    field("lightPitchDegrees", s.lightPitchDegrees, -10, 90);
    field("extinction", s.extinction, 0, 100);
    field("anisotropy", s.anisotropy, -0.9, 0.9);
    field("emissionIntensity", s.emissionIntensity, 0, 100);
    field("exposure", s.exposure, 0.01, 8);
    field("raySteps", s.raySteps, 16, 512);
    field("shadowSteps", s.shadowSteps, 1, 64);
    field("sixWayLightmaps", s.sixWayLightmaps);
    field("scatteringOctaves", s.scatteringOctaves, 1, 6);
    field("skyOcclusion", s.skyOcclusion, 0, 1);
    field("detailStrength", s.detailStrength, 0, 1);
    field("detailScale", s.detailScale, 1, 32);
    field("detailPeriod", s.detailPeriod, 0.1, 4);
    field("supersampling", s.supersampling, 1, 3);
    field("blackbodyEmission", s.blackbodyEmission);
    field("blackbodyMinKelvin", s.blackbodyMinKelvin, 500, 15000);
    field("blackbodyMaxKelvin", s.blackbodyMaxKelvin, 500, 15000);
    field("liquid.threshold", s.liquid.threshold, 0.05, 1.5);
    field("liquid.softness", s.liquid.softness, 0.005, 0.5);
    field("liquid.extinction", s.liquid.extinction, 0, 200);
    field("liquid.specular", s.liquid.specular, 0, 4);
    field("liquid.gloss", s.liquid.gloss, 1, 512);
    field("liquid.fresnelF0", s.liquid.fresnelF0, 0, 0.2);
    field("source.emitter.burst", s.source.emitter.burst);
    field("source.emitter.count", s.source.emitter.count, 1, 512);
    field("source.emitter.originRadius", s.source.emitter.originRadius, 0, 0.5);
    field("source.emitter.coneAngleDegrees", s.source.emitter.coneAngleDegrees, 0, 180);
    field("source.emitter.speed", s.source.emitter.speed, 0, 5);
    field("source.emitter.speedRandom", s.source.emitter.speedRandom, 0, 1);
    field("source.emitter.drag", s.source.emitter.drag, 0, 10);
    field("source.emitter.lifetime", s.source.emitter.lifetime, 0.05, 10);
    field("source.emitter.lifetimeRandom", s.source.emitter.lifetimeRandom, 0, 1);
    field("source.emitter.fadeIn", s.source.emitter.fadeIn, 0, 5);
    field("source.emitter.fadeOut", s.source.emitter.fadeOut, 0, 5);
    field("source.emitter.radius", s.source.emitter.radius, 0.005, 0.5);
    field("source.emitter.radiusRandom", s.source.emitter.radiusRandom, 0, 1);
    field("source.emitter.growth", s.source.emitter.growth, 0.1, 5);
    field("source.emitter.spin", s.source.emitter.spin, 0, 20);
    field("source.emitter.density", s.source.emitter.density, 0, 5);
    field("source.emitter.temperature", s.source.emitter.temperature, 0, 1);
    field("source.emitter.coolingTime", s.source.emitter.coolingTime, 0.01, 10);
    field("source.emitter.liquid", s.source.emitter.liquid, 0, 1);
    field("source.emitter.noiseScale", s.source.emitter.noiseScale, 0, 1);
    field("source.emitter.stretch", s.source.emitter.stretch, 1, 6);
    field("source.emitter.stretchPerSpeed", s.source.emitter.stretchPerSpeed, 0, 5);
    field("source.emitter.colorKeyMin", s.source.emitter.colorKeyMin, 0, 1);
    field("source.emitter.colorKeyMax", s.source.emitter.colorKeyMax, 0, 1);
    field("source.emitter.origin.x", s.source.emitter.origin.x, -1, 1);
    field("source.emitter.origin.y", s.source.emitter.origin.y, -1, 1);
    field("source.emitter.origin.z", s.source.emitter.origin.z, -1, 1);
    field("source.emitter.direction.x", s.source.emitter.direction.x, -1, 1);
    field("source.emitter.direction.y", s.source.emitter.direction.y, -1, 1);
    field("source.emitter.direction.z", s.source.emitter.direction.z, -1, 1);
    field("source.emitter.gravity.x", s.source.emitter.gravity.x, -10, 10);
    field("source.emitter.gravity.y", s.source.emitter.gravity.y, -10, 10);
    field("source.emitter.gravity.z", s.source.emitter.gravity.z, -10, 10);
    field("lightColor.x", s.lightColor.x, 0, 100000);
    field("lightColor.y", s.lightColor.y, 0, 100000);
    field("lightColor.z", s.lightColor.z, 0, 100000);
    field("ambient.x", s.ambient.x, 0, 100000);
    field("ambient.y", s.ambient.y, 0, 100000);
    field("ambient.z", s.ambient.z, 0, 100000);
    field("albedoRamp.stops[0].position", s.albedoRamp.stops[0].position, 0, 1);
    field("albedoRamp.stops[0].color.x", s.albedoRamp.stops[0].color.x, 0, 100000);
    field("albedoRamp.stops[0].color.y", s.albedoRamp.stops[0].color.y, 0, 100000);
    field("albedoRamp.stops[0].color.z", s.albedoRamp.stops[0].color.z, 0, 100000);
    field("albedoRamp.stops[1].position", s.albedoRamp.stops[1].position, 0, 1);
    field("albedoRamp.stops[1].color.x", s.albedoRamp.stops[1].color.x, 0, 100000);
    field("albedoRamp.stops[1].color.y", s.albedoRamp.stops[1].color.y, 0, 100000);
    field("albedoRamp.stops[1].color.z", s.albedoRamp.stops[1].color.z, 0, 100000);
    field("albedoRamp.stops[2].position", s.albedoRamp.stops[2].position, 0, 1);
    field("albedoRamp.stops[2].color.x", s.albedoRamp.stops[2].color.x, 0, 100000);
    field("albedoRamp.stops[2].color.y", s.albedoRamp.stops[2].color.y, 0, 100000);
    field("albedoRamp.stops[2].color.z", s.albedoRamp.stops[2].color.z, 0, 100000);
    field("albedoRamp.stops[3].position", s.albedoRamp.stops[3].position, 0, 1);
    field("albedoRamp.stops[3].color.x", s.albedoRamp.stops[3].color.x, 0, 100000);
    field("albedoRamp.stops[3].color.y", s.albedoRamp.stops[3].color.y, 0, 100000);
    field("albedoRamp.stops[3].color.z", s.albedoRamp.stops[3].color.z, 0, 100000);
    field("emissionRamp.stops[0].position", s.emissionRamp.stops[0].position, 0, 1);
    field("emissionRamp.stops[0].color.x", s.emissionRamp.stops[0].color.x, 0, 100000);
    field("emissionRamp.stops[0].color.y", s.emissionRamp.stops[0].color.y, 0, 100000);
    field("emissionRamp.stops[0].color.z", s.emissionRamp.stops[0].color.z, 0, 100000);
    field("emissionRamp.stops[1].position", s.emissionRamp.stops[1].position, 0, 1);
    field("emissionRamp.stops[1].color.x", s.emissionRamp.stops[1].color.x, 0, 100000);
    field("emissionRamp.stops[1].color.y", s.emissionRamp.stops[1].color.y, 0, 100000);
    field("emissionRamp.stops[1].color.z", s.emissionRamp.stops[1].color.z, 0, 100000);
    field("emissionRamp.stops[2].position", s.emissionRamp.stops[2].position, 0, 1);
    field("emissionRamp.stops[2].color.x", s.emissionRamp.stops[2].color.x, 0, 100000);
    field("emissionRamp.stops[2].color.y", s.emissionRamp.stops[2].color.y, 0, 100000);
    field("emissionRamp.stops[2].color.z", s.emissionRamp.stops[2].color.z, 0, 100000);
    field("emissionRamp.stops[3].position", s.emissionRamp.stops[3].position, 0, 1);
    field("emissionRamp.stops[3].color.x", s.emissionRamp.stops[3].color.x, 0, 100000);
    field("emissionRamp.stops[3].color.y", s.emissionRamp.stops[3].color.y, 0, 100000);
    field("emissionRamp.stops[3].color.z", s.emissionRamp.stops[3].color.z, 0, 100000);
    field("source.seed", s.source.seed, 0, 1000000);
}
bool Number(const OpData& value, double& out)
{
    if (value.IsInt()) out = static_cast<double>(value.AsInt64());
    else if (value.IsFloat()) out = value.AsDouble();
    else return false;
    return std::isfinite(out);
}
bool Validate(const asset::VolumeFlipbookBakeSettings& s)
{
    const auto resolution = s.volumeResolution;
    if (resolution != 32 && resolution != 48 && resolution != 64 && resolution != 96 && resolution != 128 && resolution != 160) return false;
    if (s.tileSize != 128 && s.tileSize != 256 && s.tileSize != 512) return false;
    if (s.blackbodyMinKelvin > s.blackbodyMaxKelvin || s.source.emitter.colorKeyMin > s.source.emitter.colorKeyMax) return false;
    for (const auto* ramp : {&s.albedoRamp, &s.emissionRamp})
        for (std::size_t i = 1; i < ramp->stops.size(); ++i)
            if (ramp->stops[i].position < ramp->stops[i - 1].position) return false;
    return asset::FindVolumeSource(s.source.preset) && (!s.source.loop || asset::VolumeSourceCanLoop(s.source));
}
} /// @note namespace
void VolumeFlipbookBakePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "reset_look", "control", "bake", "cancel", "apply", "preview_vfx"}) {
        EditorOperator op; const std::string name = action;
        op.id = "volume.flipbook." + name; op.label = op.id; op.category = "Volume Flipbook";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "解析puffベイクの直接操作。configureはinspect.settingsの平坦なキーを部分指定する。範囲はschema。bakeのjobはfluid.jobStatusでも追跡できる。controlはtime/playing/speed/enabledと比較再生を指定する。";
        if (name == "configure" || name == "control") { OpParam p; p.name = "settings"; p.type = OpParamType::Data; op.params.push_back(std::move(p)); }
        if (name == "apply" || name == "preview_vfx") { OpParam p; p.name = "material"; p.type = OpParamType::String; op.params.push_back(std::move(p)); }
        op.poll = [name](const OpContext& context, const OpArgs&) { return name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            if (name == "configure" || name == "reset_look") {
                if (JobActive(ctx)) return OpResult::Err("VOLUME_BUSY", "自分のベイクの完了を待ってください。");
                auto next = m_settings; std::string base = m_baseName.data();
                const OpData* patch = args.GetData("settings");
                if (name == "configure" && (!patch || !patch->IsObject())) return OpResult::Err("BAD_ARG", "settingsはオブジェクトです。");
                std::size_t consumed = 0; bool valid = true;
                if (name == "reset_look") asset::ApplyVolumeSourceLook(next);
                if (patch) {
                    if (const auto* preset = patch->Find("source.preset")) {
                        if (!preset->IsString() || !asset::FindVolumeSource(preset->AsString())) return OpResult::Err("BAD_ARG", "source.preset");
                        if (next.source.preset != preset->AsString()) { next.source.preset = preset->AsString(); asset::ApplyVolumeSourceLook(next); }
                        ++consumed;
                    }
                    if (const auto* value = patch->Find("baseName")) {
                        if (!value->IsString()) return OpResult::Err("BAD_ARG", "baseName");
                        base = value->AsString(); ++consumed;
                    }
                    VisitSettings(next, [&](const char* key, auto& value, double lower = 0, double upper = 1) {
                        const auto* data = patch->Find(key); if (!data) return; ++consumed;
                        using T = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, bool>) { if (!data->IsBool()) valid = false; else value = data->AsBool(); }
                        else { double number = 0;
                            if (!Number(*data, number) || number < lower || number > upper || (std::is_integral_v<T> && std::floor(number) != number)) valid = false;
                            else value = static_cast<T>(number);
                        }
                    });
                    valid = valid && consumed == patch->AsObject().size();
                }
                if (!valid || !Validate(next) || base.empty() || base.size() >= m_baseName.size()
                    || base.find_first_of("/\\:*?\"<>|\0", 0, 10) != std::string::npos || base == "." || base == "..")
                    return OpResult::Err("BAD_ARG", "設定値・範囲・ランプの順序・出力名を確認してください。");
                m_settings = std::move(next); std::snprintf(m_baseName.data(), m_baseName.size(), "%s", base.c_str()); MarkSettingsChanged();
            } else if (name == "control") {
                const OpData* patch = args.GetData("settings"); if (!patch || !patch->IsObject()) return OpResult::Err("BAD_ARG", "settings");
                auto time = m_previewTime, speed = m_playSpeed, compareTime = m_compareTime, fps = m_compareFps, strength = m_strengthScale;
                bool enabled = m_apiPreview, playing = m_playing, comparePlaying = m_comparePlaying, arrow = m_showLightArrow;
                int view = static_cast<int>(m_view), background = static_cast<int>(m_background), size = m_previewSize;
                for (const auto& [key, value] : patch->AsObject()) {
                    if (key == "enabled" || key == "playing" || key == "comparePlaying" || key == "showLightArrow") {
                        if (!value.IsBool()) return OpResult::Err("BAD_ARG", key);
                        if (key == "enabled") enabled = value.AsBool(); else if (key == "playing") playing = value.AsBool();
                        else if (key == "comparePlaying") comparePlaying = value.AsBool(); else arrow = value.AsBool();
                    } else { double number = 0; if (!Number(value, number)) return OpResult::Err("BAD_ARG", key);
                        if (key == "time" && number >= 0 && number <= BakeDuration()) time = static_cast<float>(number);
                        else if (key == "speed" && number >= 0.05 && number <= 4) speed = static_cast<float>(number);
                        else if (key == "compareTime" && number >= 0 && number <= 3600) compareTime = static_cast<float>(number);
                        else if (key == "compareFps" && number >= 0.1 && number <= 120) fps = static_cast<float>(number);
                        else if (key == "strengthScale" && number >= 0 && number <= 4) strength = static_cast<float>(number);
                        else if (key == "view" && number >= 0 && number <= 4 && std::floor(number) == number) view = static_cast<int>(number);
                        else if (key == "background" && number >= 0 && number <= 2 && std::floor(number) == number) background = static_cast<int>(number);
                        else if (key == "previewSize" && number >= 0 && number <= 3 && std::floor(number) == number) size = static_cast<int>(number);
                        else return OpResult::Err("BAD_ARG", key);
                    }
                }
                m_apiPreview = enabled; m_previewTime = time; m_playSpeed = speed; m_playing = playing;
                m_compareTime = compareTime; m_comparePlaying = comparePlaying; m_compareFps = fps; m_strengthScale = strength;
                m_view = static_cast<VolumeView>(view); m_background = static_cast<asset::VolumePreviewBackground>(background); m_previewSize = size; m_showLightArrow = arrow; m_previewDirty = true;
            } else if (name == "bake") {
                if (!ctx.fluidBake || !ctx.renderer || !ctx.resources || ctx.projectRoot.empty()) return OpResult::Err("NO_RENDERER", "描画デバイスとプロジェクトが必要です。");
                if (JobActive(ctx)) return OpResult::Err("VOLUME_BUSY", "ベイクは既に実行中です。");
                StartBake(ctx); if (m_statusIsError || m_jobId == 0) return OpResult::Err("VOLUME_BAKE_FAILED", m_status);
                m_apiJobId = m_jobId;
            } else if (name == "cancel") {
                if (!ctx.fluidBake || !ctx.fluidBake->Cancel(m_jobId)) return OpResult::Err("NO_JOB", "実行中のベイクがありません。");
            } else if (name == "apply" || name == "preview_vfx") {
                if (JobActive(ctx)) return OpResult::Err("VOLUME_BUSY", "ベイクの完了を待ってください。");
                if (name == "apply" && !m_lastResult.success) return OpResult::Err("NO_BAKE", "成功したベイク結果がありません。");
                if (name == "apply") {
                    std::error_code error;
                    const auto output = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(m_lastBakeSettings.outputDirectory), error);
                    const auto assets = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
                    const auto relative = output.lexically_relative(assets);
                    if (ctx.projectRoot.empty() || error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return OpResult::Err("NO_BAKE", "現在のプロジェクトでベイクしてください。");
                }
                auto requested = args.GetString("material");
                if (requested.starts_with("guid:")) requested = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(requested));
                auto file = util::FileSystem::PathFromUtf8(requested);
                if (file.is_relative()) file = util::FileSystem::PathFromUtf8(ctx.projectRoot) / file;
                std::error_code error; file = std::filesystem::weakly_canonical(file, error);
                const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
                const auto relative = file.lexically_relative(root);
                if (ctx.projectRoot.empty() || requested.empty() || error || file.extension() != ".mat" || relative.empty() || relative.is_absolute() || *relative.begin() == ".."
                    || !std::filesystem::is_regular_file(file, error)) return OpResult::Err("BAD_PATH", "Assets内の.matを指定してください。");
                m_materialPath = util::FileSystem::PathToUtf8(file);
                if (name == "apply") ApplyToMaterial(ctx); else OpenInPrefabPreview(ctx);
                if (m_statusIsError) return OpResult::Err("VOLUME_APPLY_FAILED", m_status);
            }
            OpData data = OpData::MakeObject(), settings = OpData::MakeObject(), schema = OpData::MakeObject();
            VisitSettings(m_settings, [&](const char* key, auto& value, double lower = 0, double upper = 1) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool>) settings.Set(key, OpData(value));
                else if constexpr (std::is_integral_v<T>) settings.Set(key, OpData(static_cast<std::int64_t>(value)));
                else settings.Set(key, OpData(value));
                OpData range = OpData::MakeObject(); range.Set("min", OpData(lower)); range.Set("max", OpData(upper)); schema.Set(key, std::move(range));
            });
            settings.Set("source.preset", OpData(m_settings.source.preset)); settings.Set("baseName", OpData(std::string(m_baseName.data())));
            OpData presets = OpData::MakeArray(); for (const auto& preset : asset::VolumeSources()) presets.Push(OpData(preset.name));
            data.Set("settings", std::move(settings)); data.Set("schema", std::move(schema)); data.Set("presets", std::move(presets));
            data.Set("job", OpData(m_jobId != 0 ? m_jobId : m_apiJobId)); data.Set("busy", OpData(JobActive(ctx)));
            data.Set("message", OpData(m_status)); data.Set("error", OpData(m_statusIsError)); data.Set("success", OpData(m_lastResult.success));
            data.Set("colorPath", OpData(m_lastResult.colorPath)); data.Set("motionPath", OpData(m_lastResult.motionPath));
            OpData playback = OpData::MakeObject(); playback.Set("enabled", OpData(m_apiPreview)); playback.Set("time", OpData(m_previewTime));
            playback.Set("playing", OpData(m_playing)); playback.Set("speed", OpData(m_playSpeed)); playback.Set("compareTime", OpData(m_compareTime));
            playback.Set("comparePlaying", OpData(m_comparePlaying)); playback.Set("compareFps", OpData(m_compareFps)); playback.Set("strengthScale", OpData(m_strengthScale));
            playback.Set("view", OpData(static_cast<int>(m_view))); playback.Set("background", OpData(static_cast<int>(m_background)));
            playback.Set("previewSize", OpData(m_previewSize)); playback.Set("showLightArrow", OpData(m_showLightArrow)); data.Set("playback", std::move(playback));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
