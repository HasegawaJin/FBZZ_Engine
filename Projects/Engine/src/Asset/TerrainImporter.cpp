/// @file    TerrainImporter.cpp
/// @brief   .terrain バイナリ → TerrainAsset デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/FzTerrainFormat.hpp>
#include <Engine/Asset/TerrainImporter.hpp>
#include <Engine/Core/Logger.hpp>
#include <cstring>
#include <fstream>
#include <vector>

namespace fbzz::asset {

std::unique_ptr<TerrainAsset> TerrainImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
    if (!f) {
        FBZZ_LOG_ERROR("TerrainImporter: cannot open [%s]", absPath.c_str());
        return nullptr;
    }

    const auto fileSize = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> data(fileSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!f.good() && !f.eof()) {
        FBZZ_LOG_ERROR("TerrainImporter: read failed [%s]", absPath.c_str());
        return nullptr;
    }

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
        FBZZ_LOG_ERROR("TerrainImporter: bad magic [%s]", absPath.c_str());
        return nullptr;
    }

    auto terrain = std::make_unique<TerrainAsset>();
    terrain->columns  = hdr.columns;
    terrain->rows     = hdr.rows;
    terrain->cellSize = hdr.cellSize;
    terrain->maxHeight = hdr.maxHeight;
    terrain->chunkSize = hdr.chunkSize;

    /// @note レイヤーマテリアルパス
    const uint32_t layerCount = std::min(hdr.layerCount, 4u);
    for (uint32_t li = 0; li < layerCount; ++li) {
        char pathBuf[FZTERRAIN_PATH_LEN]{};
        if (!readBytes(pathBuf, FZTERRAIN_PATH_LEN)) return nullptr;
        if (li < 4) terrain->layerMaterialPaths[li] = pathBuf;
    }
    /// @note 追加レイヤー (layerCount > 4) はスキップ
    for (uint32_t li = layerCount; li < hdr.layerCount; ++li) {
        if (pos + FZTERRAIN_PATH_LEN > data.size()) return nullptr;
        pos += FZTERRAIN_PATH_LEN;
    }

    /// @note 高さデータ
    const size_t heightCount = static_cast<size_t>(hdr.columns) * hdr.rows;
    terrain->heightData.resize(heightCount);
    if (!readBytes(terrain->heightData.data(), heightCount * sizeof(float))) {
        FBZZ_LOG_ERROR("TerrainImporter: truncated height data [%s]", absPath.c_str());
        return nullptr;
    }

    /// @note スプラットデータ
    const size_t splatCount = heightCount * hdr.layerCount;
    terrain->splatData.resize(splatCount);
    if (!readBytes(terrain->splatData.data(), splatCount)) {
        FBZZ_LOG_ERROR("TerrainImporter: truncated splat data [%s]", absPath.c_str());
        return nullptr;
    }

    return terrain;
}

} // namespace fbzz::asset
