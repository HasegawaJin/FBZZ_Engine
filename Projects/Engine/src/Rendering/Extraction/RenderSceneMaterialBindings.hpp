/// @file    RenderSceneMaterialBindings.hpp
/// @brief   同期済み材質から描画入力を生成する抽出内部の境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

namespace fbzz::renderer {
class Material;
struct RenderMaterial;
class ResourceManager;
}

namespace fbzz::scene {
struct MaterialSlot;

namespace render_extraction_detail {
class RenderMaterialResolver;

void CopyResolvedMaterial(renderer::RenderMaterial& output, const renderer::Material* material,
    renderer::ResourceManager& resources, const MaterialSlot& slot, RenderMaterialResolver& materialResolver);

/// @param[in] currentDeformationVerified スキンドの場合に、現在の変形済み頂点バッファを検証できたか。
void ExtractRayMaterialInputs(renderer::RenderMaterial& material, const renderer::Material* source,
    const MaterialSlot& slot, renderer::ResourceManager& resources, bool currentDeformationVerified);

}
}
