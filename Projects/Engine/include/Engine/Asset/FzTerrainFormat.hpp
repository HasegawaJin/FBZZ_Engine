/// @file    FzTerrainFormat.hpp
/// @brief   .terrain バイナリのオンディスクレイアウト定義。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 旧 TerrainAssetSerializer (TOML) をバイナリに置き換える。
/// 129×129 float 配列は TOML だと ~200KB → バイナリで ~66KB に削減。
///
/// レイアウト:
/// FzTerrainHeader
/// layerMaterialPaths: layerCount × FZTERRAIN_PATH_LEN バイト (null 終端)
/// heightData:         columns × rows × sizeof(float)
/// splatData:          columns × rows × layerCount × sizeof(uint8_t)
#pragma once
#include <cstdint>

namespace fbzz::asset {

constexpr uint32_t FZTERRAIN_VERSION  = 1;
constexpr uint32_t FZTERRAIN_PATH_LEN = 256;

struct FzTerrainHeader {
    char     magic[4];          ///< "FZTN"
    uint32_t version;
    uint32_t columns;
    uint32_t rows;
    float    cellSize;
    float    maxHeight;
    uint32_t chunkSize;
    uint32_t layerCount;        ///< 現状 4 固定、将来的に可変
    uint32_t _pad[4];
};
static_assert(sizeof(FzTerrainHeader) == 48, "FzTerrainHeader size mismatch");
/// 直後: layerCount × FZTERRAIN_PATH_LEN バイトのレイヤーマテリアルパス
/// 直後: columns × rows × float の heightData
/// 直後: columns × rows × layerCount × uint8 の splatData

} // namespace fbzz::asset
