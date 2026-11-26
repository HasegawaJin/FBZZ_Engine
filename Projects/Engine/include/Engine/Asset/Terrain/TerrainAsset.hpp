/// @file    TerrainAsset.hpp
/// @brief   TerrainComponent の高さ・スプラット・レイヤー設定を .terrain バイナリで管理する独立型。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note TerrainComponent から切り離すことで AssetManager のキャッシュ・FlushFailed が使える。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <cstdint>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <string>
#include <vector>

namespace fbzz::asset {

struct TerrainAsset {
    uint32_t                   columns   = 65;
    uint32_t                   rows      = 65;
    float                      cellSize  = 2.0f;
    float                      maxHeight = 20.0f;
    uint32_t                   chunkSize = 32;
    float                      heightBlendDepth = 0.2f; ///< 高さブレンドの幅。TerrainComponent::heightBlendDepth と同じ意味。

    std::vector<std::string>   layerMaterialPaths = std::vector<std::string>(4); ///< 層ごとの .mat のパス (assets/ 相対)。添字がスプラットの番号。

    std::vector<float>         heightData; ///< row-major: index = z * columns + x、値域 [-1, 1]。

    std::vector<uint8_t>       splatIndices; ///< index = (z * columns + x) * 4 + slot。層番号。正準形 (terrain_splat) を保つ。
    std::vector<uint8_t>       splatWeights; ///< splatIndices と同じ並びの重み [0, 255]。

    std::vector<uint32_t>      holeCells; ///< 穴セルの添字 (cz * (columns - 1) + cx) の昇順列。

    std::vector<math::Vector3> surfacePositions; ///< 地形ローカル空間の表面形状。頂点の巻き順と検証規則は TerrainComponent と同じ。
    std::vector<uint32_t> surfaceIndices;
    std::string surfaceMaterial;
};

} // namespace fbzz::asset
