/// @file    TerrainHeightMapLoader.hpp
/// @brief   画像 (PNG / TGA / DDS) と TerrainComponent::heightData を相互に変換する。
/// @author  Hasegawa Jin
/// @date    2026-06-14
///
/// @note 取り込み後は terrain.heightDirty と colliderDirty を立てること。
/// @note 画像サイズが terrain.columns × terrain.rows と違えば三次補間でリサイズする。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <string>

namespace fbzz::scene {

struct TerrainComponent;

/// @brief 画像の R チャンネルを heightData へ読み込む。
/// @param unipolar true なら画素値 [0,1] を heightData [0,1] へ (黒=基準面)。false なら [-1,1] へ (灰=基準面)。
/// @return 読み込みに失敗したら false。
bool LoadHeightMapFromFile(
    const std::string&  path,
    TerrainComponent&   terrain,
    bool                unipolar = true);

/// @brief heightData を columns × rows の 16 bit グレースケール PNG へ書き出す。
/// @param unipolar LoadHeightMapFromFile と対称。true なら [0,1] をそのまま (負の高さは 0 に切り詰める)、false なら [-1,1] を [0,1] へ写す。
/// @return 書き込みに失敗したら false。
bool SaveHeightMapToFile(
    const std::string&       path,
    const TerrainComponent&  terrain,
    bool                     unipolar = true);

} // namespace fbzz::scene
