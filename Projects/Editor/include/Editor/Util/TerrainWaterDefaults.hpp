// FBZZ Engine
// TerrainWaterDefaults.hpp | fbzz::editor
// Terrain / Water の Editor 既定アセットパスを集約する
#pragma once

namespace fbzz::editor {

// Terrain レイヤー [0-3] の既定 Material パスを返す。
// WHY: InspectorPanel にパス文字列を散らすと、アセット構成変更時に見落としやすいため。
[[nodiscard]] const char* DefaultTerrainLayerMaterialPath(int layerIndex);

// WaterComponent を追加・初期化するときに使う標準 Material パスを返す。
// WHY: Water.fzmat の場所を UI 実装から分離し、テンプレート/生成処理と共有しやすくするため。
[[nodiscard]] const char* DefaultWaterMaterialPath();

} // namespace fbzz::editor
