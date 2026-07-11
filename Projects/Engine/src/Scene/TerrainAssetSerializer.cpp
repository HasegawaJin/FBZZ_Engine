// FBZZ Engine
// TerrainAssetSerializer.cpp | fbzz::scene
// TerrainComponent の外部アセット保存・復元
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
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
    terrainTbl.insert("columns",      static_cast<int64_t>(tc.columns));
    terrainTbl.insert("rows",         static_cast<int64_t>(tc.rows));
    terrainTbl.insert("cellSize",     static_cast<double>(tc.cellSize));
    terrainTbl.insert("maxHeight",    static_cast<double>(tc.maxHeight));
    terrainTbl.insert("chunkSize",    static_cast<int64_t>(tc.chunkSize));
    toml::array layerMatArr;
    for (const auto& p : tc.layerMaterials)
        layerMatArr.push_back(p);
    terrainTbl.insert("layerMaterials", std::move(layerMatArr));

    // WHAT: heightData は正負の正規化高さ [-1, 1] をそのまま保存する。
    // WHY: maxHeight を変更しても元データの相対形状を保てるため、編集途中の Terrain を再利用しやすい。
    toml::array heightArr;
    for (float h : tc.heightData)
        heightArr.push_back(static_cast<double>(h));
    terrainTbl.insert("heightData", std::move(heightArr));

    toml::array splatArr;
    const size_t expectedSplatSize = static_cast<size_t>(tc.columns) * static_cast<size_t>(tc.rows) * 4u;
    if (tc.splatData.size() == expectedSplatSize) {
        for (uint8_t s : tc.splatData)
            splatArr.push_back(static_cast<int64_t>(s));
    } else {
        // WHY: splatData が空の Terrain をそのまま保存すると、次回ロード時に PaintTool が
        //      レイヤー重みの前提を失うため、保存データ上も layer0=100% に正規化する。
        const size_t vertexCount = static_cast<size_t>(tc.columns) * static_cast<size_t>(tc.rows);
        for (size_t i = 0; i < vertexCount; ++i) {
            splatArr.push_back(int64_t{255});
            splatArr.push_back(int64_t{0});
            splatArr.push_back(int64_t{0});
            splatArr.push_back(int64_t{0});
        }
    }
    terrainTbl.insert("splatData", std::move(splatArr));

    return terrainTbl;
}

bool ReadTerrainData(const toml::table& terrainTbl, TerrainComponent& tc)
{
    tc.columns      = static_cast<int>(terrainTbl["columns"].value_or(int64_t{129}));
    tc.rows         = static_cast<int>(terrainTbl["rows"].value_or(int64_t{129}));
    tc.cellSize     = static_cast<float>(terrainTbl["cellSize"].value_or(1.0));
    tc.maxHeight    = static_cast<float>(terrainTbl["maxHeight"].value_or(30.0));
    tc.chunkSize    = static_cast<int>(terrainTbl["chunkSize"].value_or(int64_t{32}));
    if (const auto* layerArr = terrainTbl["layerMaterials"].as_array()) {
        for (int li = 0; li < 4 && li < static_cast<int>(layerArr->size()); ++li)
            tc.layerMaterials[li] = (*layerArr)[li].value_or(std::string{});
    }

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
    const size_t expectedSplatSize = static_cast<size_t>(tc.columns) * static_cast<size_t>(tc.rows) * 4u;
    if (tc.splatData.size() != expectedSplatSize)
        tc.InitDefaultSplat();

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

    // レイヤー .mat / ハイトマップ等の参照を guid: 形式で保存する (リネーム・移動耐性)。
    asset::EncodeGuidRefs(doc);

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
    // guid: 参照を "Assets/..." パスへ戻してから読む。
    asset::DecodeGuidRefs(doc);
    const toml::table* terrainTbl = doc["terrain"].as_table();
    if (!terrainTbl)
        terrainTbl = &doc;

    return ReadTerrainData(*terrainTbl, component);
}

} // namespace fbzz::scene
