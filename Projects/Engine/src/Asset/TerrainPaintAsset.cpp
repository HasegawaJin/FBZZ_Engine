/// @file    TerrainPaintAsset.cpp
/// @brief   .terrainpaint の固定スキーマ、配置規則の検証と原子的保存。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include "MeshInstancesSerialization.hpp"
#include <cmath>

namespace fbzz::asset {

const char* TerrainPaintCategoryName(TerrainPaintCategory category)
{
    switch (category) {
        case TerrainPaintCategory::GRASS: return "Grass";
        case TerrainPaintCategory::TREE: return "Tree";
        case TerrainPaintCategory::ROCK: return "Rock";
        case TerrainPaintCategory::OTHER: return "Other";
    }
    return "Unknown";
}

bool ValidateTerrainPaintAsset(const TerrainPaintAsset& asset)
{
    const auto& p = asset.placement;
    const auto range = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
    return !asset.name.empty() && asset.name.size() <= 256
        && asset.category >= TerrainPaintCategory::GRASS && asset.category <= TerrainPaintCategory::OTHER
        && asset.definition.Snapshot() && asset.definition.Snapshot()->placements.empty()
        && range(p.spacing, 0.25f, 10000.0f) && range(p.density, 0.0f, 1.0f)
        && range(p.minScale, 0.01f, 100.0f) && range(p.maxScale, p.minScale, 100.0f)
        && range(p.minYaw, -360.0f, 360.0f) && range(p.maxYaw, p.minYaw, 360.0f)
        && range(p.minSlope, 0.0f, 90.0f) && range(p.maxSlope, p.minSlope, 90.0f)
        && range(p.downslopeJitter, 0.0f, 180.0f) && range(p.embedDepth, 0.0f, 100.0f);
}

bool TerrainPaintAssetsEqual(const TerrainPaintAsset& first, const TerrainPaintAsset& second)
{
    return first.name == second.name && first.category == second.category && first.placement == second.placement
        && MeshInstanceDefinitionsEqual(first.definition, second.definition);
}

bool LoadTerrainPaintAsset(std::string_view path, TerrainPaintAsset& out)
{
    using namespace mesh_instances_serialization;
    std::string text;
    if (!ReadFile(path, text, 1024 * 1024)) return false;
    const auto parsed = toml::parse(text, std::string(path));
    if (!parsed || parsed["format"].value<std::string>() != "fbzz.terrainpaint" || parsed["version"].value<int64_t>() != 1) return false;
    TerrainPaintAsset candidate;
    const auto name = parsed["name"].value<std::string>(), category = parsed["category"].value<std::string>();
    if (!name || !category) return false;
    candidate.name = *name;
    if (*category == "grass") candidate.category = TerrainPaintCategory::GRASS;
    else if (*category == "tree") candidate.category = TerrainPaintCategory::TREE;
    else if (*category == "rock") candidate.category = TerrainPaintCategory::ROCK;
    else if (*category != "other") return false;
    std::vector<InstanceMeshPart> parts;
    std::vector<float> heights;
    if (!ReadDefinition(parsed.table(), parts, heights) || !candidate.definition.Replace(parts, heights, {})) return false;
    const auto* placement = parsed["placement"].as_table();
    if (!placement) return false;
    auto& p = candidate.placement;
    const auto number = [&](const char* key, float& value) {
        const auto item = (*placement)[key].value<float>();
        if (!item) return false;
        value = *item; return true;
    };
    const auto normal = (*placement)["alignToNormal"].value<bool>(), downslope = (*placement)["alignDownslope"].value<bool>();
    if (!number("spacing", p.spacing) || !number("density", p.density) || !number("minScale", p.minScale)
        || !number("maxScale", p.maxScale) || !number("minYaw", p.minYaw) || !number("maxYaw", p.maxYaw)
        || !number("minSlope", p.minSlope) || !number("maxSlope", p.maxSlope)
        || !number("downslopeJitter", p.downslopeJitter) || !number("embedDepth", p.embedDepth) || !normal || !downslope) return false;
    p.alignToNormal = *normal; p.alignDownslope = *downslope;
    if (!ValidateTerrainPaintAsset(candidate)) return false;
    out = std::move(candidate);
    return true;
}

bool SaveTerrainPaintAsset(std::string_view path, const TerrainPaintAsset& asset)
{
    if (!ValidateTerrainPaintAsset(asset)) return false;
    static constexpr const char* CATEGORIES[]{"grass", "tree", "rock", "other"};
    toml::table table{{"format", "fbzz.terrainpaint"}, {"version", 1}, {"name", asset.name},
        {"category", CATEGORIES[static_cast<int>(asset.category)]}};
    mesh_instances_serialization::WriteDefinition(table, asset.definition);
    const auto& p = asset.placement;
    table.insert("placement", toml::table{{"spacing", p.spacing}, {"density", p.density}, {"minScale", p.minScale},
        {"maxScale", p.maxScale}, {"minYaw", p.minYaw}, {"maxYaw", p.maxYaw}, {"minSlope", p.minSlope}, {"maxSlope", p.maxSlope},
        {"alignToNormal", p.alignToNormal}, {"alignDownslope", p.alignDownslope}, {"downslopeJitter", p.downslopeJitter}, {"embedDepth", p.embedDepth}});
    return mesh_instances_serialization::WriteFile(path, table);
}
} /// @note namespace fbzz::asset
