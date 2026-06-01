// FBZZ Engine
// TerrainAssetSerializer.cpp | fbzz::scene
// TerrainComponent の外部アセット保存・復元
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>

namespace fbzz::scene {
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

toml::table WriteTerrainData(const TerrainComponent& tc)
{
    toml::table terrainTbl;
    terrainTbl.insert("columns",   static_cast<int64_t>(tc.columns));
    terrainTbl.insert("rows",      static_cast<int64_t>(tc.rows));
    terrainTbl.insert("cellSize",  static_cast<double>(tc.cellSize));
    terrainTbl.insert("maxHeight", static_cast<double>(tc.maxHeight));
    terrainTbl.insert("chunkSize", static_cast<int64_t>(tc.chunkSize));

    // WHAT: heightData は正負の正規化高さ [-1, 1] をそのまま保存する。
    // WHY: maxHeight を変更しても元データの相対形状を保てるため、編集途中の Terrain を再利用しやすい。
    toml::array heightArr;
    for (float h : tc.heightData)
        heightArr.push_back(static_cast<double>(h));
    terrainTbl.insert("heightData", std::move(heightArr));

    toml::array splatArr;
    for (uint8_t s : tc.splatData)
        splatArr.push_back(static_cast<int64_t>(s));
    terrainTbl.insert("splatData", std::move(splatArr));

    toml::array layersArr;
    for (const auto& layer : tc.layers) {
        toml::table layerTbl;
        layerTbl.insert("diffusePath", layer.diffusePath);
        layerTbl.insert("normalPath", layer.normalPath);
        layerTbl.insert("aoRoughnessPath", layer.aoRoughnessPath);
        layerTbl.insert("tilingX", static_cast<double>(layer.tilingX));
        layerTbl.insert("tilingZ", static_cast<double>(layer.tilingZ));
        layerTbl.insert("normalStrength", static_cast<double>(layer.normalStrength));
        layerTbl.insert("roughness", static_cast<double>(layer.roughness));
        layerTbl.insert("ambientOcclusion", static_cast<double>(layer.ambientOcclusion));
        layerTbl.insert("autoBlendEnabled", layer.autoBlendEnabled);
        layerTbl.insert("autoBlendStrength", static_cast<double>(layer.autoBlendStrength));
        layerTbl.insert("autoMinHeight", static_cast<double>(layer.autoMinHeight));
        layerTbl.insert("autoMaxHeight", static_cast<double>(layer.autoMaxHeight));
        layerTbl.insert("autoHeightFade", static_cast<double>(layer.autoHeightFade));
        layerTbl.insert("autoMinSlope", static_cast<double>(layer.autoMinSlope));
        layerTbl.insert("autoMaxSlope", static_cast<double>(layer.autoMaxSlope));
        layerTbl.insert("autoSlopeFade", static_cast<double>(layer.autoSlopeFade));
        layersArr.push_back(std::move(layerTbl));
    }
    terrainTbl.insert("layers", std::move(layersArr));
    return terrainTbl;
}

bool ReadTerrainData(const toml::table& terrainTbl, TerrainComponent& tc)
{
    tc.columns   = static_cast<int>(terrainTbl["columns"].value_or(int64_t{129}));
    tc.rows      = static_cast<int>(terrainTbl["rows"].value_or(int64_t{129}));
    tc.cellSize  = static_cast<float>(terrainTbl["cellSize"].value_or(1.0));
    tc.maxHeight = static_cast<float>(terrainTbl["maxHeight"].value_or(30.0));
    tc.chunkSize = static_cast<int>(terrainTbl["chunkSize"].value_or(int64_t{32}));

    tc.heightData.clear();
    if (auto* heightArr = terrainTbl["heightData"].as_array()) {
        tc.heightData.reserve(heightArr->size());
        for (auto& v : *heightArr)
            tc.heightData.push_back(static_cast<float>(v.value_or(0.0)));
    } else {
        tc.InitFlat(0.0f);
    }

    tc.splatData.clear();
    if (auto* splatArr = terrainTbl["splatData"].as_array()) {
        tc.splatData.reserve(splatArr->size());
        for (auto& v : *splatArr)
            tc.splatData.push_back(static_cast<uint8_t>(v.value_or(int64_t{0})));
    }

    tc.layers.clear();
    if (auto* layersArr = terrainTbl["layers"].as_array()) {
        for (auto& layerNode : *layersArr) {
            if (auto* layerTbl = layerNode.as_table()) {
                TerrainLayer layer{};
                layer.diffusePath = (*layerTbl)["diffusePath"].value_or(std::string{});
                layer.normalPath = (*layerTbl)["normalPath"].value_or(std::string{});
                layer.aoRoughnessPath = (*layerTbl)["aoRoughnessPath"].value_or(std::string{});
                layer.tilingX = static_cast<float>((*layerTbl)["tilingX"].value_or(8.0));
                layer.tilingZ = static_cast<float>((*layerTbl)["tilingZ"].value_or(8.0));
                layer.normalStrength = static_cast<float>((*layerTbl)["normalStrength"].value_or(1.0));
                layer.roughness = static_cast<float>((*layerTbl)["roughness"].value_or(0.8));
                layer.ambientOcclusion = static_cast<float>((*layerTbl)["ambientOcclusion"].value_or(1.0));
                layer.autoBlendEnabled = (*layerTbl)["autoBlendEnabled"].value_or(false);
                layer.autoBlendStrength = static_cast<float>((*layerTbl)["autoBlendStrength"].value_or(1.0));
                layer.autoMinHeight = static_cast<float>((*layerTbl)["autoMinHeight"].value_or(-10000.0));
                layer.autoMaxHeight = static_cast<float>((*layerTbl)["autoMaxHeight"].value_or(10000.0));
                layer.autoHeightFade = static_cast<float>((*layerTbl)["autoHeightFade"].value_or(1.0));
                layer.autoMinSlope = static_cast<float>((*layerTbl)["autoMinSlope"].value_or(0.0));
                layer.autoMaxSlope = static_cast<float>((*layerTbl)["autoMaxSlope"].value_or(1.0));
                layer.autoSlopeFade = static_cast<float>((*layerTbl)["autoSlopeFade"].value_or(0.1));
                if (tc.layers.size() < 4) tc.layers.push_back(std::move(layer));
            }
        }
    }

    tc.heightDirty = true;
    tc.splatDirty = true;
    tc.colliderDirty = true;
    return true;
}

} // namespace

bool TerrainAssetSerializer::Save(const TerrainComponent& component, const std::string& path)
{
    if (path.empty()) return false;

    toml::table doc;
    toml::table metaTbl;
    metaTbl.insert("format_version", 1);
    doc.insert("terrain_asset", std::move(metaTbl));
    doc.insert("terrain", WriteTerrainData(component));

    NormalizeTomlFloats(doc);

    std::ostringstream ss;
    ss << doc;

    const std::string dir = util::FileSystem::GetDirectory(path);
    if (!dir.empty())
        util::FileSystem::EnsureDirectory(dir);

    if (!util::FileSystem::WriteText(path, ss.str())) {
        FBZZ_LOG_ERROR("TerrainAssetSerializer: save failed: %s", path.c_str());
        return false;
    }

    return true;
}

bool TerrainAssetSerializer::Load(const std::string& path, TerrainComponent& component)
{
    if (path.empty()) return false;

    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        FBZZ_LOG_ERROR("TerrainAssetSerializer: read failed: %s", path.c_str());
        return false;
    }

    toml::parse_result result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("TerrainAssetSerializer: parse failed: %s", path.c_str());
        return false;
    }

    toml::table doc = result.table();
    const toml::table* terrainTbl = doc["terrain"].as_table();
    if (!terrainTbl)
        terrainTbl = &doc;

    return ReadTerrainData(*terrainTbl, component);
}

} // namespace fbzz::scene
