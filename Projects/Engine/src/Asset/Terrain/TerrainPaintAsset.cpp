/// @file    TerrainPaintAsset.cpp
/// @brief   .terrainpaint の固定スキーマ、配置規則の検証と原子的保存。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include <Engine/Asset/Terrain/TerrainPaintAsset.hpp>
#include <Engine/Asset/Instances/MeshInstancesAsset.hpp>
#include "../Instances/MeshInstancesSerialization.hpp"
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
    constexpr size_t MAX_NAME_BYTES = 256;
    if (asset.name.empty() || asset.name.size() > MAX_NAME_BYTES) {
        return false;
    }
    if (asset.category < TerrainPaintCategory::GRASS || asset.category > TerrainPaintCategory::OTHER) {
        return false;
    }

    // 配置素材はモデルの定義だけを保持する。配置済みの個体は別のアセットに保存する。
    const auto definition = asset.definition.Snapshot();
    if (!definition || !definition->placements.empty()) {
        return false;
    }

    // 範囲の下限に別フィールドを使う設定では、その下限を先に検証する。
    const auto& placement = asset.placement;
    const auto inRange = [](float value, float low, float high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    if (!inRange(placement.spacing, 0.25f, 10000.0f) || !inRange(placement.density, 0.0f, 1.0f)) {
        return false;
    }
    if (!inRange(placement.minScale, 0.01f, 100.0f) || !inRange(placement.maxScale, placement.minScale, 100.0f)) {
        return false;
    }
    if (!inRange(placement.minYaw, -360.0f, 360.0f) || !inRange(placement.maxYaw, placement.minYaw, 360.0f)) {
        return false;
    }
    if (!inRange(placement.minSlope, 0.0f, 90.0f) || !inRange(placement.maxSlope, placement.minSlope, 90.0f)) {
        return false;
    }
    if (!inRange(placement.downslopeJitter, 0.0f, 180.0f) || !inRange(placement.embedDepth, 0.0f, 100.0f)) {
        return false;
    }
    return inRange(placement.waterClearance, 0.0f, 1000.0f) && inRange(placement.pathClearance, 0.0f, 1000.0f);
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
    if (!ReadFile(path, text, 1024 * 1024)) {
        return false;
    }
    const auto parsed = toml::parse(text, std::string(path));
    if (!parsed || parsed["format"].value<std::string>() != "fbzz.terrainpaint") {
        return false;
    }
    const auto version = parsed["version"].value<int64_t>();
    if (!version || (*version != 1 && *version != 2)) {
        return false;
    }
    TerrainPaintAsset candidate;
    const auto name = parsed["name"].value<std::string>(), category = parsed["category"].value<std::string>();
    if (!name || !category) {
        return false;
    }
    candidate.name = *name;
    // 素材のモデルと配置規則を候補へ読み、検証が完了してから出力に渡す。
    if (*category == "grass") {
        candidate.category = TerrainPaintCategory::GRASS;
    } else if (*category == "tree") {
        candidate.category = TerrainPaintCategory::TREE;
    } else if (*category == "rock") {
        candidate.category = TerrainPaintCategory::ROCK;
    } else if (*category != "other") {
        return false;
    }

    std::vector<InstanceMeshPart> parts;
    std::vector<float> heights;
    if (!ReadDefinition(parsed.table(), parts, heights)) {
        return false;
    }
    if (!candidate.definition.Replace(parts, heights, {})) {
        return false;
    }

    const auto* placementTable = parsed["placement"].as_table();
    if (!placementTable) {
        return false;
    }
    auto& placement = candidate.placement;
    const auto readNumber = [placementTable](const char* key, float& value) {
        const auto item = (*placementTable)[key].value<float>();
        if (!item) {
            return false;
        }
        value = *item;
        return true;
    };
    const auto alignToNormal = (*placementTable)["alignToNormal"].value<bool>();
    const auto alignDownslope = (*placementTable)["alignDownslope"].value<bool>();
    if (!readNumber("spacing", placement.spacing) || !readNumber("density", placement.density)) {
        return false;
    }
    if (!readNumber("minScale", placement.minScale) || !readNumber("maxScale", placement.maxScale)
        || !readNumber("minYaw", placement.minYaw) || !readNumber("maxYaw", placement.maxYaw)) {
        return false;
    }
    if (!readNumber("minSlope", placement.minSlope) || !readNumber("maxSlope", placement.maxSlope)
        || !readNumber("downslopeJitter", placement.downslopeJitter) || !readNumber("embedDepth", placement.embedDepth)) {
        return false;
    }
    if (!alignToNormal || !alignDownslope) {
        return false;
    }
    placement.alignToNormal = *alignToNormal;
    placement.alignDownslope = *alignDownslope;

    // 版 1 には水面や道からの離隔がないため、候補の既定値を使う。
    if (*version == 2 && (!readNumber("waterClearance", placement.waterClearance) || !readNumber("pathClearance", placement.pathClearance))) {
        return false;
    }
    if (!ValidateTerrainPaintAsset(candidate)) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

bool SaveTerrainPaintAsset(std::string_view path, const TerrainPaintAsset& asset)
{
    if (!ValidateTerrainPaintAsset(asset)) {
        return false;
    }
    static constexpr const char* CATEGORIES[]{"grass", "tree", "rock", "other"};
    toml::table table{{"format", "fbzz.terrainpaint"}, {"version", 2}, {"name", asset.name},
        {"category", CATEGORIES[static_cast<int>(asset.category)]}};
    mesh_instances_serialization::WriteDefinition(table, asset.definition);
    const auto& p = asset.placement;
    table.insert("placement", toml::table{{"spacing", p.spacing}, {"density", p.density}, {"minScale", p.minScale},
        {"maxScale", p.maxScale}, {"minYaw", p.minYaw}, {"maxYaw", p.maxYaw}, {"minSlope", p.minSlope}, {"maxSlope", p.maxSlope},
        {"alignToNormal", p.alignToNormal}, {"alignDownslope", p.alignDownslope}, {"downslopeJitter", p.downslopeJitter}, {"embedDepth", p.embedDepth},
        {"waterClearance", p.waterClearance}, {"pathClearance", p.pathClearance}});
    return mesh_instances_serialization::WriteFile(path, table);
}
} // namespace fbzz::asset
