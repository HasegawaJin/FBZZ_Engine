/// @file    TerrainAssetSerializer.cpp
/// @brief   TerrainComponent の外部アセット保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @see Docs/design/terrain-layers.md (§7 保存形式)
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace fbzz::scene {
namespace {

/// @brief 現在書き出す .terrain の版。1 = RGBA 4 層 (splatData)、2 = 可変層 (splatIndices/splatWeights/holes)。
constexpr int64_t TERRAIN_FORMAT_VERSION = 2;

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

std::uint8_t ReadByte(const toml::node& node)
{
    return static_cast<std::uint8_t>(std::clamp<int64_t>(node.value_or(int64_t{0}), 0, 255));
}

/// @brief TOML の整数配列を uint8 列として読む。
/// @return 配列が無い、または長さが expected と違えば false。out は未定義。
bool ReadByteArray(const toml::table& tbl, const char* key, size_t expected, std::vector<std::uint8_t>& out)
{
    const auto* arr = tbl[key].as_array();
    if (!arr || arr->size() != expected) return false;
    out.resize(expected);
    for (size_t i = 0; i < expected; ++i)
        out[i] = ReadByte((*arr)[i]);
    return true;
}

toml::table WriteTerrainData(const TerrainComponent& tc)
{
    toml::table terrainTbl;
    terrainTbl.insert("columns",          static_cast<int64_t>(tc.columns));
    terrainTbl.insert("rows",             static_cast<int64_t>(tc.rows));
    terrainTbl.insert("cellSize",         static_cast<double>(tc.cellSize));
    terrainTbl.insert("maxHeight",        static_cast<double>(tc.maxHeight));
    terrainTbl.insert("chunkSize",        static_cast<int64_t>(tc.chunkSize));
    terrainTbl.insert("heightBlendDepth", static_cast<double>(tc.heightBlendDepth));
    toml::array layerMatArr;
    for (const auto& p : tc.layerMaterials)
        layerMatArr.push_back(p);
    terrainTbl.insert("layerMaterials", std::move(layerMatArr));

    /// @note heightData は正負の正規化高さ [-1, 1] をそのまま保存する。maxHeight を変更しても
    ///       元データの相対形状を保てるため、編集途中の Terrain を再利用しやすい。
    toml::array heightArr;
    for (float h : tc.heightData)
        heightArr.push_back(static_cast<double>(h));
    terrainTbl.insert("heightData", std::move(heightArr));

    /// @note スプラットが頂点数と合わない Terrain は «全頂点が層 0 = 100%» として書く。
    ///       読み手が毎回同じ既定値を作り直すより、保存データ上も正準形にしておく。
    const size_t slotCount = tc.VertexCount() * static_cast<size_t>(TERRAIN_SPLAT_SLOTS);
    const bool validSplat = tc.HasValidSplat();
    toml::array indexArr;
    toml::array weightArr;
    indexArr.reserve(slotCount);
    weightArr.reserve(slotCount);
    for (size_t i = 0; i < slotCount; ++i) {
        const bool firstSlot = (i % static_cast<size_t>(TERRAIN_SPLAT_SLOTS)) == 0;
        indexArr.push_back(static_cast<int64_t>(validSplat ? tc.splatIndices[i] : 0u));
        weightArr.push_back(static_cast<int64_t>(validSplat ? tc.splatWeights[i] : (firstSlot ? 255u : 0u)));
    }
    terrainTbl.insert("splatIndices", std::move(indexArr));
    terrainTbl.insert("splatWeights", std::move(weightArr));

    /// @note 穴は昇順の疎な添字列。大半の地形は穴を持たないので、セル数ぶんの 0 を書かない。
    toml::array holeArr;
    if (tc.holeData.size() == tc.CellCount()) {
        for (size_t i = 0; i < tc.holeData.size(); ++i)
            if (tc.holeData[i] != 0) holeArr.push_back(static_cast<int64_t>(i));
    }
    terrainTbl.insert("holes", std::move(holeArr));

    return terrainTbl;
}

/// @brief v1 の splatData (頂点 × RGBA) を正準形へ移す。見た目は変わらない。
/// @return 長さが合わなければ false。
bool ReadLegacySplat(const toml::table& terrainTbl, TerrainComponent& tc)
{
    std::vector<std::uint8_t> rgba;
    const size_t slotCount = tc.VertexCount() * static_cast<size_t>(TERRAIN_SPLAT_SLOTS);
    if (slotCount == 0 || !ReadByteArray(terrainTbl, "splatData", slotCount, rgba)) return false;
    tc.splatIndices.resize(slotCount);
    tc.splatWeights.resize(slotCount);
    for (size_t base = 0; base < slotCount; base += TERRAIN_SPLAT_SLOTS)
        terrain_splat::FromLegacyRgba(&rgba[base], &tc.splatIndices[base], &tc.splatWeights[base]);
    return true;
}

/// @brief v2 の splatIndices / splatWeights を読み、全頂点を正準形へ直す。
/// @note 手編集や旧ツールの出力でも不変条件を崩さないため、読んだ値を信用せず畳み直す。
///       層数以上の番号は残す (描画が既定層で補う)。
bool ReadSplat(const toml::table& terrainTbl, TerrainComponent& tc)
{
    const size_t slotCount = tc.VertexCount() * static_cast<size_t>(TERRAIN_SPLAT_SLOTS);
    if (slotCount == 0) return false;
    if (!ReadByteArray(terrainTbl, "splatIndices", slotCount, tc.splatIndices) ||
        !ReadByteArray(terrainTbl, "splatWeights", slotCount, tc.splatWeights))
        return false;
    for (size_t base = 0; base < slotCount; base += TERRAIN_SPLAT_SLOTS)
        terrain_splat::Canonicalize(&tc.splatIndices[base], &tc.splatWeights[base]);
    return true;
}

void ReadHoles(const toml::table& terrainTbl, TerrainComponent& tc)
{
    tc.holeData.clear();
    const auto* holeArr = terrainTbl["holes"].as_array();
    const size_t cellCount = tc.CellCount();
    if (!holeArr || holeArr->empty() || cellCount == 0) return;

    tc.holeData.assign(cellCount, 0u);
    size_t set = 0;
    for (const auto& node : *holeArr) {
        const int64_t cell = node.value_or(int64_t{-1});
        if (cell < 0 || static_cast<uint64_t>(cell) >= cellCount) continue;
        tc.holeData[static_cast<size_t>(cell)] = 1u;
        ++set;
    }
    if (set == 0) tc.holeData.clear();
}

bool ReadTerrainData(const toml::table& terrainTbl, int64_t formatVersion, TerrainComponent& tc)
{
    tc.columns          = static_cast<int>(terrainTbl["columns"].value_or(int64_t{129}));
    tc.rows             = static_cast<int>(terrainTbl["rows"].value_or(int64_t{129}));
    tc.cellSize         = static_cast<float>(terrainTbl["cellSize"].value_or(1.0));
    tc.maxHeight        = static_cast<float>(terrainTbl["maxHeight"].value_or(30.0));
    tc.chunkSize        = static_cast<int>(terrainTbl["chunkSize"].value_or(int64_t{32}));
    tc.heightBlendDepth = static_cast<float>(terrainTbl["heightBlendDepth"].value_or(0.2));
    if (const auto* layerArr = terrainTbl["layerMaterials"].as_array()) {
        const size_t layerCount = (std::min)(layerArr->size(), static_cast<size_t>(TERRAIN_MAX_LAYERS));
        tc.layerMaterials.resize(layerCount);
        for (size_t li = 0; li < layerCount; ++li)
            tc.layerMaterials[li] = (*layerArr)[li].value_or(std::string{});
    }

    tc.heightData.clear();
    if (auto* heightArr = terrainTbl["heightData"].as_array()) {
        tc.heightData.reserve(heightArr->size());
        for (auto& v : *heightArr)
            tc.heightData.push_back(static_cast<float>(v.value_or(0.0)));
    }
    if (tc.heightData.size() != tc.VertexCount()) {
        if (!tc.heightData.empty())
            FBZZ_LOG_WARN("TerrainAssetSerializer: heightData size %zu != %zu, reset to flat",
                          tc.heightData.size(), tc.VertexCount());
        tc.heightData.assign(tc.VertexCount(), 0.0f);
    }

    /// @note v1、または splatIndices を持たないファイルは RGBA 4 層 (splatData) として読む。
    ///       版番号だけで決めないのは、版を書き忘れた手編集ファイルでも中身で判断するため。
    const bool legacy = formatVersion < 2 ? !terrainTbl.contains("splatIndices")
                                          : !terrainTbl.contains("splatIndices") && terrainTbl.contains("splatData");
    const bool splatOk = legacy ? ReadLegacySplat(terrainTbl, tc) : ReadSplat(terrainTbl, tc);
    if (!splatOk)
        tc.InitDefaultSplat();

    ReadHoles(terrainTbl, tc);

    tc.heightDirty = true;
    tc.splatDirty = true;
    tc.materialParamDirty = true;
    tc.colliderDirty = true;
    return true;
}

} // namespace

bool TerrainAssetSerializer::Save(const TerrainComponent& component, const std::string& path)
{
    if (path.empty()) return false;

    toml::table doc;
    toml::table metaTbl;
    metaTbl.insert("format_version", TERRAIN_FORMAT_VERSION);
    doc.insert("terrain_asset", std::move(metaTbl));
    doc.insert("terrain", WriteTerrainData(component));

    NormalizeTomlFloats(doc);

    /// @note レイヤー .mat / ハイトマップ等の参照を guid: 形式で保存する (リネーム・移動耐性)。
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
    /// @note guid: 参照を "Assets/..." パスへ戻してから読む。
    asset::DecodeGuidRefs(doc);
    const toml::table* terrainTbl = doc["terrain"].as_table();
    if (!terrainTbl)
        terrainTbl = &doc;
    const int64_t formatVersion = doc["terrain_asset"]["format_version"].value_or(int64_t{1});

    return ReadTerrainData(*terrainTbl, formatVersion, component);
}

} // namespace fbzz::scene
