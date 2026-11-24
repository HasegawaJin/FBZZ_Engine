/// @file    RenderDecalMaterials.hpp
/// @brief   デカールの材質解決と描画資源を設定する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Effects/Decals/RenderDecalInput.hpp>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene { struct DecalComponent; }

namespace fbzz::scene::decal_extraction_detail {

struct DecalMaterialBinding;
void BeginDecalMaterialPass();
DecalMaterialBinding* ResolveDecalMaterial(renderer::ResourceManager& resources,
                                           const DecalComponent& decal);
void BindCustomDecalMaterial(renderer::ResourceManager& resources, DecalMaterialBinding& binding,
    const DecalComponent& decal, renderer::RenderDecalInput& input);
void BindBuiltInDecalMaterial(renderer::ResourceManager& resources, const renderer::RenderPassHandles& handles,
    const DecalComponent& decal, renderer::RenderDecalInput& input);

} /// @note namespace fbzz::scene::decal_extraction_detail
