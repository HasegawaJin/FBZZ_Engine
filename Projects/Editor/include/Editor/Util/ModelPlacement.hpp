/// @file    ModelPlacement.hpp
/// @brief   .fbx アセットを Scene 上の GameObject 階層として配置するユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::asset { struct ModelAsset; }

namespace fbzz::editor {

struct EditorContext;

/// モデルの meshIndex 番目の submesh に、インポーターが書き出した .mat の論理パスを返す。
/// 見つからなければ空文字列 (配置時はここで Fallback.mat へ落ちる)。
/// WHY 公開するか: Animation Preview が「シーンへ置いたときと同じ材質」で描くため。
///     命名規則 (サニタイズ・重複番号・Library/Baked 解決) を写すと片方だけずれる。
std::string FindImportedMaterialPath(const std::string& modelPath,
                                     const asset::ModelAsset* modelAsset,
                                     int meshIndex);

// .fbx を読み込み、メッシュ種別に応じた MeshRenderer / SkinnedMeshRenderer と
// MaterialComponent を持つ GO 階層を生成する。
// WHY: AssetBrowser / Viewport / Hierarchy の D&D 配置経路で同じ生成規則を共有し、
//      .asset 廃止後も「モデルを置く」という操作を UI ごとに分岐させないため。
scene::EntityID SpawnModelAssetHierarchy(EditorContext& ctx,
                                         const std::string& modelPath,
                                         const math::Vector3* worldPosition = nullptr,
                                         scene::EntityID parentId = scene::EntityID::INVALID);

// .fbx を配置可能な正規モデルパスへ解決する (Unity 流「FBX だけで完結」の入口)。
// インポート済みなら既存の論理パスを返し、未インポートならデフォルト設定で
// その場で自動インポートしてから返す。失敗時は空文字列。
// WHY: Hierarchy / Viewport へ FBX を直接 D&D したとき、ユーザーに
//      「先にインポート」という手順を要求しないため。
std::string ResolveOrImportFbxModel(const std::string& fbxAssetPath);

} // namespace fbzz::editor
