/// @file    RenderPipelineAsset.cpp
/// @brief   Rendering configuration ownership, typed serialization and fallback resolution.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "RenderPipelineAssetCodec.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace fbzz::asset {
namespace {

void CopyOwned(const renderer::RenderSettings& source, renderer::RenderSettings& target)
{
    target.pipeline = source.pipeline;
    target.modeRequest = source.modeRequest;
    target.hybridQuality = source.hybridQuality;
    target.shadowEnabled = source.shadowEnabled;
    const bool cascadeDebug = target.shadow.debugVisualizeCascades;
    target.shadow = source.shadow;
    target.shadow.debugVisualizeCascades = cascadeDebug;
    target.gpuInstancing = source.gpuInstancing;
    target.asyncCompute = source.asyncCompute;
    target.clustered.enabled = source.clustered.enabled;
    target.clustered.maxDistance = source.clustered.maxDistance;
    target.particleBudget = source.particleBudget;
    target.particleBudgetEnabled = source.particleBudgetEnabled;
    target.froxelFog.gridX = source.froxelFog.gridX;
    target.froxelFog.gridY = source.froxelFog.gridY;
    target.froxelFog.gridZ = source.froxelFog.gridZ;
    target.schedulePolicy = source.schedulePolicy;
    target.passOverrides = source.passOverrides;
}

bool Valid(const renderer::RenderSettings& value)
{
    if (!renderer::IsHybridQualityValid(value.hybridQuality)) return false;
    const auto& shadow = value.shadow;
    if (static_cast<unsigned>(value.pipeline) > static_cast<unsigned>(renderer::RenderingPipeline::DeferredPlus)
        || static_cast<unsigned>(value.modeRequest.mode) > static_cast<unsigned>(renderer::RenderMode::PATH_TRACING)
        || static_cast<unsigned>(value.modeRequest.pathProfile) > static_cast<unsigned>(renderer::PathTracingProfile::GAME)
        || static_cast<unsigned>(value.schedulePolicy) > static_cast<unsigned>(renderer::RenderGraphSchedulePolicy::MinimizeLifetimes)) return false;
    if (!shadow.mapResolution || shadow.mapResolution > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || !shadow.punctualMapResolution || shadow.punctualMapResolution > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || shadow.cascadeCount < 1 || shadow.cascadeCount > renderer::kMaxShadowCascades
        || shadow.pcfRadius < 0 || shadow.punctualPcfRadius < 0 || shadow.maxShadowedPointLights < 0) return false;
    if (!std::isfinite(shadow.cascadeSplitLambda) || shadow.cascadeSplitLambda < 0 || shadow.cascadeSplitLambda > 1
        || !std::isfinite(shadow.cascadeBlend) || shadow.cascadeBlend < 0 || shadow.cascadeBlend > 1
        || !std::isfinite(shadow.autoFitDistance) || shadow.autoFitDistance < 0
        || !std::isfinite(shadow.pcssLightRadius) || shadow.pcssLightRadius < 0
        || !std::isfinite(shadow.punctualShadowDistance) || shadow.punctualShadowDistance < 0
        || !std::isfinite(value.clustered.maxDistance) || value.clustered.maxDistance <= 0) return false;
    for (uint32_t dimension : {value.froxelFog.gridX, value.froxelFog.gridY, value.froxelFog.gridZ})
        if (!dimension || dimension > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;
    for (const auto& entry : value.passOverrides) {
        if (entry.name.empty()) return false;
        for (const auto& read : entry.extraReads) if (read.empty()) return false;
    }
    return true;
}

const toml::table* ReadTable(const toml::table& root, const char* key, bool& valid)
{
    const auto* node = root.get(key);
    if (!node) return nullptr;
    const auto* table = node->as_table();
    if (!table) valid = false;
    return table;
}

bool Read(const toml::table& table, const char* key, bool& out)
{
    const auto* node = table.get(key);
    if (!node) return true;
    const auto* value = node->as_boolean();
    if (!value) return false;
    out = value->get();
    return true;
}

bool Read(const toml::table& table, const char* key, int& out)
{
    const auto* node = table.get(key);
    if (!node) return true;
    const auto* value = node->as_integer();
    if (!value || value->get() < std::numeric_limits<int>::min() || value->get() > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(value->get());
    return true;
}

bool Read(const toml::table& table, const char* key, uint32_t& out)
{
    const auto* node = table.get(key);
    if (!node) return true;
    const auto* value = node->as_integer();
    if (!value || value->get() < 0 || value->get() > std::numeric_limits<int>::max()) return false;
    out = static_cast<uint32_t>(value->get());
    return true;
}

bool Read(const toml::table& table, const char* key, float& out)
{
    const auto* node = table.get(key);
    if (!node) return true;
    double value = 0;
    if (const auto* number = node->as_floating_point()) value = number->get();
    else if (const auto* integer = node->as_integer()) value = static_cast<double>(integer->get());
    else return false;
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
    out = static_cast<float>(value);
    return true;
}

bool Read(const toml::table& table, const char* key, std::string& out)
{
    const auto* node = table.get(key);
    if (!node) return true;
    const auto* value = node->as_string();
    if (!value) return false;
    out = value->get();
    return true;
}

bool ReadRequest(const toml::table& table, renderer::RenderSettings& value)
{
    std::string pipeline = "Forward", mode = "Raster", profile = "Reference";
    if (!Read(table, "pipeline", pipeline) || !Read(table, "mode", mode) || !Read(table, "pathProfile", profile)
        || !Read(table, "rayShadow", value.modeRequest.rayShadow)
        || !Read(table, "rayReflection", value.modeRequest.rayReflection)
        || !Read(table, "rayDiffuseGi", value.modeRequest.rayDiffuseGi)
        || !Read(table, "gpuInstancing", value.gpuInstancing) || !Read(table, "asyncCompute", value.asyncCompute)) return false;
    if (pipeline == "Forward") value.pipeline = renderer::RenderingPipeline::Forward;
    else if (pipeline == "Deferred") value.pipeline = renderer::RenderingPipeline::Deferred;
    else if (pipeline == "Forward+") value.pipeline = renderer::RenderingPipeline::ForwardPlus;
    else if (pipeline == "Deferred+") value.pipeline = renderer::RenderingPipeline::DeferredPlus;
    else return false;
    if (mode == "Raster") value.modeRequest.mode = renderer::RenderMode::RASTER;
    else if (mode == "Hybrid") value.modeRequest.mode = renderer::RenderMode::HYBRID;
    else if (mode == "PathTracing") value.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    else return false;
    if (profile == "Reference") value.modeRequest.pathProfile = renderer::PathTracingProfile::REFERENCE;
    else if (profile == "Game") value.modeRequest.pathProfile = renderer::PathTracingProfile::GAME;
    else return false;
    return true;
}

toml::table& EnsureTable(toml::table& parent, const char* key)
{
    if (!parent[key].is_table()) parent.insert_or_assign(key, toml::table{});
    return *parent[key].as_table();
}

void ReflectUnsigned(scene::IReflector& reflector, const char* name, uint32_t& value)
{
    int editable = static_cast<int>(value);
    reflector.IntRange(name, editable, 1, std::numeric_limits<int>::max());
    if (editable > 0) value = static_cast<uint32_t>(editable);
}

} /// @note namespace

bool RenderPipelineAsset::Capture(const renderer::RenderSettings& source)
{
    if (!Valid(source)) return false;
    renderer::RenderSettings captured;
    CopyOwned(source, captured);
    m_settings = std::move(captured);
    m_preservedToml.clear();
    m_preservedPassIndices.clear();
    return true;
}

void RenderPipelineAsset::ApplyTo(renderer::RenderSettings& target) const
{
    CopyOwned(m_settings, target);
}

void RenderPipelineAsset::Reflect(scene::IReflector& r)
{
    r.Group("Rendering");
    int pipeline = static_cast<int>(m_settings.pipeline);
    static constexpr const char* PIPELINES[] = {"Forward", "Deferred", "Forward+", "Deferred+"};
    r.Enum("pipeline", pipeline, PIPELINES);
    m_settings.pipeline = static_cast<renderer::RenderingPipeline>(pipeline);
    int mode = static_cast<int>(m_settings.modeRequest.mode);
    static constexpr const char* MODES[] = {"Raster", "Hybrid", "PathTracing"};
    r.Enum("mode", mode, MODES);
    m_settings.modeRequest.mode = static_cast<renderer::RenderMode>(mode);
    int profile = static_cast<int>(m_settings.modeRequest.pathProfile);
    static constexpr const char* PROFILES[] = {"Reference", "Game"};
    r.Enum("pathProfile", profile, PROFILES);
    m_settings.modeRequest.pathProfile = static_cast<renderer::PathTracingProfile>(profile);
    r.Field("rayShadow", m_settings.modeRequest.rayShadow);
    r.Field("rayReflection", m_settings.modeRequest.rayReflection);
    r.Field("rayDiffuseGi", m_settings.modeRequest.rayDiffuseGi);
    r.Field("gpuInstancing", m_settings.gpuInstancing);
    r.Field("asyncCompute", m_settings.asyncCompute);
    r.Group("Hybrid quality");
    auto& quality = m_settings.hybridQuality;
    const int previousPreset = static_cast<int>(renderer::DetectHybridQualityPreset(quality));
    int preset = previousPreset;
    static constexpr const char* HYBRID_PRESETS[] = {"Custom", "Low", "Balanced", "High"};
    r.Enum("hybridPreset", preset, HYBRID_PRESETS);
    if (preset != previousPreset && preset > 0 && preset <= 3)
        quality = renderer::MakeHybridQualityPreset(static_cast<renderer::HybridQualityPreset>(preset));
    const auto boundedUnsigned = [&](const char* name, uint32_t& field, int low, int high) {
        int editable = static_cast<int>(field);
        r.IntRange(name, editable, low, high);
        if (editable >= low && editable <= high) field = static_cast<uint32_t>(editable);
    };
    boundedUnsigned("reflectionSamples", quality.reflectionSamples, 1, 64);
    boundedUnsigned("historyLimit", quality.historyLimit, 1, 64);
    boundedUnsigned("spatialRadius", quality.spatialRadius, 0, 2);
    boundedUnsigned("glassBoundaryLimit", quality.glassBoundaryLimit, 1, 16);
    r.Field("maxTraceDistance", quality.maxTraceDistance);
    boundedUnsigned("maxHistoryMiB", quality.maxHistoryMiB, 1, 16384);
    boundedUnsigned("maxProbeCapturesPerFrame", quality.maxProbeCapturesPerFrame, 0, 16);
    r.Group("Hybrid budget targets [ms]");
    r.Field("frameBudgetMs", quality.frameBudgetMs);
    r.Field("asUpdateBudgetMs", quality.asUpdateBudgetMs);
    r.Field("traceBudgetMs", quality.traceBudgetMs);
    r.Field("reconstructionBudgetMs", quality.reconstructionBudgetMs);
    r.Field("probeUpdateBudgetMs", quality.probeUpdateBudgetMs);
    r.Group("Shadows");
    r.Field("shadowEnabled", m_settings.shadowEnabled);
    auto& shadow = m_settings.shadow;
    ReflectUnsigned(r, "mapResolution", shadow.mapResolution);
    r.IntRange("cascadeCount", shadow.cascadeCount, 1, renderer::kMaxShadowCascades);
    r.FloatRange("cascadeSplitLambda", shadow.cascadeSplitLambda, 0, 1);
    r.FloatRange("cascadeBlend", shadow.cascadeBlend, 0, 1);
    r.Field("autoFitDistance", shadow.autoFitDistance);
    r.IntRange("pcfRadius", shadow.pcfRadius, 0, 8);
    r.Field("pcssEnabled", shadow.pcssEnabled);
    r.Field("pcssLightRadius", shadow.pcssLightRadius);
    ReflectUnsigned(r, "punctualMapResolution", shadow.punctualMapResolution);
    r.IntRange("punctualPcfRadius", shadow.punctualPcfRadius, 0, 8);
    r.Field("maxShadowedPointLights", shadow.maxShadowedPointLights);
    r.Field("punctualShadowDistance", shadow.punctualShadowDistance);
    r.Group("Clustered lighting");
    r.Field("clusteredEnabled", m_settings.clustered.enabled);
    r.Field("clusteredMaxDistance", m_settings.clustered.maxDistance);
    r.Group("Resource budgets");
    r.Field("particleBudgetEnabled", m_settings.particleBudgetEnabled);
    r.Field("particleBudget", m_settings.particleBudget);
    ReflectUnsigned(r, "froxelGridX", m_settings.froxelFog.gridX);
    ReflectUnsigned(r, "froxelGridY", m_settings.froxelFog.gridY);
    ReflectUnsigned(r, "froxelGridZ", m_settings.froxelFog.gridZ);
    r.Group("Render graph");
    int schedule = static_cast<int>(m_settings.schedulePolicy);
    static constexpr const char* POLICIES[] = {"RegistrationOrder", "MinimizeLifetimes"};
    r.Enum("schedulePolicy", schedule, POLICIES);
    m_settings.schedulePolicy = static_cast<renderer::RenderGraphSchedulePolicy>(schedule);
    const size_t count = r.BeginObjectList("passOverrides", m_settings.passOverrides.size());
    m_settings.passOverrides.resize(count);
    m_preservedPassIndices.resize(count, scene::IReflector::NO_REMOVE);
    for (size_t i = 0; i < count; ++i) {
        r.BeginObjectElement(i);
        auto& entry = m_settings.passOverrides[i];
        r.Field("name", entry.name);
        r.Field("enabled", entry.enabled);
        r.Field("allowCulling", entry.allowCulling);
        r.ListField("extraReads", entry.extraReads);
        r.EndObjectElement();
    }
    const size_t removed = r.EndObjectList();
    if (removed < m_settings.passOverrides.size()) {
        m_settings.passOverrides.erase(m_settings.passOverrides.begin() + static_cast<std::ptrdiff_t>(removed));
        m_preservedPassIndices.erase(m_preservedPassIndices.begin() + static_cast<std::ptrdiff_t>(removed));
        return;
    }
    size_t from = 0;
    size_t to = 0;
    if (r.ObjectListMove(from, to) && from < m_settings.passOverrides.size()
        && to < m_settings.passOverrides.size() && from != to) {
        auto moved = std::move(m_settings.passOverrides[from]);
        const size_t preserved = m_preservedPassIndices[from];
        m_settings.passOverrides.erase(m_settings.passOverrides.begin() + static_cast<std::ptrdiff_t>(from));
        m_preservedPassIndices.erase(m_preservedPassIndices.begin() + static_cast<std::ptrdiff_t>(from));
        m_settings.passOverrides.insert(m_settings.passOverrides.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        m_preservedPassIndices.insert(m_preservedPassIndices.begin() + static_cast<std::ptrdiff_t>(to), preserved);
    }
}

bool RenderPipelineAssetCodec::Load(const toml::table& table, RenderPipelineAsset& out)
{
    const auto* type = table["type"].as_string();
    const auto* version = table["schemaVersion"].as_integer();
    if (!type || type->get() != RenderPipelineAsset::TYPE_NAME || !version
        || version->get() != RenderPipelineAsset::SCHEMA_VERSION) return false;
    renderer::RenderSettings value;
    bool valid = true;
    const auto* render = ReadTable(table, "render", valid);
    if (render && !ReadRequest(*render, value)) return false;
    if (const auto* hybrid = ReadTable(table, "hybrid", valid))
        if (!LoadHybridQuality(*hybrid, value.hybridQuality)) return false;
    if (const auto* shadow = ReadTable(table, "shadow", valid)) {
        auto& s = value.shadow;
        if (!Read(*shadow, "enabled", value.shadowEnabled) || !Read(*shadow, "mapResolution", s.mapResolution)
            || !Read(*shadow, "cascadeCount", s.cascadeCount) || !Read(*shadow, "cascadeSplitLambda", s.cascadeSplitLambda)
            || !Read(*shadow, "cascadeBlend", s.cascadeBlend) || !Read(*shadow, "autoFitDistance", s.autoFitDistance)
            || !Read(*shadow, "pcfRadius", s.pcfRadius) || !Read(*shadow, "pcssEnabled", s.pcssEnabled)
            || !Read(*shadow, "pcssLightRadius", s.pcssLightRadius) || !Read(*shadow, "punctualMapResolution", s.punctualMapResolution)
            || !Read(*shadow, "punctualPcfRadius", s.punctualPcfRadius) || !Read(*shadow, "maxShadowedPointLights", s.maxShadowedPointLights)
            || !Read(*shadow, "punctualShadowDistance", s.punctualShadowDistance)) return false;
    }
    if (const auto* clustered = ReadTable(table, "clustered", valid))
        if (!Read(*clustered, "enabled", value.clustered.enabled) || !Read(*clustered, "maxDistance", value.clustered.maxDistance)) return false;
    if (const auto* particles = ReadTable(table, "particles", valid))
        if (!Read(*particles, "enabled", value.particleBudgetEnabled) || !Read(*particles, "budget", value.particleBudget)) return false;
    if (const auto* froxel = ReadTable(table, "froxel", valid))
        if (!Read(*froxel, "gridX", value.froxelFog.gridX) || !Read(*froxel, "gridY", value.froxelFog.gridY)
            || !Read(*froxel, "gridZ", value.froxelFog.gridZ)) return false;
    if (const auto* graph = ReadTable(table, "graph", valid)) {
        std::string policy = "RegistrationOrder";
        if (!Read(*graph, "schedulePolicy", policy)) return false;
        if (policy == "RegistrationOrder") value.schedulePolicy = renderer::RenderGraphSchedulePolicy::RegistrationOrder;
        else if (policy == "MinimizeLifetimes") value.schedulePolicy = renderer::RenderGraphSchedulePolicy::MinimizeLifetimes;
        else return false;
        if (const auto* node = graph->get("passOverrides")) {
            const auto* overrides = node->as_array();
            if (!overrides) return false;
            for (const auto& item : *overrides) {
                const auto* entry = item.as_table();
                if (!entry) return false;
                renderer::RenderPassOverride pass;
                if (!Read(*entry, "name", pass.name) || !Read(*entry, "enabled", pass.enabled)
                    || !Read(*entry, "allowCulling", pass.allowCulling)) return false;
                if (const auto* readsNode = entry->get("extraReads")) {
                    const auto* reads = readsNode->as_array();
                    if (!reads) return false;
                    for (const auto& read : *reads) {
                        const auto* text = read.as_string();
                        if (!text) return false;
                        pass.extraReads.push_back(text->get());
                    }
                }
                value.passOverrides.push_back(std::move(pass));
            }
        }
    }
    if (!valid || !Valid(value)) return false;
    std::ostringstream preserved;
    preserved << table;
    std::vector<size_t> preservedPassIndices(value.passOverrides.size());
    for (size_t i = 0; i < preservedPassIndices.size(); ++i) preservedPassIndices[i] = i;
    out.m_settings = std::move(value);
    out.m_preservedToml = preserved.str();
    out.m_preservedPassIndices = std::move(preservedPassIndices);
    return true;
}

bool RenderPipelineAssetCodec::Save(const RenderPipelineAsset& asset, toml::table& out)
{
    const auto& value = asset.m_settings;
    if (!Valid(value)) return false;
    toml::table table;
    if (!asset.m_preservedToml.empty()) {
        auto original = toml::parse(asset.m_preservedToml);
        if (!original) return false;
        table = std::move(original.table());
    }
    table.insert_or_assign("type", RenderPipelineAsset::TYPE_NAME);
    table.insert_or_assign("schemaVersion", RenderPipelineAsset::SCHEMA_VERSION);
    auto& render = EnsureTable(table, "render");
    static constexpr const char* PIPELINES[] = {"Forward", "Deferred", "Forward+", "Deferred+"};
    static constexpr const char* MODES[] = {"Raster", "Hybrid", "PathTracing"};
    render.insert_or_assign("pipeline", PIPELINES[static_cast<unsigned>(value.pipeline)]);
    render.insert_or_assign("mode", MODES[static_cast<unsigned>(value.modeRequest.mode)]);
    render.insert_or_assign("pathProfile", value.modeRequest.pathProfile == renderer::PathTracingProfile::GAME ? "Game" : "Reference");
    render.insert_or_assign("rayShadow", value.modeRequest.rayShadow);
    render.insert_or_assign("rayReflection", value.modeRequest.rayReflection);
    render.insert_or_assign("rayDiffuseGi", value.modeRequest.rayDiffuseGi);
    render.insert_or_assign("gpuInstancing", value.gpuInstancing);
    render.insert_or_assign("asyncCompute", value.asyncCompute);
    SaveHybridQuality(value.hybridQuality, EnsureTable(table, "hybrid"));
    auto& shadow = EnsureTable(table, "shadow");
    const auto& s = value.shadow;
    shadow.insert_or_assign("enabled", value.shadowEnabled);
    shadow.insert_or_assign("mapResolution", static_cast<int64_t>(s.mapResolution));
    shadow.insert_or_assign("cascadeCount", s.cascadeCount);
    shadow.insert_or_assign("cascadeSplitLambda", static_cast<double>(s.cascadeSplitLambda));
    shadow.insert_or_assign("cascadeBlend", static_cast<double>(s.cascadeBlend));
    shadow.insert_or_assign("autoFitDistance", static_cast<double>(s.autoFitDistance));
    shadow.insert_or_assign("pcfRadius", s.pcfRadius);
    shadow.insert_or_assign("pcssEnabled", s.pcssEnabled);
    shadow.insert_or_assign("pcssLightRadius", static_cast<double>(s.pcssLightRadius));
    shadow.insert_or_assign("punctualMapResolution", static_cast<int64_t>(s.punctualMapResolution));
    shadow.insert_or_assign("punctualPcfRadius", s.punctualPcfRadius);
    shadow.insert_or_assign("maxShadowedPointLights", s.maxShadowedPointLights);
    shadow.insert_or_assign("punctualShadowDistance", static_cast<double>(s.punctualShadowDistance));
    auto& clustered = EnsureTable(table, "clustered");
    clustered.insert_or_assign("enabled", value.clustered.enabled);
    clustered.insert_or_assign("maxDistance", static_cast<double>(value.clustered.maxDistance));
    auto& particles = EnsureTable(table, "particles");
    particles.insert_or_assign("enabled", value.particleBudgetEnabled);
    particles.insert_or_assign("budget", value.particleBudget);
    auto& froxel = EnsureTable(table, "froxel");
    froxel.insert_or_assign("gridX", static_cast<int64_t>(value.froxelFog.gridX));
    froxel.insert_or_assign("gridY", static_cast<int64_t>(value.froxelFog.gridY));
    froxel.insert_or_assign("gridZ", static_cast<int64_t>(value.froxelFog.gridZ));
    auto& graph = EnsureTable(table, "graph");
    graph.insert_or_assign("schedulePolicy", value.schedulePolicy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
        ? "MinimizeLifetimes" : "RegistrationOrder");
    toml::array overrides;
    const auto* oldOverrides = graph["passOverrides"].as_array();
    for (size_t i = 0; i < value.passOverrides.size(); ++i) {
        const auto& pass = value.passOverrides[i];
        toml::table entry;
        const size_t original = i < asset.m_preservedPassIndices.size()
            ? asset.m_preservedPassIndices[i] : scene::IReflector::NO_REMOVE;
        if (oldOverrides && original < oldOverrides->size())
            if (const auto* old = (*oldOverrides)[original].as_table(); old && (*old)["name"].value_or(std::string{}) == pass.name) entry = *old;
        entry.insert_or_assign("name", pass.name);
        entry.insert_or_assign("enabled", pass.enabled);
        entry.insert_or_assign("allowCulling", pass.allowCulling);
        toml::array reads;
        for (const auto& read : pass.extraReads) reads.push_back(read);
        entry.insert_or_assign("extraReads", std::move(reads));
        overrides.push_back(std::move(entry));
    }
    graph.insert_or_assign("passOverrides", std::move(overrides));
    out = std::move(table);
    return true;
}

bool RenderPipelineAssetCodec::LoadHybridQuality(const toml::table& table, renderer::HybridQualitySettings& out)
{
    renderer::HybridQualitySettings value;
    if (!Read(table, "reflectionSamples", value.reflectionSamples) || !Read(table, "historyLimit", value.historyLimit)
        || !Read(table, "spatialRadius", value.spatialRadius) || !Read(table, "glassBoundaryLimit", value.glassBoundaryLimit)
        || !Read(table, "maxTraceDistance", value.maxTraceDistance) || !Read(table, "maxHistoryMiB", value.maxHistoryMiB)
        || !Read(table, "maxProbeCapturesPerFrame", value.maxProbeCapturesPerFrame)
        || !Read(table, "frameBudgetMs", value.frameBudgetMs) || !Read(table, "asUpdateBudgetMs", value.asUpdateBudgetMs)
        || !Read(table, "traceBudgetMs", value.traceBudgetMs) || !Read(table, "reconstructionBudgetMs", value.reconstructionBudgetMs)
        || !Read(table, "probeUpdateBudgetMs", value.probeUpdateBudgetMs) || !renderer::IsHybridQualityValid(value)) return false;
    out = value;
    return true;
}

void RenderPipelineAssetCodec::SaveHybridQuality(const renderer::HybridQualitySettings& value, toml::table& out)
{
    out.insert_or_assign("reflectionSamples", static_cast<int64_t>(value.reflectionSamples));
    out.insert_or_assign("historyLimit", static_cast<int64_t>(value.historyLimit));
    out.insert_or_assign("spatialRadius", static_cast<int64_t>(value.spatialRadius));
    out.insert_or_assign("glassBoundaryLimit", static_cast<int64_t>(value.glassBoundaryLimit));
    out.insert_or_assign("maxTraceDistance", static_cast<double>(value.maxTraceDistance));
    out.insert_or_assign("maxHistoryMiB", static_cast<int64_t>(value.maxHistoryMiB));
    out.insert_or_assign("maxProbeCapturesPerFrame", static_cast<int64_t>(value.maxProbeCapturesPerFrame));
    out.insert_or_assign("frameBudgetMs", static_cast<double>(value.frameBudgetMs));
    out.insert_or_assign("asUpdateBudgetMs", static_cast<double>(value.asUpdateBudgetMs));
    out.insert_or_assign("traceBudgetMs", static_cast<double>(value.traceBudgetMs));
    out.insert_or_assign("reconstructionBudgetMs", static_cast<double>(value.reconstructionBudgetMs));
    out.insert_or_assign("probeUpdateBudgetMs", static_cast<double>(value.probeUpdateBudgetMs));
}

bool ResolveRenderPipelineSettings(const renderer::RenderSettings& inlineFallback,
    std::string_view assetReference, renderer::RenderSettings& out)
{
    out = inlineFallback;
    if (assetReference.empty()) return false;
    const std::string reference(assetReference);
    /// @note A cached last-good object survives malformed reloads, but a removed assignment's file must select inline fallback.
    const std::string absolute = AssetManager::ResolveAssetPath(reference);
    if (absolute.empty() || !util::FileSystem::Exists(absolute)) return false;
    const auto* data = DataAssetRegistry::Resolve(reference);
    if (!data || std::string_view(data->GetTypeName()) != RenderPipelineAsset::TYPE_NAME) return false;
    const auto& pipeline = static_cast<const RenderPipelineAsset&>(*data);
    if (!Valid(pipeline.Settings())) return false;
    pipeline.ApplyTo(out);
    return true;
}

bool CreateRenderPipelineAsset(std::string_view path, const renderer::RenderSettings& source)
{
    if (path.empty()) return false;
    RenderPipelineAsset captured;
    if (!captured.Capture(source)) return false;
    const std::string absolute = AssetManager::ResolveAssetPath(std::string(path));
    if (absolute.empty()) return false;
    std::error_code error;
    if (std::filesystem::exists(util::FileSystem::PathFromUtf8(absolute), error) || error) return false;
    toml::table table;
    if (!RenderPipelineAssetCodec::Save(captured, table)) return false;
    std::ostringstream text;
    text << table;
    if (!util::FileSystem::WriteTextAtomic(absolute, text.str())) return false;
    (void)DataAssetRegistry::ReloadFile(absolute);
    return true;
}

} /// @note namespace fbzz::asset

FBZZ_REGISTER_BUILTIN_DATA_ASSET(::fbzz::asset::RenderPipelineAsset);
