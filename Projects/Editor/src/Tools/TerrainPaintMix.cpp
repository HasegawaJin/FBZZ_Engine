/// @file    TerrainPaintMix.cpp
/// @brief   再利用する地形ブラシ配合の固定スキーマと原子的保存。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "TerrainPaintMix.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace fbzz::editor {
namespace terrain_paint_mix_detail {
struct FloatField { const char* name; float TerrainScatterSettings::* member; float low, high; };
constexpr FloatField FIELDS[]{
    {"spacing", &TerrainScatterSettings::spacing, 0.25f, 10000}, {"density", &TerrainScatterSettings::density, 0, 1},
    {"minHeight", &TerrainScatterSettings::minHeight, -100000, 100000}, {"maxHeight", &TerrainScatterSettings::maxHeight, -100000, 100000},
    {"minSlope", &TerrainScatterSettings::minSlope, 0, 90}, {"maxSlope", &TerrainScatterSettings::maxSlope, 0, 90},
    {"layerThreshold", &TerrainScatterSettings::layerThreshold, 0, 1}, {"minScale", &TerrainScatterSettings::minScale, 0.01f, 100},
    {"maxScale", &TerrainScatterSettings::maxScale, 0.01f, 100}, {"minYaw", &TerrainScatterSettings::minYaw, -360, 360},
    {"maxYaw", &TerrainScatterSettings::maxYaw, -360, 360}, {"downslopeJitter", &TerrainScatterSettings::downslopeJitter, 0, 180},
    {"embedDepth", &TerrainScatterSettings::embedDepth, 0, 100}, {"minimumDistance", &TerrainScatterSettings::minimumDistance, 0, 10000}};

bool Valid(const TerrainPaintMix& mix)
{
    if (mix.name.empty() || mix.name.size() > 256 || mix.sources.empty() || mix.sources.size() > TERRAIN_SCATTER_PALETTE_LIMIT
        || !std::isfinite(mix.radius) || mix.radius < 0.25f || mix.radius > 500 || mix.excludedLayer < -1 || mix.excludedLayer > 255
        || !std::isfinite(mix.feather) || mix.feather < 0 || mix.feather > 1 || mix.excludedMaterials.size() > 256) return false;
    for (const auto& ref : mix.excludedMaterials) if(ref.empty() || ref.size()>4096) return false;
    std::unordered_set<std::string> sources;
    for (const auto& source : mix.sources) {
        const auto guid = asset::AssetDatabase::GuidFromRef(source.reference);
        if (guid.empty() || source.reference.size() > 4096 || !sources.insert(guid).second
            || !std::isfinite(source.weight) || source.weight < 0 || source.weight > 100) return false;
    }
    const auto& p = mix.settings;
    for (const auto& field : FIELDS) {
        const float value = p.*field.member;
        if (!std::isfinite(value) || value < field.low || value > field.high) return false;
    }
    return p.minHeight <= p.maxHeight && p.minSlope <= p.maxSlope && p.minScale <= p.maxScale && p.minYaw <= p.maxYaw
        && p.layer >= -1 && p.layer <= 255 && p.maxInstances > 0 && p.maxInstances <= 1000000 && p.seed <= INT32_MAX;
}
} /// @note namespace terrain_paint_mix_detail

bool SaveTerrainPaintMix(const std::string& path, const TerrainPaintMix& mix)
{
    using namespace terrain_paint_mix_detail;
    if (!Valid(mix)) return false;
    toml::table table{{"format", "fbzz.terrainmix"}, {"version", 2}, {"name", mix.name}, {"radius", mix.radius},
        {"avoidWater", mix.avoidWater}, {"avoidPaths", mix.avoidPaths}, {"excludedLayer", mix.excludedLayer}};
    toml::array excluded; for(const auto& ref:mix.excludedMaterials)excluded.push_back(ref);
    table.insert("excludedMaterials",std::move(excluded)); table.insert("feather",mix.feather);
    toml::array sources;
    for (const auto& source : mix.sources) sources.push_back(toml::table{{"reference", source.reference}, {"weight", source.weight}});
    table.insert("sources", std::move(sources));
    const auto& p = mix.settings;
    toml::table settings{{"seed", p.seed}, {"maxInstances", p.maxInstances}, {"layer", p.layer},
        {"alignToNormal", p.alignToNormal}, {"alignDownslope", p.alignDownslope}};
    for (const auto& field : FIELDS) settings.insert(field.name, p.*field.member);
    table.insert("settings", std::move(settings));
    std::ostringstream text; text << table;
    return util::FileSystem::WriteTextAtomic(path, text.str());
}

bool LoadTerrainPaintMix(const std::string& path, TerrainPaintMix& out)
{
    using namespace terrain_paint_mix_detail;
    std::string text;
    std::error_code error;
    const auto size = std::filesystem::file_size(util::FileSystem::PathFromUtf8(path), error);
    if (error || size > 1024 * 1024 || !util::FileSystem::ReadText(path, text)) return false;
    const auto parsed = toml::parse(text, path);
    if (!parsed || parsed["format"].value<std::string>() != "fbzz.terrainmix") return false;
    const auto version=parsed["version"].value<int>(); if(!version || (*version!=1 && *version!=2))return false;
    TerrainPaintMix candidate;
    const auto name = parsed["name"].value<std::string>();
    const auto radius = parsed["radius"].value<float>(); const auto water = parsed["avoidWater"].value<bool>();
    const auto layer = parsed["excludedLayer"].value<int>();
    const auto* sources = parsed["sources"].as_array(); const auto* settings = parsed["settings"].as_table();
    if (!name || !radius || !water || !layer || !sources || !settings || sources->size() > TERRAIN_SCATTER_PALETTE_LIMIT) return false;
    candidate.name = *name; candidate.radius = *radius; candidate.avoidWater = *water; candidate.excludedLayer = *layer;
    const auto paths = parsed["avoidPaths"].value<bool>();
    if (!paths) return false;
    candidate.avoidPaths = *paths;
    if (*version==2) {
        const auto fade=parsed["feather"].value<float>(); const auto* excluded=parsed["excludedMaterials"].as_array();
        if(!fade||!excluded||excluded->size()>256)return false; candidate.feather=*fade;
        for(const auto& ref:*excluded){const auto text=ref.value<std::string>();if(!text)return false;candidate.excludedMaterials.push_back(*text);}
    }
    for (const auto& item : *sources) {
        const auto* row = item.as_table();
        if (!row) return false;
        const auto ref = (*row)["reference"].value<std::string>(); const auto weight = (*row)["weight"].value<float>();
        if (!ref || !weight) return false;
        candidate.sources.push_back({*ref, *weight});
    }
    auto& p = candidate.settings;
    for (const auto& field : FIELDS) {
        const auto value = (*settings)[field.name].value<float>();
        if (!value) return false;
        p.*field.member = *value;
    }
    const auto seed = (*settings)["seed"].value<int64_t>(), count = (*settings)["maxInstances"].value<int64_t>();
    const auto targetLayer = (*settings)["layer"].value<int>();
    const auto normal = (*settings)["alignToNormal"].value<bool>(), downslope = (*settings)["alignDownslope"].value<bool>();
    if (!seed || *seed < 0 || *seed > INT32_MAX || !count || *count < 1 || *count > 1000000 || !targetLayer || !normal || !downslope) return false;
    p.seed = static_cast<uint32_t>(*seed); p.maxInstances = static_cast<uint32_t>(*count); p.layer = *targetLayer;
    p.alignToNormal = *normal; p.alignDownslope = *downslope;
    if (!Valid(candidate)) return false;
    out = std::move(candidate);
    return true;
}
} /// @note namespace fbzz::editor
