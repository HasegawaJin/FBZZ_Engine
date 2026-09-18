/// @file    TerrainAsset.hpp
/// @brief   TerrainComponent の高さ・スプラット・レイヤー設定を .terrain バイナリで管理する独立型。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note TerrainComponent から切り離すことで AssetManager のキャッシュ・FlushFailed が使える。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

struct TerrainAsset {
    uint32_t                   columns   = 65;
    uint32_t                   rows      = 65;
    float                      cellSize  = 2.0f;
    float                      maxHeight = 20.0f;
    uint32_t                   chunkSize = 32;
    /// @brief 高さブレンドの幅。TerrainComponent::heightBlendDepth と同じ意味。
    float                      heightBlendDepth = 0.2f;

    /// @brief 層ごとの .mat のパス (assets/ 相対)。添字がスプラットの番号。
    std::vector<std::string>   layerMaterialPaths = std::vector<std::string>(4);

    /// @brief row-major: index = z * columns + x、値域 [-1, 1]。
    std::vector<float>         heightData;

    /// @brief index = (z * columns + x) * 4 + slot。層番号。正準形 (terrain_splat) を保つ。
    std::vector<uint8_t>       splatIndices;
    /// @brief splatIndices と同じ並びの重み [0, 255]。
    std::vector<uint8_t>       splatWeights;

    /// @brief 穴セルの添字 (cz * (columns - 1) + cx) の昇順列。
    std::vector<uint32_t>      holeCells;
};

} // namespace fbzz::asset
