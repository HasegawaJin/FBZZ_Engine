// FBZZ Engine
// TerrainAsset.hpp | fbzz::asset
// TerrainComponent の高さ・スプラット・レイヤー設定を .terrain バイナリで管理する独立型
// WHY: TerrainComponent から切り離すことで AssetManager のキャッシュ・FlushFailed が使える。
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

struct TerrainAsset {
    uint32_t                   columns   = 129;
    uint32_t                   rows      = 129;
    float                      cellSize  = 1.0f;
    float                      maxHeight = 30.0f;
    uint32_t                   chunkSize = 32;

    // 各レイヤーが参照する .mat のパス（assets/ 相対）
    std::array<std::string, 4> layerMaterialPaths;

    // row-major: index = z * columns + x, 値域 [-1, 1]
    std::vector<float>         heightData;

    // index = (z * columns + x) * 4 + ch (0=R 1=G 2=B 3=A), 値域 [0, 255]
    std::vector<uint8_t>       splatData;
};

} // namespace fbzz::asset
