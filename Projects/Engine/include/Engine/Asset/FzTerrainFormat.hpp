/// @file    FzTerrainFormat.hpp
/// @brief   .terrain バイナリ (FZTN) のオンディスクレイアウト定義。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note v2 のレイアウト: FzTerrainHeader → layerCount × FZTERRAIN_PATH_LEN (null 終端パス)
///       → columns × rows × float (heightData) → columns × rows × 4 (splatIndices)
///       → columns × rows × 4 (splatWeights) → holeCount × uint32 (穴セルの添字)。
/// @note v1 はパスと高さの後に columns × rows × layerCount の密な重み
///       (index = (z * columns + x) * layerCount + 層) が続き、穴は無い。
/// @see Docs/design/terrain-layers.md (§7 保存形式)
#pragma once
#include <cstdint>

namespace fbzz::asset {

/// @brief 書き出す版。読み込みは 1 と 2 を受け付ける。
constexpr uint32_t FZTERRAIN_VERSION  = 2;
constexpr uint32_t FZTERRAIN_PATH_LEN = 256;

struct FzTerrainHeader {
    char     magic[4];          ///< "FZTN"
    uint32_t version;
    uint32_t columns;
    uint32_t rows;
    float    cellSize;
    float    maxHeight;
    uint32_t chunkSize;
    uint32_t layerCount;
    uint32_t holeCount;         ///< v2 から。v1 では予約領域 (0)
    float    heightBlendDepth;  ///< v2 から。v1 では予約領域 (0)
    uint32_t _pad[2];
};
/// @note v1 の _pad[4] を holeCount / heightBlendDepth に割り当てたので大きさは変わらない。
static_assert(sizeof(FzTerrainHeader) == 48, "FzTerrainHeader size mismatch");

} // namespace fbzz::asset
