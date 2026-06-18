// FBZZ Engine
// FzTerrainSerializer.cpp | fbzz::asset
// .terrain バイナリの読み書き
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Asset/FzTerrainFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace fbzz::asset {

bool FzTerrainSerializer::Save(const TerrainAsset& asset, const std::string& absPath) const
{
    std::ofstream f(absPath, std::ios::binary);
    if (!f) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: cannot open for write [%s]", absPath.c_str());
        return false;
    }

    const uint32_t layerCount = 4;

    FzTerrainHeader hdr{};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'Z'; hdr.magic[2] = 'T'; hdr.magic[3] = 'N';
    hdr.version    = FZTERRAIN_VERSION;
    hdr.columns    = asset.columns;
    hdr.rows       = asset.rows;
    hdr.cellSize   = asset.cellSize;
    hdr.maxHeight  = asset.maxHeight;
    hdr.chunkSize  = asset.chunkSize;
    hdr.layerCount = layerCount;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // レイヤーマテリアルパス (固定 4 スロット)
    for (uint32_t li = 0; li < layerCount; ++li) {
        char pathBuf[FZTERRAIN_PATH_LEN]{};
        if (li < std::size(asset.layerMaterialPaths)) {
            const auto& p = asset.layerMaterialPaths[li];
            const size_t len = std::min(p.size(), static_cast<size_t>(FZTERRAIN_PATH_LEN - 1));
            std::memcpy(pathBuf, p.data(), len);
        }
        f.write(pathBuf, FZTERRAIN_PATH_LEN);
    }

    const size_t heightCount = static_cast<size_t>(asset.columns) * asset.rows;
    if (asset.heightData.size() >= heightCount) {
        f.write(reinterpret_cast<const char*>(asset.heightData.data()),
                static_cast<std::streamsize>(heightCount * sizeof(float)));
    } else {
        // WHY: 不完全な TerrainAsset を保存してもロード不能な .terrain にしないため、
        //      欠けた高さはフラット地形として 0 で埋める。
        const std::vector<float> defaultHeights(heightCount, 0.0f);
        f.write(reinterpret_cast<const char*>(defaultHeights.data()),
                static_cast<std::streamsize>(heightCount * sizeof(float)));
    }

    const size_t splatCount = heightCount * layerCount;
    if (asset.splatData.size() >= splatCount) {
        f.write(reinterpret_cast<const char*>(asset.splatData.data()),
                static_cast<std::streamsize>(splatCount));
    } else {
        // WHY: PaintTool は各頂点に RGBA の重みが必ず存在する前提で編集するため、
        //      未初期化データは layer0=100% の正規化済み splat として保存する。
        std::vector<uint8_t> defaultSplat(splatCount, 0u);
        for (size_t i = 0; i < heightCount; ++i)
            defaultSplat[i * layerCount] = 255u;
        f.write(reinterpret_cast<const char*>(defaultSplat.data()),
                static_cast<std::streamsize>(splatCount));
    }

    return f.good();
}

bool FzTerrainSerializer::Load(const std::string& absPath, TerrainAsset& outAsset) const
{
    // TerrainImporter と同じロジック。Serializer 単体でも使えるようここに読み取り処理を持つ。
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
    if (!f) {
        FBZZ_LOG_ERROR("FzTerrainSerializer: cannot open [%s]", absPath.c_str());
        return false;
    }

    const auto fileSize = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> data(fileSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!f.good() && !f.eof()) return false;

    size_t pos = 0;
    auto readBytes = [&](void* dst, size_t bytes) -> bool {
        if (pos + bytes > data.size()) return false;
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

    outAsset.columns   = hdr.columns;
    outAsset.rows      = hdr.rows;
    outAsset.cellSize  = hdr.cellSize;
    outAsset.maxHeight = hdr.maxHeight;
    outAsset.chunkSize = hdr.chunkSize;

    const uint32_t layerCount = std::min(hdr.layerCount, 4u);
    for (uint32_t li = 0; li < layerCount; ++li) {
        char pathBuf[FZTERRAIN_PATH_LEN]{};
        if (!readBytes(pathBuf, FZTERRAIN_PATH_LEN)) return false;
        if (li < std::size(outAsset.layerMaterialPaths))
            outAsset.layerMaterialPaths[li] = pathBuf;
    }
    for (uint32_t li = layerCount; li < hdr.layerCount; ++li) {
        if (pos + FZTERRAIN_PATH_LEN > data.size()) return false;
        pos += FZTERRAIN_PATH_LEN;
    }

    const size_t heightCount = static_cast<size_t>(hdr.columns) * hdr.rows;
    outAsset.heightData.resize(heightCount);
    if (!readBytes(outAsset.heightData.data(), heightCount * sizeof(float))) return false;

    const size_t splatCount = heightCount * hdr.layerCount;
    outAsset.splatData.resize(splatCount);
    if (!readBytes(outAsset.splatData.data(), splatCount)) return false;

    return true;
}

} // namespace fbzz::asset
