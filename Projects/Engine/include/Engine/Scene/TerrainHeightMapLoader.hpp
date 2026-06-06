#pragma once
// FBZZ Engine
// TerrainHeightMapLoader.hpp | fbzz::scene
// 画像ファイル (PNG / TGA / DDS) を読み込み、TerrainComponent::heightData に変換する。
//
// 使い方:
//   bool ok = LoadHeightMapFromFile("assets/terrain/myheight.png", terrain, /*unipolar=*/true);
//   if (ok) { terrain.heightDirty = true; terrain.colliderDirty = true; }
//
// 正規化モード:
//   unipolar=true  : 画素値 [0, 1] → heightData [0, 1]  (黒=基準面、白=maxHeight 上)
//   unipolar=false : 画素値 [0, 1] → heightData [-1, 1] (黒=-maxH、灰=基準面、白=+maxH)
//
// サイズ不一致:
//   画像サイズが terrain.columns × terrain.rows と異なる場合、
//   三次補間 (Cubic) でリサイズして取り込む。
#include <string>

namespace fbzz::scene {

struct TerrainComponent;

// 戻り値: 成功 = true, 失敗 (ファイル読み込みエラー等) = false
bool LoadHeightMapFromFile(
    const std::string&  path,
    TerrainComponent&   terrain,
    bool                unipolar = true);

} // namespace fbzz::scene
