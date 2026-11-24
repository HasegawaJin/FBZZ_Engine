/// @file    RenderSceneSkinningGeometry.hpp
/// @brief   スキン姿勢の抽出に使う内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "RenderSceneExtraction.hpp"

namespace fbzz::scene {
struct SkinnedMeshRenderer;

namespace render_extraction_detail {

/// @pre Ray 用 LOD の選択後に呼ぶ。現在の姿勢がなければ両方の描画から除外する。
void ExtractSkinnedPose(GameObject& owner, const SkinnedMeshRenderer& skin,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette,
    const renderer::ResourceManager* resources, renderer::RenderObject& object);

}
}
