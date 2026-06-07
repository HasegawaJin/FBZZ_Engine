// FBZZ Engine
// MaterialInspectorWidgets.hpp | fbzz::editor
// .fzmat 用 Inspector ウィジェット群
#pragma once

#include <Engine/Asset/MaterialAsset.hpp>

namespace fbzz::editor {

// DrawMaterialTextureField — Drag & Drop 対応の texture path 入力欄を描画する。
// WHY: Terrain / Water / 汎用 Material で同じ UI とパス正規化を使い、パネル間の挙動差をなくす。
bool DrawMaterialTextureField(asset::MaterialAsset& mat, const char* label, const char* key);

// DrawTerrainMaterialInspector — Terrain 専用 .fzmat の layer パラメータを編集する。
// WHY: 汎用 float 一覧では採用担当者・利用者が各値の意味を追いにくいため、用途単位で整理する。
bool DrawTerrainMaterialInspector(asset::MaterialAsset& mat);

// DrawWaterMaterialInspector — Water 専用 .fzmat の見た目パラメータを編集する。
// WHY: 水面は色・法線・泡・flow・caustics の調整頻度が高く、意味別のまとまりが必要。
bool DrawWaterMaterialInspector(asset::MaterialAsset& mat);

} // namespace fbzz::editor
