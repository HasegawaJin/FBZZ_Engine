/// @file    RenderMeshTrailExtraction.hpp
/// @brief   履歴サンプルと材質をメッシュ残像の描画へ渡す。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Graphics/Effects/RenderMeshTrailInput.hpp>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene { class Scene; class GameObject; }

namespace fbzz::scene::mesh_trail_extraction_detail {

struct TrailMaterialBinding;
struct TrailMaterialCache;

TrailMaterialCache& BeginMeshTrailMaterialPass(Scene& scene, renderer::ResourceManager& resources);
const TrailMaterialBinding* ResolveTrailMaterialBinding(TrailMaterialCache& cache, renderer::ResourceManager& resources,
    const std::string& materialPath);
void PrepareMeshTrailAppearance(MeshTrailComponent& trail, renderer::ResourceManager& resources);
void ApplyMeshTrailMaterial(const TrailMaterialBinding* material, bool skinned, renderer::RenderMeshTrailInput& input);
void PrepareMeshTrailSampleStorage(MeshTrailComponent& trail);
void UpdateMeshTrailSamples(GameObject& owner, MeshTrailComponent& trail, float currentTime);
void ClearMeshTrailSamples(MeshTrailComponent& trail);
MeshTrailSample& ReadMeshTrailSample(MeshTrailComponent& trail, int logicalIndex);

} /// @note namespace fbzz::scene::mesh_trail_extraction_detail
