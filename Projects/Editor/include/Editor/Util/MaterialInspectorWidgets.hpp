/// @file    MaterialInspectorWidgets.hpp
/// @brief   .mat 用 Inspector ウィジェット群。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>

namespace fbzz::editor {

/// @note Project-owned MaterialConstants use reflection types and the existing eight named bindless texture slots.
bool DrawReflectedMaterialInspector(asset::MaterialAssetAuthoring& material,
    const renderer::ShaderDescriptor* descriptor, const std::string& projectRoot);

/// @note Preserve the legacy reflected Water layout; project-owned layouts expose their appearance before shared services.
bool DrawWaterMaterialAuthoring(asset::MaterialAssetAuthoring& material,
    const renderer::ShaderDescriptor* descriptor, const std::string& projectRoot);

/// @note 型・行列・配列の要素数を Reflection に合わせる。未対応型は理由を表示し編集しない。
bool DrawReflectedMaterialParam(asset::MaterialAssetAuthoring& material, const renderer::ShaderVarDesc& variable);

/// @note 完全被覆の滑らかな固体へ切り替え、光学設定は shader reflection と独立に保存する。
bool DrawSolidDielectricMaterialInspector(asset::MaterialAssetAuthoring& material);

/// @brief Drag & Drop 対応の texture path 入力欄を、左にサムネイルを添えて描画する。
/// @return パスが変わったら true (サムネイルへのドロップを含む)。
/// @note Terrain / Water / 汎用 Material で同じ UI とパス正規化を使い、パネル間の挙動差をなくす。
/// @see widgets::TextureThumbnail
bool DrawMaterialTextureField(asset::MaterialAssetAuthoring& mat, const char* label, const char* key);

/// @brief Terrain レイヤー用 .mat の変更種別。
/// @note textureDirty はテクスチャパス変更 (レイヤーテクスチャ再ロードが要る)、
/// @note paramDirty は float / bool パラメータ変更 (CB 再構築のみでスプラットマップの
/// @note 再アップロードは不要)。
struct TerrainLayerDirtyFlags { bool textureDirty = false; bool paramDirty = false; };

/// @brief Terrain レイヤー用 .mat のパラメータを編集する。
/// @note プレフィックスなし ("diffuse", "tilingX" 等) で各レイヤーが独立した fzmat を持つ設計に対応。
/// @note 汎用 float 一覧では各値の意味を追いにくいため、用途単位で整理する。
TerrainLayerDirtyFlags DrawTerrainLayerMaterialInspector(asset::MaterialAssetAuthoring& mat);

/// @brief Water 専用 .mat (水の種類) を編集する。
/// @return いずれかの値が変わったら true。
/// @note 見た目 (色・反射・さざ波・泡) に加え、Gerstner 波・環境風への反応・水流もここで持つ。
/// @note 水面は調整頻度が高く、汎用 float 一覧では «どれが波でどれが色か» を追えない。
bool DrawWaterMaterialInspector(asset::MaterialAssetAuthoring& mat);

} /// @note namespace fbzz::editor
