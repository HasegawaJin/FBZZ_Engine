/// @file    FzTerrainSerializer.cpp
/// @brief   .terrain バイナリ (FZTN) の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-06-18
/// @see Docs/design/terrain-layers.md (§7 保存形式)
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Asset/FzTerrainFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr size_t SLOTS = static_cast<size_t>(scene::TERRAIN_SPLAT_SLOTS);

/// @brief POD 配列をそのままバイト列で書く。
template <class T>
void WriteArray(std::ofstream& f, const T* values, size_t count)
{
    if (count == 0) return;
    f.write(reinterpret_cast<const char*>(values), static_cast<std::streamsize>(count * sizeof(T)));
}

size_t CellCountOf(uint32_t columns, uint32_t rows)
{
    return columns > 1 && rows > 1 ? static_cast<size_t>(columns - 1) * static_cast<size_t>(rows - 1) : 0u;
}

/// @brief 範囲外を捨て、昇順・重複なしに揃えた穴セル列を返す。
std::vector<uint32_t> SanitizeHoles(const std::vector<uint32_t>& holes, size_t cellCount)
{
    std::vector<uint32_t> result;
    result.reserve(holes.size());
    for (uint32_t cell : holes)
        if (cell < cellCount) result.push_back(cell);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

/// @brief v1 の密な重み (index = 頂点 × layerCount + 層) を上位 4 層の正準形へ畳む。
/// @note Canonicalize は重みの上位 4 つだけで正規化するので、同じ順序 (重み降順・同重みは番号昇順)
///       で先に 4 つ選んでから渡しても結果は変わらない。層数が Canonicalize の作業枠を超えても畳める。
void FoldDenseWeights(const uint8_t* dense, uint32_t layerCount, size_t vertexCount,
                      std::vector<uint8_t>& outIndices, std::vector<uint8_t>& outWeights)
{
    outIndices.assign(vertexCount * SLOTS, 0u);
    outWeights.assign(vertexCount * SLOTS, 0u);
    struct Candidate { int layer; float weight; };
    std::vector<Candidate> candidates;
    candidates.reserve(layerCount);
    const auto before = [](const Candidate& a, const Candidate& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.layer < b.layer;
    };

    for (size_t v = 0; v < vertexCount; ++v) {
        candidates.clear();
        for (uint32_t li = 0; li < layerCount && li < static_cast<uint32_t>(scene::TERRAIN_MAX_LAYERS); ++li) {
            const uint8_t w = dense[v * layerCount + li];
            if (w > 0) candidates.push_back({ static_cast<int>(li), static_cast<float>(w) });
        }
        const size_t kept = (std::min)(candidates.size(), SLOTS);
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(kept),
                          candidates.end(), before);
        int layers[scene::TERRAIN_SPLAT_SLOTS]{};
        float weights[scene::TERRAIN_SPLAT_SLOTS]{};
        for (size_t i = 0; i < kept; ++i) {
            layers[i] = candidates[i].layer;
            weights[i] = candidates[i].weight;
        }
        scene::terrain_splat::Canonicalize(layers, weights, static_cast<int>(kept),
                                           &outIndices[v * SLOTS], &outWeights[v * SLOTS]);
    }
}

} // namespace

bool FzTerrainSerializer::Save(const TerrainAsset& asset, const std::string& absPath) const
{
    std::ofstream f(absPath, std::ios::binary);
    if (!f) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: cannot open for write [%s]", absPath.c_str());
        return false;
    }

    const uint32_t layerCount = static_cast<uint32_t>(
        (std::min)(asset.layerMaterialPaths.size(), static_cast<size_t>(scene::TERRAIN_MAX_LAYERS)));
    const size_t heightCount = static_cast<size_t>(asset.columns) * asset.rows;
    const std::vector<uint32_t> holes = SanitizeHoles(asset.holeCells, CellCountOf(asset.columns, asset.rows));

    FzTerrainHeader hdr{};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'Z'; hdr.magic[2] = 'T'; hdr.magic[3] = 'N';
    hdr.version          = FZTERRAIN_VERSION;
    hdr.columns          = asset.columns;
    hdr.rows             = asset.rows;
    hdr.cellSize         = asset.cellSize;
    hdr.maxHeight        = asset.maxHeight;
    hdr.chunkSize        = asset.chunkSize;
    hdr.layerCount       = layerCount;
    hdr.holeCount        = static_cast<uint32_t>(holes.size());
    hdr.heightBlendDepth = asset.heightBlendDepth;
    WriteArray(f, &hdr, 1);

    for (uint32_t li = 0; li < layerCount; ++li) {
        char pathBuf[FZTERRAIN_PATH_LEN]{};
        const auto& p = asset.layerMaterialPaths[li];
        const size_t len = (std::min)(p.size(), static_cast<size_t>(FZTERRAIN_PATH_LEN - 1));
        std::memcpy(pathBuf, p.data(), len);
        f.write(pathBuf, FZTERRAIN_PATH_LEN);
    }

    if (asset.heightData.size() >= heightCount) {
        WriteArray(f, asset.heightData.data(), heightCount);
    } else {
        /// @note 不完全な TerrainAsset を保存してもロード不能な .terrain にしないため、
        ///       欠けた高さはフラット地形として 0 で埋める。
        const std::vector<float> defaultHeights(heightCount, 0.0f);
        WriteArray(f, defaultHeights.data(), heightCount);
    }

    const size_t slotCount = heightCount * SLOTS;
    if (asset.splatIndices.size() == slotCount && asset.splatWeights.size() == slotCount) {
        WriteArray(f, asset.splatIndices.data(), slotCount);
        WriteArray(f, asset.splatWeights.data(), slotCount);
    } else {
        /// @note 塗りは全頂点に正準形のスプラットがある前提なので、欠けていれば «層 0 = 100%» で書く。
        const std::vector<uint8_t> defaultIndices(slotCount, 0u);
        std::vector<uint8_t> defaultWeights(slotCount, 0u);
        for (size_t i = 0; i < slotCount; i += SLOTS)
            defaultWeights[i] = 255u;
        WriteArray(f, defaultIndices.data(), slotCount);
        WriteArray(f, defaultWeights.data(), slotCount);
    }

    WriteArray(f, holes.data(), holes.size());

    return f.good();
}

bool FzTerrainSerializer::Load(const std::string& absPath, TerrainAsset& outAsset) const
{
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
    if (!f) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: cannot open [%s]", absPath.c_str());
        return false;
    }

    const auto fileSize = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> data(fileSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!f.good() && !f.eof()) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: read failed [%s]", absPath.c_str());
        return false;
    }

    size_t pos = 0;
    /// @note 壊れたヘッダーの巨大な寸法で確保しないよう、確保の前に残りバイト数で弾く。
    const auto fits = [&](size_t count, size_t elementSize) {
        return elementSize == 0 || count <= (data.size() - pos) / elementSize;
    };
    const auto readBytes = [&](void* dst, size_t bytes) -> bool {
        if (bytes > data.size() - pos) return false;
        std::memcpy(dst, data.data() + pos, bytes);
        pos += bytes;
        return true;
    };

    FzTerrainHeader hdr{};
    if (!readBytes(&hdr, sizeof(hdr)) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'T' || hdr.magic[3] != 'N') {
        FBZZ_LOG_ERROR("FzTerrainSerializer: bad magic [%s]", absPath.c_str());
        return false;
    }
    if (hdr.version < 1 || hdr.version > FZTERRAIN_VERSION) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: unsupported version %u [%s]", hdr.version, absPath.c_str());
        return false;
    }

    outAsset.columns          = hdr.columns;
    outAsset.rows             = hdr.rows;
    outAsset.cellSize         = hdr.cellSize;
    outAsset.maxHeight        = hdr.maxHeight;
    outAsset.chunkSize        = hdr.chunkSize;
    outAsset.heightBlendDepth = hdr.version >= 2 ? hdr.heightBlendDepth : 0.2f;

    if (!fits(hdr.layerCount, FZTERRAIN_PATH_LEN)) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: truncated layer paths [%s]", absPath.c_str());
        return false;
    }
    const uint32_t keptLayers = (std::min)(hdr.layerCount, static_cast<uint32_t>(scene::TERRAIN_MAX_LAYERS));
    outAsset.layerMaterialPaths.assign(keptLayers, std::string{});
    for (uint32_t li = 0; li < hdr.layerCount; ++li) {
        char pathBuf[FZTERRAIN_PATH_LEN + 1]{};
        if (!readBytes(pathBuf, FZTERRAIN_PATH_LEN)) return false;
        if (li < keptLayers) outAsset.layerMaterialPaths[li] = pathBuf;
    }

    const size_t heightCount = static_cast<size_t>(hdr.columns) * hdr.rows;
    if (!fits(heightCount, sizeof(float))) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: truncated height data [%s]", absPath.c_str());
        return false;
    }
    outAsset.heightData.resize(heightCount);
    if (!readBytes(outAsset.heightData.data(), heightCount * sizeof(float))) return false;

    outAsset.holeCells.clear();
    if (hdr.version == 1) {
        if (hdr.layerCount != 0 && !fits(heightCount, hdr.layerCount)) {
            FBZZ_LOG_ERROR("FzTerrainSerializer: truncated splat data [%s]", absPath.c_str());
            return false;
        }
        std::vector<uint8_t> dense(heightCount * hdr.layerCount);
        if (!readBytes(dense.data(), dense.size())) return false;
        FoldDenseWeights(dense.data(), hdr.layerCount, heightCount, outAsset.splatIndices, outAsset.splatWeights);
        return true;
    }

    const size_t slotCount = heightCount * SLOTS;
    if (!fits(slotCount, 2u)) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: truncated splat data [%s]", absPath.c_str());
        return false;
    }
    outAsset.splatIndices.resize(slotCount);
    outAsset.splatWeights.resize(slotCount);
    if (!readBytes(outAsset.splatIndices.data(), slotCount) ||
        !readBytes(outAsset.splatWeights.data(), slotCount))
        return false;
    /// @note 読んだ値を信用せず正準形へ直す。番号が層数以上でも残す (描画が既定層で補う)。
    for (size_t base = 0; base < slotCount; base += SLOTS)
        scene::terrain_splat::Canonicalize(&outAsset.splatIndices[base], &outAsset.splatWeights[base]);

    if (!fits(hdr.holeCount, sizeof(uint32_t))) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: truncated hole data [%s]", absPath.c_str());
        return false;
    }
    std::vector<uint32_t> holes(hdr.holeCount);
    if (!holes.empty() && !readBytes(holes.data(), holes.size() * sizeof(uint32_t))) return false;
    outAsset.holeCells = SanitizeHoles(holes, CellCountOf(hdr.columns, hdr.rows));
    return true;
}

} // namespace fbzz::asset
