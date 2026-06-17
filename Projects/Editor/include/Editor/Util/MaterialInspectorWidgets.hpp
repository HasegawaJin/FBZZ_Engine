// FBZZ Engine
// MaterialInspectorWidgets.hpp | fbzz::editor
// .mat 用 Inspector ウィジェット群
#pragma once

#include <Engine/Asset/MaterialAsset.hpp>

namespace fbzz::editor {

// DrawMaterialTextureField — Drag & Drop 対応の texture path 入力欄を描画する。
// WHY: Terrain / Water / 汎用 Material で同じ UI とパス正規化を使い、パネル間の挙動差をなくす。
bool DrawMaterialTextureField(asset::MaterialAsset& mat, const char* label, const char* key);

// DrawTerrainLayerMaterialInspector — Terrain レイヤー用 .mat のパラメータを編集する。
// プレフィックスなし ("diffuse", "tilingX" 等) で各レイヤーが独立した fzmat を持つ設計に対応。
// WHY: 汎用 float 一覧では各値の意味を追いにくいため、用途単位で整理する。
// textureDirty = テクスチャパス変更（レイヤーテクスチャ再ロード要）
// paramDirty   = float/bool パラメータ変更（CB 再構築のみ、スプラットマップ再アップロード不要）
struct TerrainLayerDirtyFlags { bool textureDirty = false; bool paramDirty = false; };
TerrainLayerDirtyFlags DrawTerrainLayerMaterialInspector(asset::MaterialAsset& mat);

// DrawWaterMaterialInspector — Water 専用 .mat の見た目パラメータを編集する。
// WHY: 水面は色・法線・泡・flow・caustics の調整頻度が高く、意味別のまとまりが必要。
bool DrawWaterMaterialInspector(asset::MaterialAsset& mat);

} // namespace fbzz::editor
