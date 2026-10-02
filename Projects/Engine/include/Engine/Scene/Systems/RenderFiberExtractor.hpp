/// @file    RenderFiberExtractor.hpp
/// @brief   繊維表面・骨・接触・場の描画入力の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { class ResourceManager; struct RenderScene; }
namespace fbzz::scene {
class Scene;
struct RenderPassContext;
/// @note Append after Raster extraction. CPU shape counts and existing terrain draws prove source presence without camera, distance, or renderer LOD rejection; no geometry is uploaded.
void ExtractRayFiberSources(Scene& scene, renderer::ResourceManager& resources, renderer::RenderScene& output);
void ExtractRenderFibers(RenderPassContext& ctx, renderer::RenderScene& output);
}
