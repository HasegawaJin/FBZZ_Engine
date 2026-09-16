/// @file    MaterialInspectorWidgets.hpp
/// @brief   .mat 用 Inspector ウィジェット群。
/// @author  Hasegawa Jin
/// @date    2026-06-07
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

// DrawWaterMaterialInspector — Water 専用 .mat (水の種類) を編集する。
// 見た目 (色・反射・さざ波・泡) に加え、Gerstner 波・環境風への反応・水流もここで持つ。
// WHY: 水面は調整頻度が高く、汎用 float 一覧では «どれが波でどれが色か» を追えない。
bool DrawWaterMaterialInspector(asset::MaterialAsset& mat);

} // namespace fbzz::editor
