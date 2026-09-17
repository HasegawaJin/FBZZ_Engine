/// @file    TerrainHeightMapLoader.hpp
/// @brief   画像 (PNG / TGA / DDS) を TerrainComponent::heightData へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-06-14
///
/// 取り込み後は terrain.heightDirty と colliderDirty を立てること。
/// 画像サイズが terrain.columns × terrain.rows と違えば三次補間でリサイズする。
#pragma once
#include <string>

namespace fbzz::scene {

struct TerrainComponent;

/// @param unipolar true なら画素値 [0,1] を heightData [0,1] へ (黒=基準面)。
///        false なら [-1,1] へ (黒=-maxHeight、灰=基準面、白=+maxHeight)。
/// @return 読み込みに失敗したら false。
bool LoadHeightMapFromFile(
    const std::string&  path,
    TerrainComponent&   terrain,
    bool                unipolar = true);

} // namespace fbzz::scene
