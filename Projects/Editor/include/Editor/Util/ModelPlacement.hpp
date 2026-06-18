// FBZZ Engine
// ModelPlacement.hpp | fbzz::editor
// .fzasset アセットを Scene 上の GameObject 階層として配置するユーティリティ
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::editor {

struct EditorContext;

// .fzasset を読み込み、描画に必要な SkinnedMeshRenderer / MaterialComponent を持つ GO 階層を生成する。
// WHY: AssetBrowser / Viewport / Hierarchy の D&D 配置経路で同じ生成規則を共有し、
//      .asset 廃止後も「モデルを置く」という操作を UI ごとに分岐させないため。
scene::EntityID SpawnModelAssetHierarchy(EditorContext& ctx,
                                         const std::string& modelPath,
                                         const math::Vector3* worldPosition = nullptr,
                                         scene::EntityID parentId = scene::EntityID::INVALID);

} // namespace fbzz::editor
